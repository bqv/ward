/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/turn.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/judge.h"
#include "core/proc.h"
#include "core/worker.h"
#include "core/util.h"

static int policy_check(wd_turn *turn, const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len, char **err);
static uint8_t *encode_keep(cbor_item_t *item, size_t *len);
static int invoke(wd_instance *instance, cbor_item_t *payload,
                  cbor_item_t **result, char **err);
#include "net/http.h"
#include "proto/codec.h"

#define WD_TURN_DEPTH 8
#define WD_TURN_FUEL 50000000ull
#define WD_TURN_MEMORY (16 * 1024 * 1024)

struct wd_turn {
  wd_engine *engine;
  char *endpoint;
  char *root;
  wd_module *driver_module;
  wd_module *provider_module;
  wd_module *engine_module;
  wd_module *tools_module;
  wd_module *policy_module;
  wd_module *skills_module;
  wd_instance *driver;
  wd_instance *provider;
  wd_instance *eng;
  wd_instance *tools;
  wd_worker *worker;
  wd_worker *tool_worker;
  wd_http_job *http_job;
  uint64_t id;
  cbor_item_t *history;
  char *context;
  wd_instance *judge;
  wd_module *judge_module;
  wd_judge *judge_cache;
  char *worker_root;
  wd_instance *policy;
  wd_instance *skills;
  int pending_kind;
  cbor_item_t *parked;
  uint64_t park_call;
  uint64_t park_epoch;
  char *park_reason;
  uint64_t epoch;
};

static const wd_limits TURN_LIMITS = {.fuel = WD_TURN_FUEL,
                                      .max_memory = WD_TURN_MEMORY,
                                      .max_instances = 6};

#define WD_TOOL_MAX (64u * 1024u)

static wd_worker *worker_for(wd_turn *turn);
static int provider_phase(wd_turn *turn, cbor_item_t *payload,
                          cbor_item_t **reply, char **err);
static int exec_reply(wd_proc_result *result, cbor_item_t **out);

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

static wd_module *load_module(wd_engine *engine, const char *dir,
                              const char *file, const wd_module_spec *spec,
                              char **err) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, file);
  size_t len = 0;
  uint8_t *bytes = read_file(path, &len);
  if (bytes == NULL) {
    *err = strdup("cannot read module");
    return NULL;
  }
  wd_module *module = wd_module_load(engine, spec, bytes, len, err);
  free(bytes);
  return module;
}

static int net_request(wd_turn *turn, const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len, char **err) {
  if (turn->endpoint == NULL) {
    *err = strdup("no endpoint configured");
    return -1;
  }
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    *err = strdup("net.request: parameters are not cbor");
    return -1;
  }
  char *path = wd_text_copy(wd_get(request, "path"));
  char *body = wd_text_copy(wd_get(request, "body"));
  wd_free(request);
  char url[512];
  snprintf(url, sizeof(url), "%s%s", turn->endpoint, path != NULL ? path : "");
  free(path);
  if (!wd_http_loopback(url)) {
    free(body);
    *err = strdup("net.request: the endpoint is not loopback");
    return -1;
  }
  wd_http_response response;
  int rc = wd_http_request(url, "POST", body != NULL ? body : "", 300000,
                           4u << 20, &response, err);
  free(body);
  if (rc != 0) return -1;
  size_t total = 4 + response.len;
  uint8_t *bytes = malloc(total);
  if (bytes == NULL) {
    wd_http_response_free(&response);
    *err = strdup("net.request: out of memory");
    return -1;
  }
  bytes[0] = (uint8_t)(response.status & 0xff);
  bytes[1] = (uint8_t)((response.status >> 8) & 0xff);
  bytes[2] = (uint8_t)((response.status >> 16) & 0xff);
  bytes[3] = (uint8_t)((response.status >> 24) & 0xff);
  if (response.len > 0) memcpy(bytes + 4, response.body, response.len);
  wd_http_response_free(&response);
  *out = bytes;
  *out_len = total;
  return 0;
}

static bool within_root(const char *root, const char *path) {
  size_t n = strlen(root);
  return strncmp(root, path, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static int tool_reply(cbor_item_t *reply, uint8_t **out, size_t *out_len) {
  size_t len = 0;
  uint8_t *bytes = wd_encode(reply, &len);
  if (bytes == NULL) return -1;
  *out = bytes;
  *out_len = len;
  return 0;
}

static int tool_fail(const char *message, uint8_t **out, size_t *out_len) {
  if (out == NULL) return -1;
  cbor_item_t *reply = wd_map(2);
  wd_put(reply, "ok", cbor_build_bool(false));
  wd_put(reply, "error", wd_text(message));
  return tool_reply(reply, out, out_len);
}

static int tool_ok_text(const uint8_t *text, size_t len, uint8_t **out,
                        size_t *out_len) {
  cbor_item_t *reply = wd_map(2);
  wd_put(reply, "ok", cbor_build_bool(true));
  wd_put(reply, "text", wd_text_len((const char *)text, len));
  return tool_reply(reply, out, out_len);
}

static int tool_ok_count(size_t count, uint8_t **out, size_t *out_len) {
  char text[64];
  int n = snprintf(text, sizeof(text), "wrote %zu bytes", count);
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(true));
  wd_put(reply, "written", wd_uint(count));
  wd_put(reply, "text", wd_text_len(text, n > 0 ? (size_t)n : 0));
  return tool_reply(reply, out, out_len);
}

static int resolve_path(wd_turn *turn, const char *rel, bool parent, char *out,
                        size_t cap) {
  if (turn->root == NULL || rel == NULL || rel[0] == '\0' || rel[0] == '/')
    return -1;
  if (strstr(rel, "..") != NULL) return -1;
  char joined[PATH_MAX];
  int n = snprintf(joined, sizeof(joined), "%s/%s", turn->root, rel);
  if (n < 0 || (size_t)n >= sizeof(joined)) return -1;
  char resolved[PATH_MAX];
  if (!parent) {
    if (realpath(joined, resolved) == NULL) return -1;
  } else {
    char *slash = strrchr(joined, '/');
    if (slash == NULL || slash == joined) return -1;
    *slash = '\0';
    char dir[PATH_MAX];
    if (realpath(joined, dir) == NULL) return -1;
    n = snprintf(resolved, sizeof(resolved), "%s/%s", dir, slash + 1);
    if (n < 0 || (size_t)n >= sizeof(resolved)) return -1;
  }
  if (!within_root(turn->root, resolved)) return -1;
  if (strlen(resolved) >= cap) return -1;
  snprintf(out, cap, "%s", resolved);
  return 0;
}

static int fs_read(wd_turn *turn, const uint8_t *in, size_t in_len,
                   uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    *out = NULL;
    cbor_item_t *reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("fs.read: parameters are not cbor"));
    return tool_reply(reply, out, out_len);
  }
  char *rel = wd_text_copy(wd_get(request, "path"));
  wd_free(request);
  char path[PATH_MAX];
  int rc = resolve_path(turn, rel, false, path, sizeof(path));
  free(rel);
  if (rc != 0)
    return tool_fail("fs.read: the path is outside the tool root", out, out_len);
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return tool_fail("fs.read: cannot open the file", out, out_len);
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size > (off_t)WD_TOOL_MAX) {
    close(fd);
    return tool_fail("fs.read: not a regular file under the size limit", out,
                     out_len);
  }
  size_t len = (size_t)st.st_size;
  uint8_t *buf = malloc(len + 1);
  if (buf == NULL) {
    close(fd);
    return tool_fail("fs.read: out of memory", out, out_len);
  }
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, buf + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      close(fd);
      return tool_fail("fs.read: cannot read the file", out, out_len);
    }
    if (n == 0) break;
    got += (size_t)n;
  }
  close(fd);
  if (memchr(buf, '\0', got) != NULL) {
    free(buf);
    return tool_fail("fs.read: the file is binary", out, out_len);
  }
  rc = tool_ok_text(buf, got, out, out_len);
  free(buf);
  return rc;
}

static int clock_now(uint8_t **out, size_t *out_len) {
  time_t now = time(NULL);
  struct tm tm;
  char text[64];
  text[0] = '\0';
  if (localtime_r(&now, &tm) != NULL)
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z", &tm);
  cbor_item_t *reply = wd_map(2);
  wd_put(reply, "unix_ms", wd_uint((uint64_t)now * 1000u));
  wd_put(reply, "text", wd_text(text));
  return tool_reply(reply, out, out_len);
}

static const char *recall_binary(void) {
  const char *override = getenv("WARD_RECALL_BIN");
  if (override != NULL && override[0] != '\0') return override;
  static char path[PATH_MAX];
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return NULL;
  snprintf(path, sizeof(path), "%s/bin/dsh-recall", home);
  return path;
}

static int recall_search_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                             uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL)
    return tool_fail("recall.search: parameters are not cbor", out, out_len);
  char *query = wd_text_copy(wd_get(request, "query"));
  uint64_t k = 5;
  wd_uint_of(wd_get(request, "k"), &k);
  wd_free(request);
  if (query == NULL || query[0] == '\0' || strlen(query) > 512) {
    free(query);
    return tool_fail("recall.search: the query is missing or too long", out,
                     out_len);
  }
  if (k == 0 || k > 20) k = 5;
  char count[8];
  snprintf(count, sizeof(count), "%llu", (unsigned long long)k);
  const char *binary = recall_binary();
  if (binary == NULL) {
    free(query);
    return tool_fail("recall.search: no index is configured", out, out_len);
  }
  const char *argv[6];
  size_t argc = 0;
  argv[argc++] = binary;
  argv[argc++] = "search";
  argv[argc++] = query;
  argv[argc++] = "-k";
  argv[argc++] = count;
  argv[argc] = NULL;
  wd_proc_result result;
  char *err = NULL;
  int rc = wd_proc_run(argv, turn->root, 20000, WD_TOOL_MAX, &result, &err);
  free(query);
  free(err);
  if (rc != 0)
    return tool_fail("recall.search: cannot run dsh-recall", out, out_len);
  int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 127;
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  if (result.len > 0)
    wd_put(reply, "text", wd_text_len((const char *)result.out, result.len));
  else
    wd_put(reply, "text", wd_text(""));
  wd_proc_result_free(&result);
  return tool_reply(reply, out, out_len);
}

static const char *mcp_binary(void) {
  const char *override = getenv("WARD_MCP_BIN");
  if (override != NULL && override[0] != '\0') return override;
  static char path[PATH_MAX];
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return NULL;
  snprintf(path, sizeof(path), "%s/bin/mcp", home);
  return path;
}

static int mcp_call_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL)
    return tool_fail("mcp.call: parameters are not cbor", out, out_len);
  char *line = wd_text_copy(wd_get(request, "request"));
  wd_free(request);
  const char *binary = mcp_binary();
  if (line == NULL || line[0] == '\0' || strlen(line) > 4096) {
    free(line);
    return tool_fail("mcp.call: the request is missing or too long", out,
                     out_len);
  }
  if (binary == NULL) {
    free(line);
    return tool_fail("mcp.call: no server is configured", out, out_len);
  }
  size_t len = strlen(line) + 1;
  char *input = malloc(len + 1);
  if (input == NULL) {
    free(line);
    return tool_fail("mcp.call: out of memory", out, out_len);
  }
  memcpy(input, line, len - 1);
  input[len - 1] = '\n';
  input[len] = '\0';
  const char *argv[] = {binary, NULL};
  wd_proc_result result;
  char *err = NULL;
  int rc = wd_proc_run_input(argv, turn->root, input, len, 20000, WD_TOOL_MAX,
                             &result, &err);
  free(input);
  free(line);
  free(err);
  if (rc != 0) return tool_fail("mcp.call: cannot start the server", out,
                                out_len);
  int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 127;
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  if (result.len > 0)
    wd_put(reply, "text", wd_text_len((const char *)result.out, result.len));
  else
    wd_put(reply, "text", wd_text(""));
  wd_proc_result_free(&result);
  return tool_reply(reply, out, out_len);
}

static const char *search_binary(void) {
  const char *override = getenv("WARD_SEARCH_BIN");
  if (override != NULL && override[0] != '\0') return override;
  static char path[PATH_MAX];
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') return "srchdsh";
  snprintf(path, sizeof(path), "%s/bin/srchdsh", home);
  return path;
}

static int web_search_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                          uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL)
    return tool_fail("web.search: parameters are not cbor", out, out_len);
  char *query = wd_text_copy(wd_get(request, "query"));
  uint64_t k = 5;
  wd_uint_of(wd_get(request, "k"), &k);
  wd_free(request);
  if (query == NULL || query[0] == '\0' || strlen(query) > 512) {
    free(query);
    return tool_fail("web.search: the query is missing or too long", out,
                     out_len);
  }
  if (k == 0 || k > 20) k = 5;
  char count[8];
  snprintf(count, sizeof(count), "%llu", (unsigned long long)k);
  const char *argv[8];
  size_t argc = 0;
  argv[argc++] = search_binary();
  argv[argc++] = "search";
  argv[argc++] = "-q";
  argv[argc++] = query;
  argv[argc++] = "-max";
  argv[argc++] = count;
  argv[argc++] = "-json";
  argv[argc] = NULL;
  wd_proc_result result;
  char *err = NULL;
  int rc = wd_proc_run(argv, turn->root, 20000, WD_TOOL_MAX, &result, &err);
  free(query);
  free(err);
  if (rc != 0) return tool_fail("web.search: cannot run srchdsh", out, out_len);
  int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 127;
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  if (result.len > 0)
    wd_put(reply, "text", wd_text_len((const char *)result.out, result.len));
  else
    wd_put(reply, "text", wd_text(""));
  wd_proc_result_free(&result);
  return tool_reply(reply, out, out_len);
}

static int python_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                      uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL)
    return tool_fail("python.run: parameters are not cbor", out, out_len);
  char *program = wd_text_copy(wd_get(request, "program"));
  wd_free(request);
  if (program == NULL || program[0] == '\0' || strlen(program) > 4096) {
    free(program);
    return tool_fail("python.run: the program is missing or too long", out,
                     out_len);
  }
  const char *argv[5];
  size_t argc = 0;
  argv[argc++] = "python3";
  argv[argc++] = "-I";
  argv[argc++] = "-c";
  argv[argc++] = program;
  argv[argc] = NULL;
  wd_proc_result result;
  char *err = NULL;
  int rc = wd_proc_run(argv, turn->root, 15000, WD_TOOL_MAX, &result, &err);
  free(program);
  free(err);
  if (rc != 0) return tool_fail("python.run: cannot start python", out, out_len);
  int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 127;
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  if (result.len > 0)
    wd_put(reply, "text", wd_text_len((const char *)result.out, result.len));
  else
    wd_put(reply, "text", wd_text(""));
  wd_proc_result_free(&result);
  return tool_reply(reply, out, out_len);
}

static int grep_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                    uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    cbor_item_t *reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("grep.run: parameters are not cbor"));
    return tool_reply(reply, out, out_len);
  }
  char *pattern = wd_text_copy(wd_get(request, "pattern"));
  char *path = wd_text_copy(wd_get(request, "path"));
  wd_free(request);
  if (pattern == NULL || pattern[0] == '\0' || strlen(pattern) > 1024) {
    free(pattern);
    free(path);
    return tool_fail("grep.run: the pattern is missing or too long", out,
                     out_len);
  }
  if (path != NULL &&
      (path[0] == '\0' || path[0] == '/' || strstr(path, "..") != NULL)) {
    free(pattern);
    free(path);
    return tool_fail("grep.run: the path is outside the tool root", out,
                     out_len);
  }
  const char *argv[9];
  size_t argc = 0;
  argv[argc++] = "rg";
  argv[argc++] = "--line-number";
  argv[argc++] = "--no-heading";
  argv[argc++] = "--color=never";
  argv[argc++] = "--max-count=200";
  argv[argc++] = "-e";
  argv[argc++] = pattern;
  if (path != NULL) argv[argc++] = path;
  argv[argc] = NULL;
  wd_proc_result result;
  char *err = NULL;
  int rc = wd_proc_run(argv, turn->root, 10000, WD_TOOL_MAX, &result, &err);
  free(pattern);
  free(path);
  if (rc != 0) {
    char message[256];
    snprintf(message, sizeof(message), "%s",
             err != NULL ? err : "grep.run: cannot run rg");
    free(err);
    return tool_fail(message, out, out_len);
  }
  int status = WIFEXITED(result.status) ? WEXITSTATUS(result.status) : 127;
  cbor_item_t *reply = wd_map(3);
  wd_put(reply, "ok", cbor_build_bool(status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  if (result.len > 0)
    wd_put(reply, "text", wd_text_len((const char *)result.out, result.len));
  else
    wd_put(reply, "text", wd_text(status == 1 ? "no matches" : ""));
  wd_proc_result_free(&result);
  return tool_reply(reply, out, out_len);
}

static int edit_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                    uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    cbor_item_t *reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("fs.edit: parameters are not cbor"));
    return tool_reply(reply, out, out_len);
  }
  char *rel = wd_text_copy(wd_get(request, "path"));
  char *find = wd_text_copy(wd_get(request, "find"));
  char *replace = wd_text_copy(wd_get(request, "replace"));
  wd_free(request);
  char path[PATH_MAX];
  int rc = resolve_path(turn, rel, false, path, sizeof(path));
  free(rel);
  if (rc != 0) {
    free(find);
    free(replace);
    return tool_fail("fs.edit: the path is outside the tool root", out,
                     out_len);
  }
  if (find == NULL || find[0] == '\0' || replace == NULL) {
    free(find);
    free(replace);
    return tool_fail("fs.edit: find and replace are required", out, out_len);
  }
  uint8_t *content = NULL;
  size_t len = 0;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    free(find);
    free(replace);
    return tool_fail("fs.edit: cannot open the file", out, out_len);
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size > (off_t)WD_TOOL_MAX) {
    close(fd);
    free(find);
    free(replace);
    return tool_fail("fs.edit: not a regular file under the size limit", out,
                     out_len);
  }
  len = (size_t)st.st_size;
  content = malloc(len + 1);
  if (content == NULL) {
    close(fd);
    free(find);
    free(replace);
    return tool_fail("fs.edit: out of memory", out, out_len);
  }
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, content + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    got += (size_t)n;
  }
  close(fd);
  content[got] = '\0';
  char *at = got == len ? strstr((char *)content, find) : NULL;
  if (at == NULL) {
    free(content);
    free(find);
    free(replace);
    return tool_fail("fs.edit: the text to find is not in the file", out,
                     out_len);
  }
  size_t before = (size_t)(at - (char *)content);
  size_t find_len = strlen(find);
  size_t replace_len = strlen(replace);
  size_t total = got - find_len + replace_len;
  if (total > WD_TOOL_MAX) {
    free(content);
    free(find);
    free(replace);
    return tool_fail("fs.edit: the file would grow past the size limit", out,
                     out_len);
  }
  uint8_t *next = malloc(total + 1);
  if (next == NULL) {
    free(content);
    free(find);
    free(replace);
    return tool_fail("fs.edit: out of memory", out, out_len);
  }
  memcpy(next, content, before);
  memcpy(next + before, replace, replace_len);
  memcpy(next + before + replace_len, content + before + find_len,
         got - before - find_len);
  next[total] = '\0';
  free(content);
  free(find);
  free(replace);
  int wfd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (wfd < 0) {
    free(next);
    return tool_fail("fs.edit: cannot rewrite the file", out, out_len);
  }
  int written = wd_write_all(wfd, next, total);
  close(wfd);
  free(next);
  if (written != 0) return tool_fail("fs.edit: cannot rewrite the file", out, out_len);
  return tool_ok_count(total, out, out_len);
}

static int exec_reply(wd_proc_result *result, cbor_item_t **out) {
  int status = WIFEXITED(result->status) ? WEXITSTATUS(result->status) : 127;
  cbor_item_t *reply = wd_map(4);
  wd_put(reply, "ok", cbor_build_bool(!result->timed_out && status == 0));
  wd_put(reply, "status", wd_uint((uint64_t)status));
  wd_put(reply, "timed_out", cbor_build_bool(result->timed_out));
  if (result->len > 0) {
    wd_put(reply, "text", wd_text_len((const char *)result->out, result->len));
  } else {
    char note[96];
    if (result->timed_out)
      snprintf(note, sizeof(note), "the command timed out");
    else
      snprintf(note, sizeof(note), "the command exited with status %d", status);
    wd_put(reply, "text", wd_text(note));
  }
  *out = reply;
  return 0;
}

static int exec_argv(cbor_item_t *args, char **copy_out, const char **argv,
                     uint64_t *timeout_out) {
  char *command = wd_text_copy(wd_get(args, "command"));
  uint64_t timeout = 10000;
  wd_uint_of(wd_get(args, "timeout_ms"), &timeout);
  if (command == NULL || command[0] == '\0') {
    free(command);
    return -1;
  }
  char *copy = strdup(command);
  free(command);
  if (copy == NULL) return -1;
  size_t argc = 0;
  for (char *tok = strtok(copy, " \t"); tok != NULL && argc < 32;
       tok = strtok(NULL, " \t"))
    argv[argc++] = tok;
  argv[argc] = NULL;
  if (argc == 0) {
    free(copy);
    return -1;
  }
  if (timeout == 0 || timeout > 600000) timeout = 600000;
  *copy_out = copy;
  *timeout_out = timeout;
  return 0;
}

static int exec_run(wd_turn *turn, const uint8_t *in, size_t in_len,
                    uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    cbor_item_t *reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("exec.run: parameters are not cbor"));
    return tool_reply(reply, out, out_len);
  }
  char *copy = NULL;
  const char *argv[33];
  uint64_t timeout = 0;
  int parsed = exec_argv(request, &copy, argv, &timeout);
  wd_free(request);
  if (parsed != 0)
    return tool_fail("exec.run: the command is missing or empty", out, out_len);
  wd_proc_result result;
  char *err = NULL;
  cbor_item_t *reply = NULL;
  int rc = 0;
  wd_worker *worker = worker_for(turn);
  rc = wd_worker_run(worker, argv, timeout, WD_TOOL_MAX, &result, &err);
  if (rc != 0) {
    free(err);
    err = NULL;
    worker = worker_for(turn);
    rc = wd_worker_run(worker, argv, timeout, WD_TOOL_MAX, &result, &err);
  }
  free(copy);
  if (rc != 0) {
    char message[256];
    snprintf(message, sizeof(message), "%s",
             err != NULL ? err : "exec.run: cannot run the command");
    free(err);
    return tool_fail(message, out, out_len);
  }
  rc = exec_reply(&result, &reply);
  wd_proc_result_free(&result);
  if (rc != 0) return tool_fail("exec.run: cannot build the reply", out, out_len);
  return tool_reply(reply, out, out_len);
}

static int fs_write(wd_turn *turn, const uint8_t *in, size_t in_len,
                    uint8_t **out, size_t *out_len) {
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    cbor_item_t *reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("fs.write: parameters are not cbor"));
    return tool_reply(reply, out, out_len);
  }
  char *rel = wd_text_copy(wd_get(request, "path"));
  char *text = wd_text_copy(wd_get(request, "text"));
  wd_free(request);
  char path[PATH_MAX];
  int rc = resolve_path(turn, rel, true, path, sizeof(path));
  free(rel);
  size_t len = text != NULL ? strlen(text) : 0;
  if (rc != 0) {
    free(text);
    return tool_fail("fs.write: the path is outside the tool root", out,
                     out_len);
  }
  if (text == NULL || len > WD_TOOL_MAX) {
    free(text);
    return tool_fail("fs.write: the text is missing or too long", out, out_len);
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
                0600);
  if (fd < 0) {
    free(text);
    return tool_fail("fs.write: cannot create the file", out, out_len);
  }
  int written = wd_write_all(fd, text, len);
  close(fd);
  free(text);
  if (written != 0) return tool_fail("fs.write: cannot write the file", out, out_len);
  return tool_ok_count(len, out, out_len);
}

typedef struct {
  wd_turn *turn;
  const char *payload;
} judge_ctx;

static int judge_ask(void *ctx, const char *tool, const uint8_t *args,
                     size_t args_len, wd_verdict *out) {
  (void)args;
  (void)args_len;
  judge_ctx *jc = ctx;
  if (jc->turn->judge == NULL) return -1;
  cbor_item_t *request = wd_map(2);
  wd_put(request, "tool", wd_text(tool));
  wd_put(request, "payload", wd_text(jc->payload));
  size_t in_len = 0;
  uint8_t *in = encode_keep(request, &in_len);
  wd_free(request);
  if (in == NULL) return -1;
  cbor_item_t *parsed = wd_decode(in, in_len);
  cbor_item_t *reply = NULL;
  int rc = parsed != NULL ? invoke(jc->turn->judge, parsed, &reply, NULL) : -1;
  wd_free(parsed);
  free(in);
  if (rc != 0 || reply == NULL) return -1;
  char *verdict = wd_text_copy(wd_get(reply, "verdict"));
  wd_free(reply);
  if (verdict == NULL) return -1;
  int result = -1;
  if (strcmp(verdict, "allow") == 0) {
    *out = WD_VERDICT_ALLOW;
    result = 0;
  } else if (strcmp(verdict, "ask") == 0) {
    *out = WD_VERDICT_ASK;
    result = 0;
  } else if (strcmp(verdict, "deny") == 0) {
    *out = WD_VERDICT_DENY;
    result = 0;
  }
  free(verdict);
  return result;
}

static int policy_check(wd_turn *turn, const uint8_t *in, size_t in_len,
                        uint8_t **out, size_t *out_len, char **err) {
  if (wd_instance_call(turn->policy, in, in_len, out, out_len, err) != 0)
    return -1;
  if (turn->judge == NULL) return 0;
  cbor_item_t *request = wd_decode(in, in_len);
  cbor_item_t *reply = wd_decode(*out, *out_len);
  if (request == NULL || reply == NULL) {
    wd_free(request);
    wd_free(reply);
    return 0;
  }
  char *tool = wd_text_copy(wd_get(request, "tool"));
  char *payload = wd_text_copy(wd_get(request, "payload"));
  char *decision = wd_text_copy(wd_get(reply, "decision"));
  if (tool != NULL && decision != NULL && strcmp(decision, "allow") == 0) {
    judge_ctx jc = {turn, payload != NULL ? payload : ""};
    bool consulted = false;
    wd_verdict merged =
        wd_judge_decide(turn->judge_cache, WD_VERDICT_ALLOW, tool,
                        (const uint8_t *)(payload != NULL ? payload : ""),
                        payload != NULL ? strlen(payload) : 0, judge_ask, &jc,
                        &consulted);
    if (merged != WD_VERDICT_ALLOW) {
      cbor_item_t *rewritten = wd_map(3);
      wd_put(rewritten, "decision", wd_text(wd_verdict_name(merged)));
      wd_put(rewritten, "judge", cbor_build_bool(true));
      wd_put(rewritten, "reason", wd_text("the judge escalated this call"));
      size_t len = 0;
      uint8_t *bytes = encode_keep(rewritten, &len);
      wd_free(rewritten);
      if (bytes != NULL) {
        free(*out);
        *out = bytes;
        *out_len = len;
      }
    }
  }
  free(tool);
  free(payload);
  free(decision);
  wd_free(request);
  wd_free(reply);
  return 0;
}

static int turn_capability(void *ctx, const char *capability,
                           const uint8_t *in, size_t in_len, uint8_t **out,
                           size_t *out_len, char **err) {
  wd_turn *turn = ctx;
  if (strcmp(capability, "engine.call") == 0)
    return wd_instance_call(turn->eng, in, in_len, out, out_len, err);
  if (strcmp(capability, "net.request") == 0)
    return net_request(turn, in, in_len, out, out_len, err);
  if (strcmp(capability, "fs.read") == 0)
    return fs_read(turn, in, in_len, out, out_len);
  if (strcmp(capability, "fs.write") == 0)
    return fs_write(turn, in, in_len, out, out_len);
  if (strcmp(capability, "clock.now") == 0)
    return clock_now(out, out_len);
  if (strcmp(capability, "grep.run") == 0)
    return grep_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "fs.edit") == 0)
    return edit_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "exec.run") == 0)
    return exec_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "plan.run") == 0) {
    cbor_item_t *reply = wd_map(1);
    wd_put(reply, "text", wd_text("the plan was approved"));
    size_t need = cbor_serialized_size(reply);
    uint8_t *buf = malloc(need);
    if (buf == NULL) {
      wd_free(reply);
      *err = strdup("out of memory");
      return -1;
    }
    size_t written = cbor_serialize(reply, buf, need);
    wd_free(reply);
    if (written == 0) {
      free(buf);
      *err = strdup("cannot encode the plan reply");
      return -1;
    }
    *out = buf;
    *out_len = written;
    return 0;
  }
  if (strcmp(capability, "python.run") == 0)
    return python_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "web.search") == 0)
    return web_search_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "recall.search") == 0)
    return recall_search_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "mcp.call") == 0)
    return mcp_call_run(turn, in, in_len, out, out_len);
  if (strcmp(capability, "skills.expand") == 0)
    return wd_instance_call(turn->skills, in, in_len, out, out_len, err);
  if (strcmp(capability, "policy.check") == 0)
    return policy_check(turn, in, in_len, out, out_len, err);
  *err = strdup("unknown capability");
  return -1;
}

wd_turn *wd_turn_new(wd_engine *engine, const char *module_dir, const char *root,
                     char **err) {
  wd_turn *turn = calloc(1, sizeof(*turn));
  if (turn == NULL) {
    *err = strdup("out of memory");
    return NULL;
  }
  turn->engine = engine;
  const char *endpoint = getenv("WARD_ENDPOINT");
  if (endpoint != NULL) turn->endpoint = strdup(endpoint);
  if (root != NULL) {
    char resolved[PATH_MAX];
    turn->root = realpath(root, resolved) != NULL ? strdup(resolved)
                                                  : strdup(root);
  }
  static const char *const driver_caps[] = {"engine.call", "policy.check",
                                            "clock.now", "skills.expand"};
  static const char *const provider_caps[] = {"net.request"};
  static const char *const no_caps[] = {NULL};
  static const char *const skills_caps[] = {"web.search", "mcp.call",
                                            "recall.search"};
  wd_module_spec engine_spec = {"engine", no_caps, 0, true};
  wd_module_spec driver_spec = {"driver", driver_caps, 3, false};
  wd_module_spec policy_spec = {"policy", no_caps, 0, true};
  wd_module_spec skills_spec = {"skills", skills_caps, 3, false};
  wd_module_spec provider_spec = {"provider", provider_caps, 1, false};
  wd_module_spec tools_spec = {"tools", no_caps, 0, true};

  turn->engine_module = load_module(engine, module_dir, "engine.wasm",
                                    &engine_spec, err);
  if (turn->engine_module == NULL) goto fail;
  turn->driver_module = load_module(engine, module_dir, "driver.wasm",
                                    &driver_spec, err);
  if (turn->driver_module == NULL) goto fail;
  turn->provider_module = load_module(engine, module_dir, "provider.wasm",
                                      &provider_spec, err);
  if (turn->provider_module == NULL) goto fail;
  turn->tools_module = load_module(engine, module_dir, "tools.wasm",
                                   &tools_spec, err);
  if (turn->tools_module == NULL) goto fail;
  turn->policy_module = load_module(engine, module_dir, "policy.wasm",
                                    &policy_spec, err);
  if (turn->policy_module == NULL) goto fail;
  turn->skills_module = load_module(engine, module_dir, "skills.wasm",
                                    &skills_spec, err);
  if (turn->skills_module == NULL) goto fail;
  static const char *const judge_caps[] = {NULL};
  wd_module_spec judge_spec = {"judge", judge_caps, 0, true};
  turn->judge_module = load_module(engine, module_dir, "judge.wasm",
                                   &judge_spec, err);
  if (turn->judge_module == NULL) goto fail;

  turn->eng = wd_instance_new(turn->engine_module, no_caps, 0, &TURN_LIMITS,
                              NULL, NULL, err);
  if (turn->eng == NULL) goto fail;
  turn->provider = wd_instance_new(turn->provider_module, provider_caps, 1,
                                   &TURN_LIMITS, turn_capability, turn, err);
  if (turn->provider == NULL) goto fail;
  turn->driver = wd_instance_new(turn->driver_module, driver_caps, 4,
                                 &TURN_LIMITS, turn_capability, turn, err);
  if (turn->driver == NULL) goto fail;
  turn->tools = wd_instance_new(turn->tools_module, no_caps, 0, &TURN_LIMITS,
                                turn_capability, turn, err);
  if (turn->tools == NULL) goto fail;
  turn->policy = wd_instance_new(turn->policy_module, no_caps, 0, &TURN_LIMITS,
                                 turn_capability, turn, err);
  if (turn->policy == NULL) goto fail;
  turn->skills = wd_instance_new(turn->skills_module, skills_caps, 3,
                                 &TURN_LIMITS, turn_capability, turn, err);
  if (turn->skills == NULL) goto fail;
  turn->judge = wd_instance_new(turn->judge_module, no_caps, 0, &TURN_LIMITS,
                                NULL, NULL, err);
  if (turn->judge == NULL) goto fail;
  turn->judge_cache = wd_judge_new(64);
  if (turn->judge_cache == NULL) goto fail;
  return turn;

fail:
  wd_turn_free(turn);
  return NULL;
}

void wd_turn_set_history(wd_turn *turn, cbor_item_t *history) {
  if (turn == NULL) return;
  if (turn->history != NULL) wd_free(turn->history);
  turn->history = history;
}

void wd_turn_set_root(wd_turn *turn, const char *root) {
  if (turn == NULL || root == NULL) return;
  char resolved[PATH_MAX];
  char *next = realpath(root, resolved) != NULL ? strdup(resolved) : strdup(root);
  if (next == NULL) return;
  free(turn->root);
  turn->root = next;
}

void wd_turn_free(wd_turn *turn) {
  if (turn == NULL) return;
  if (turn->history != NULL) wd_free(turn->history);
  free(turn->context);
  wd_http_job_free(turn->http_job);
  wd_worker_free(turn->worker);
  free(turn->worker_root);
  wd_instance_free(turn->driver);
  wd_instance_free(turn->provider);
  wd_instance_free(turn->tools);
  wd_instance_free(turn->policy);
  wd_instance_free(turn->skills);
  wd_module_free(turn->skills_module);
  wd_instance_free(turn->judge);
  wd_module_free(turn->judge_module);
  wd_judge_free(turn->judge_cache);
  wd_instance_free(turn->eng);
  wd_module_free(turn->driver_module);
  wd_module_free(turn->provider_module);
  wd_module_free(turn->tools_module);
  wd_module_free(turn->policy_module);
  wd_module_free(turn->engine_module);
  if (turn->parked != NULL) wd_free(turn->parked);
  free(turn->park_reason);
  free(turn->root);
  free(turn->endpoint);
  free(turn);
}

static wd_worker *worker_for(wd_turn *turn) {
  if (turn->worker != NULL && wd_worker_alive(turn->worker) &&
      turn->worker_root != NULL && turn->root != NULL &&
      strcmp(turn->worker_root, turn->root) == 0)
    return turn->worker;
  if (turn->worker != NULL && turn->tool_worker == turn->worker)
    return turn->worker;
  wd_http_job_free(turn->http_job);
  wd_worker_free(turn->worker);
  turn->worker = NULL;
  free(turn->worker_root);
  turn->worker_root = NULL;
  char *err = NULL;
  turn->worker = wd_worker_new(turn->root, &err);
  free(err);
  if (turn->worker != NULL && turn->root != NULL)
    turn->worker_root = strdup(turn->root);
  return turn->worker;
}

static wd_instance *instance_for(wd_turn *turn, const char *module,
                                 const char *name) {
  if (strcmp(module, "provider") == 0 && strcmp(name, "chat") == 0)
    return turn->provider;
  if (strcmp(module, "tools") == 0 && name != NULL) return turn->tools;
  return NULL;
}

static uint8_t *encode_keep(cbor_item_t *item, size_t *len) {
  size_t need = cbor_serialized_size(item);
  if (need == 0) return NULL;
  uint8_t *buf = malloc(need);
  if (buf == NULL) return NULL;
  size_t written = cbor_serialize(item, buf, need);
  if (written == 0) {
    free(buf);
    return NULL;
  }
  *len = written;
  return buf;
}

static int invoke(wd_instance *instance, cbor_item_t *payload,
                  cbor_item_t **result, char **err) {
  size_t len = 0;
  uint8_t *bytes = encode_keep(payload, &len);
  if (bytes == NULL) {
    *err = strdup("cannot encode a module payload");
    return -1;
  }
  uint8_t *out = NULL;
  size_t out_len = 0;
  int rc = wd_instance_call(instance, bytes, len, &out, &out_len, err);
  free(bytes);
  if (rc != 0) return -1;
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  if (reply == NULL) {
    *err = strdup("module returned invalid CBOR");
    return -1;
  }
  *result = reply;
  return 0;
}

static int provider_phase(wd_turn *turn, cbor_item_t *payload,
                          cbor_item_t **reply, char **err) {
  return invoke(turn->provider, payload, reply, err);
}

static int net_submit(wd_turn *turn, cbor_item_t *reply, char **err) {
  cbor_item_t *request = wd_get(reply, "request");
  char *path = wd_text_copy(wd_get(request, "path"));
  char *body = wd_text_copy(wd_get(request, "body"));
  if (turn->endpoint == NULL || path == NULL) {
    free(path);
    free(body);
    *err = strdup("the provider asked for no endpoint");
    return -1;
  }
  char url[512];
  snprintf(url, sizeof(url), "%s%s", turn->endpoint, path);
  free(path);
  if (!wd_http_loopback(url)) {
    free(body);
    *err = strdup("net.request: the endpoint is not loopback");
    return -1;
  }
  wd_http_job_free(turn->http_job);
  turn->http_job =
      wd_http_start(url, "POST", body != NULL ? body : "", 300000, 4u << 20, err);
  free(body);
  if (turn->http_job == NULL) return -1;
  turn->pending_kind = 3;
  return 1;
}

static int exec_submit(wd_turn *turn, cbor_item_t *args, char **err) {
  char *copy = NULL;
  const char *argv[33];
  uint64_t timeout = 0;
  if (exec_argv(args, &copy, argv, &timeout) != 0) {
    *err = strdup("exec.run: the command is missing or empty");
    return -1;
  }
  wd_worker *worker = worker_for(turn);
  int rc = wd_worker_submit(worker, argv, timeout, WD_TOOL_MAX, err);
  if (rc != 0) {
    free(err != NULL ? *err : NULL);
    if (err != NULL) *err = NULL;
    worker = worker_for(turn);
    rc = wd_worker_submit(worker, argv, timeout, WD_TOOL_MAX, err);
  }
  free(copy);
  if (rc != 0) return -1;
  turn->tool_worker = worker;
  turn->pending_kind = 2;
  return 1;
}

static int perform_capability(wd_turn *turn, const char *capability,
                              cbor_item_t *args, cbor_item_t **result,
                              char **err) {
  if (strcmp(capability, "exec.run") == 0)
    return exec_submit(turn, args, err);
  size_t len = 0;
  uint8_t *bytes = encode_keep(args, &len);
  if (bytes == NULL) {
    *err = strdup("cannot encode the capability arguments");
    return -1;
  }
  uint8_t *out = NULL;
  size_t out_len = 0;
  int rc = turn_capability(turn, capability, bytes, len, &out, &out_len, err);
  free(bytes);
  if (rc != 0) return -1;
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  if (reply == NULL) {
    *err = strdup("the capability returned invalid CBOR");
    return -1;
  }
  *result = reply;
  return 0;
}

static int process_action(wd_turn *turn, cbor_item_t *action, wd_turn_emit emit,
                          void *ctx, cbor_item_t **next, bool *pending,
                          bool approved, char **err) {
  char *kind = wd_text_copy(wd_get(action, "kind"));
  if (kind == NULL) {
    *err = strdup("action has no kind");
    return -1;
  }
  int rc = 0;
  if (strcmp(kind, "emit") == 0) {
    char *event = wd_text_copy(wd_get(action, "event"));
    if (event == NULL) {
      *err = strdup("emit action has no event");
      rc = -1;
    } else {
      cbor_item_t *payload = wd_get(action, "payload");
      if (strcmp(event, "turn.start") == 0 ||
          strcmp(event, "turn.end") == 0) {
        size_t extra = cbor_isa_map(payload) ? cbor_map_size(payload) : 0;
        cbor_item_t *stamped = wd_map(extra + 1);
        wd_put(stamped, "turn", wd_uint(turn->id));
        if (cbor_isa_map(payload)) {
          struct cbor_pair *pairs = cbor_map_handle(payload);
          for (size_t i = 0; i < extra; i++) {
            char *key = wd_text_copy(pairs[i].key);
            if (key == NULL) continue;
            cbor_item_t *value = pairs[i].value;
            if (cbor_incref(value)) wd_put(stamped, key, value);
            free(key);
          }
        }
        rc = emit(ctx, event, stamped, err);
        wd_free(stamped);
      } else {
        rc = emit(ctx, event, payload, err);
      }
      free(event);
    }
  } else if (strcmp(kind, "invoke") == 0) {
    char *module = wd_text_copy(wd_get(action, "module"));
    char *name = wd_text_copy(wd_get(action, "name"));
    cbor_item_t *payload = wd_get(action, "payload");
    bool approve = false;
    wd_bool_of(wd_get(action, "approve"), &approve);
    if (approve && !approved) {
      char *reason = wd_text_copy(wd_get(action, "reason"));
      uint64_t call = 0;
      wd_uint_of(wd_get(action, "call"), &call);
      turn->epoch++;
      turn->park_call = call;
      turn->park_epoch = turn->epoch;
      free(turn->park_reason);
      turn->park_reason =
          reason != NULL ? reason : strdup("approval required");
      if (turn->parked != NULL) wd_free(turn->parked);
      turn->parked = action;
      if (!cbor_incref(action)) {
        turn->parked = NULL;
        *err = strdup("cannot hold the parked action");
        rc = -1;
      } else {
        cbor_item_t *notice = wd_map(3);
        wd_put(notice, "call", wd_uint(turn->park_call));
        wd_put(notice, "epoch", wd_uint(turn->park_epoch));
        wd_put(notice, "reason", wd_text(turn->park_reason));
        rc = emit(ctx, "approval.requested", notice, err);
        wd_free(notice);
        if (rc == 0) {
          turn->pending_kind = 1;
          *pending = true;
        }
      }
      free(module);
      free(name);
      free(kind);
      return rc;
    }
    wd_instance *instance =
        module != NULL && name != NULL ? instance_for(turn, module, name) : NULL;
    if (instance == NULL || payload == NULL) {
      *err = strdup("no such module action");
      rc = -1;
    } else if (instance == turn->provider) {
      cbor_item_t *request = NULL;
      if (turn->history != NULL) {
        char *text = wd_text_copy(wd_get(payload, "prompt"));
        cbor_item_t *merged = wd_map(2);
        wd_put(merged, "prompt", wd_text(text != NULL ? text : ""));
        free(text);
        cbor_item_t *shared = turn->history;
        if (cbor_incref(shared)) wd_put(merged, "history", shared);
        rc = provider_phase(turn, merged, &request, err);
        wd_free(merged);
      } else {
        rc = provider_phase(turn, payload, &request, err);
      }
      if (rc == 0) {
        int parked = net_submit(turn, request, err);
        wd_free(request);
        if (parked == 1) {
          *pending = true;
          rc = 0;
        } else if (parked != 0) {
          rc = -1;
        }
      }
    } else if (instance == turn->tools) {
      cbor_item_t *request = NULL;
      rc = invoke(instance, payload, &request, err);
      if (rc == 0) {
        char *capability = wd_text_copy(wd_get(request, "call"));
        cbor_item_t *args = wd_get(request, "payload");
        cbor_item_t *result = NULL;
        if (capability == NULL || args == NULL) {
          *err = strdup("the tools module asked for nothing");
          rc = -1;
        } else {
          int parked = perform_capability(turn, capability, args, &result, err);
          if (parked == 1) {
            *pending = true;
            rc = 0;
          } else if (parked != 0) {
            rc = -1;
          }
        }
        free(capability);
        wd_free(request);
        if (rc == 0 && !*pending) {
          cbor_item_t *message = wd_map(2);
          wd_put(message, "kind", wd_text("result"));
          wd_put(message, "payload", result);
          *next = message;
        }
      }
    } else {
      cbor_item_t *result = NULL;
      rc = invoke(instance, payload, &result, err);
      if (rc == 0) {
        cbor_item_t *message = wd_map(2);
        wd_put(message, "kind", wd_text("result"));
        wd_put(message, "payload", result);
        *next = message;
      }
    }
    free(module);
    free(name);
  } else {
    *err = strdup("unknown action kind");
    rc = -1;
  }
  free(kind);
  return rc;
}

static int drive(wd_turn *turn, cbor_item_t *message, wd_turn_emit emit,
                 void *ctx, char **err) {
  for (int depth = 0; depth < WD_TURN_DEPTH; depth++) {
    if (message == NULL) return WD_TURN_OK;
    size_t len = 0;
    uint8_t *bytes = wd_encode(message, &len);
    message = NULL;
    if (bytes == NULL) {
      *err = strdup("cannot encode the driver message");
      return -1;
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = wd_instance_call(turn->driver, bytes, len, &out, &out_len, err);
    free(bytes);
    if (rc != 0) return -1;
    cbor_item_t *reply = wd_decode(out, out_len);
    free(out);
    if (reply == NULL) {
      *err = strdup("driver returned invalid CBOR");
      return -1;
    }
    cbor_item_t *actions = wd_get(reply, "actions");
    size_t count = 0;
    cbor_item_t **items = NULL;
    if (actions != NULL && cbor_isa_array(actions)) {
      items = cbor_array_handle(actions);
      count = cbor_array_size(actions);
    }
    bool pending = false;
    int failed = 0;
    for (size_t i = 0; i < count && !failed && !pending; i++) {
      cbor_item_t *next = NULL;
      if (process_action(turn, items[i], emit, ctx, &next, &pending, false,
                         err) != 0) {
        failed = 1;
        break;
      }
      if (next != NULL) {
        message = next;
        break;
      }
    }
    wd_free(reply);
    if (failed) {
      wd_free(message);
      return -1;
    }
    if (pending) return WD_TURN_PENDING;
    if (message == NULL) return WD_TURN_OK;
  }
  *err = strdup("turn exceeded its action depth");
  wd_free(message);
  return -1;
}

static uint64_t next_turn_id(void) {
  static uint64_t counter;
  if (counter == 0) {
    uint64_t seed = 0;
    if (wd_random_bytes(&seed, sizeof(seed)) != 0) seed = wd_now_ms();
    counter = seed | 1;
  }
  return ++counter;
}

uint64_t wd_turn_id(wd_turn *turn) { return turn != NULL ? turn->id : 0; }

void wd_turn_set_context(wd_turn *turn, const char *text) {
  free(turn->context);
  turn->context = text != NULL ? strdup(text) : NULL;
}


static char *read_project_rules(const char *root) {
  if (root == NULL) return NULL;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/AGENTS.md", root);
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  char *buf = malloc(2049);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, 2048, f);
  fclose(f);
  if (got == 0) {
    free(buf);
    return NULL;
  }
  buf[got] = '\0';
  return buf;
}

static cbor_item_t *history_with_context(cbor_item_t *history,
                                        const char *root,
                                        const char *context) {
  char *rules = read_project_rules(root);
  bool have_context = context != NULL && context[0] != '\0';
  if (rules == NULL && !have_context) return history;
  size_t extra = history != NULL ? cbor_array_size(history) : 0;
  size_t added = (rules != NULL ? 1 : 0) + (have_context ? 1 : 0);
  cbor_item_t *combined = wd_array(extra + added);
  bool pushed = true;
  if (rules != NULL) {
    cbor_item_t *entry = wd_map(2);
    wd_put(entry, "role", wd_text("system"));
    wd_put(entry, "text", wd_text(rules));
    pushed = cbor_array_push(combined, entry);
    cbor_decref(&entry);
    free(rules);
  }
  if (pushed && have_context) {
    cbor_item_t *entry = wd_map(2);
    wd_put(entry, "role", wd_text("system"));
    wd_put(entry, "text", wd_text(context));
    pushed = cbor_array_push(combined, entry);
    cbor_decref(&entry);
  }
  if (pushed && extra > 0) {
    cbor_item_t **items = cbor_array_handle(history);
    for (size_t i = 0; i < extra; i++) {
      if (!cbor_incref(items[i])) continue;
      if (!cbor_array_push(combined, items[i])) break;
    }
  }
  if (history != NULL) wd_free(history);
  return combined;
}

int wd_turn_prompt(wd_turn *turn, const char *text, wd_turn_emit emit, void *ctx,
                   char **err) {
  turn->id = next_turn_id();
  turn->history = history_with_context(turn->history, turn->root, turn->context);
  cbor_item_t *message = wd_map(2);
  if (message == NULL) {
    *err = strdup("out of memory");
    return -1;
  }
  wd_put(message, "kind", wd_text("step"));
  wd_put(message, "prompt", wd_text(text));
  return drive(turn, message, emit, ctx, err);
}

int wd_turn_resume(wd_turn *turn, bool allow, wd_turn_emit emit, void *ctx,
                   char **err) {
  if (turn->parked == NULL) {
    *err = strdup("no approval is pending");
    return -1;
  }
  turn->pending_kind = 0;
  cbor_item_t *action = turn->parked;
  turn->parked = NULL;
  free(turn->park_reason);
  turn->park_reason = NULL;
  cbor_item_t *next = NULL;
  if (allow) {
    bool pending = false;
    int rc =
        process_action(turn, action, emit, ctx, &next, &pending, true, err);
    wd_free(action);
    if (rc != 0) return -1;
    if (pending) return WD_TURN_PENDING;
  } else {
    wd_free(action);
    next = wd_map(2);
    wd_put(next, "kind", wd_text("result"));
    cbor_item_t *payload = wd_map(2);
    wd_put(payload, "ok", cbor_build_bool(false));
    wd_put(payload, "error", wd_text("the approval was denied"));
    wd_put(next, "payload", payload);
  }
  return drive(turn, next, emit, ctx, err);
}

int wd_turn_resume_tool(wd_turn *turn, wd_turn_emit emit, void *ctx,
                        char **err) {
  if (turn->pending_kind != 2) {
    *err = strdup("no tool call is pending");
    return -1;
  }
  turn->pending_kind = 0;
  wd_worker *worker =
      turn->tool_worker != NULL ? turn->tool_worker : worker_for(turn);
  turn->tool_worker = NULL;
  wd_proc_result result;
  cbor_item_t *reply = NULL;
  int rc = wd_worker_complete(worker, WD_TOOL_MAX, &result, err);
  if (rc == 0) {
    exec_reply(&result, &reply);
    wd_proc_result_free(&result);
  } else {
    free(err != NULL ? *err : NULL);
    if (err != NULL) *err = NULL;
    reply = wd_map(2);
    wd_put(reply, "ok", cbor_build_bool(false));
    wd_put(reply, "error", wd_text("the worker is gone"));
  }
  cbor_item_t *message = wd_map(2);
  wd_put(message, "kind", wd_text("result"));
  wd_put(message, "payload", reply);
  return drive(turn, message, emit, ctx, err);
}

int wd_turn_abort(wd_turn *turn, const char *reason, wd_turn_emit emit,
                  void *ctx, char **err) {
  if (turn->pending_kind == 0) {
    *err = strdup("nothing is running");
    return -1;
  }
  if (turn->pending_kind == 2) (void)wd_turn_cancel_tool(turn);
  if (turn->http_job != NULL) {
    wd_http_job_free(turn->http_job);
    turn->http_job = NULL;
  }
  turn->pending_kind = 0;
  if (turn->parked != NULL) {
    wd_free(turn->parked);
    turn->parked = NULL;
  }
  cbor_item_t *event = wd_map(3);
  wd_put(event, "turn", wd_uint(turn->id));
  wd_put(event, "aborted", cbor_build_bool(true));
  wd_put(event, "reason", wd_text(reason != NULL ? reason : "aborted"));
  int rc = emit(ctx, "turn.end", event, err);
  wd_free(event);
  return rc == 0 ? 0 : -1;
}

int wd_turn_resume_http(wd_turn *turn, wd_turn_emit emit, void *ctx,
                        char **err) {
  if (turn->pending_kind != 3 || turn->http_job == NULL) {
    *err = strdup("no provider request is pending");
    return -1;
  }
  turn->pending_kind = 0;
  wd_http_response response;
  int rc = wd_http_job_finish(turn->http_job, &response, err);
  wd_http_job_free(turn->http_job);
  turn->http_job = NULL;
  cbor_item_t *message = wd_map(2);
  wd_put(message, "kind", wd_text("result"));
  if (rc != 0) {
    free(err != NULL ? *err : NULL);
    if (err != NULL) *err = NULL;
    cbor_item_t *payload = wd_map(1);
    wd_put(payload, "text", wd_text("the provider request failed"));
    wd_put(message, "payload", payload);
    return drive(turn, message, emit, ctx, err);
  }
  cbor_item_t *phase = wd_map(2);
  wd_put(phase, "status", wd_uint((uint64_t)response.status));
  wd_put(phase, "body", wd_text_len((const char *)response.body, response.len));
  cbor_item_t *reply = NULL;
  rc = provider_phase(turn, phase, &reply, err);
  wd_free(phase);
  wd_http_response_free(&response);
  if (rc != 0) {
    wd_free(message);
    return -1;
  }
  wd_put(message, "payload", reply);
  return drive(turn, message, emit, ctx, err);
}

int wd_turn_compact(wd_turn *turn, cbor_item_t *history, char **summary,
                    char **err) {
  static const char ask[] =
      "Summarise the conversation so far in at most two hundred words, keeping "
      "decisions, facts and open threads.";
  cbor_item_t *payload = wd_map(2);
  wd_put(payload, "prompt", wd_text(ask));
  if (history != NULL && cbor_incref(history))
    wd_put(payload, "history", history);
  cbor_item_t *request = NULL;
  int rc = provider_phase(turn, payload, &request, err);
  wd_free(payload);
  if (rc != 0) return -1;
  rc = net_submit(turn, request, err);
  wd_free(request);
  if (rc != 1) return -1;
  turn->pending_kind = 0;
  wd_http_response response;
  if (wd_http_job_finish(turn->http_job, &response, err) != 0) {
    wd_http_job_free(turn->http_job);
    turn->http_job = NULL;
    return -1;
  }
  wd_http_job_free(turn->http_job);
  turn->http_job = NULL;
  cbor_item_t *phase = wd_map(2);
  wd_put(phase, "status", wd_uint((uint64_t)response.status));
  wd_put(phase, "body", wd_text_len((const char *)response.body, response.len));
  cbor_item_t *reply = NULL;
  rc = provider_phase(turn, phase, &reply, err);
  wd_free(phase);
  wd_http_response_free(&response);
  if (rc != 0) return -1;
  *summary = wd_text_copy(wd_get(reply, "text"));
  wd_free(reply);
  if (*summary == NULL || (*summary)[0] == '\0') {
    free(*summary);
    *summary = NULL;
    *err = strdup("the provider returned no summary");
    return -1;
  }
  return 0;
}

int wd_turn_http_fd(wd_turn *turn) {
  if (turn == NULL || turn->http_job == NULL) return -1;
  return wd_http_job_fd(turn->http_job);
}

int wd_turn_cancel_tool(wd_turn *turn) {
  if (turn == NULL || turn->pending_kind != 2 || turn->worker == NULL) return 0;
  wd_worker_kill(turn->worker);
  return 1;
}

int wd_turn_pending_kind(wd_turn *turn) { return turn != NULL ? turn->pending_kind : 0; }

int wd_turn_worker_fd(wd_turn *turn) {
  if (turn == NULL || turn->worker == NULL) return -1;
  return wd_worker_fd(turn->worker);
}

int wd_turn_pending(wd_turn *turn, uint64_t *call, uint64_t *epoch,
                    char **reason) {
  if (turn->parked == NULL) return -1;
  if (call != NULL) *call = turn->park_call;
  if (epoch != NULL) *epoch = turn->park_epoch;
  if (reason != NULL)
    *reason = strdup(turn->park_reason != NULL ? turn->park_reason
                                               : "approval required");
  return 0;
}
