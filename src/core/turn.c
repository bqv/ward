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
#include <unistd.h>

#include "core/util.h"
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
  wd_instance *driver;
  wd_instance *provider;
  wd_instance *eng;
  wd_instance *tools;
  wd_instance *policy;
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
  int written = wu_write_all(fd, text, len);
  close(fd);
  free(text);
  if (written != 0) return tool_fail("fs.write: cannot write the file", out, out_len);
  return tool_ok_count(len, out, out_len);
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
  if (strcmp(capability, "policy.check") == 0)
    return wd_instance_call(turn->policy, in, in_len, out, out_len, err);
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
  static const char *const driver_caps[] = {"engine.call", "policy.check"};
  static const char *const provider_caps[] = {"net.request"};
  static const char *const tools_caps[] = {"fs.read", "fs.write"};
  static const char *const no_caps[] = {NULL};
  wd_module_spec engine_spec = {"engine", no_caps, 0, true};
  wd_module_spec driver_spec = {"driver", driver_caps, 2, false};
  wd_module_spec policy_spec = {"policy", no_caps, 0, true};
  wd_module_spec provider_spec = {"provider", provider_caps, 1, false};
  wd_module_spec tools_spec = {"tools", tools_caps, 2, false};

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

  turn->eng = wd_instance_new(turn->engine_module, no_caps, 0, &TURN_LIMITS,
                              NULL, NULL, err);
  if (turn->eng == NULL) goto fail;
  turn->provider = wd_instance_new(turn->provider_module, provider_caps, 1,
                                   &TURN_LIMITS, turn_capability, turn, err);
  if (turn->provider == NULL) goto fail;
  turn->driver = wd_instance_new(turn->driver_module, driver_caps, 2,
                                 &TURN_LIMITS, turn_capability, turn, err);
  if (turn->driver == NULL) goto fail;
  turn->tools = wd_instance_new(turn->tools_module, tools_caps, 2,
                                &TURN_LIMITS, turn_capability, turn, err);
  if (turn->tools == NULL) goto fail;
  turn->policy = wd_instance_new(turn->policy_module, no_caps, 0, &TURN_LIMITS,
                                 turn_capability, turn, err);
  if (turn->policy == NULL) goto fail;
  return turn;

fail:
  wd_turn_free(turn);
  return NULL;
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
  wd_instance_free(turn->driver);
  wd_instance_free(turn->provider);
  wd_instance_free(turn->tools);
  wd_instance_free(turn->policy);
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
      rc = emit(ctx, event, wd_get(action, "payload"), err);
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
        if (rc == 0) *pending = true;
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

int wd_turn_prompt(wd_turn *turn, const char *text, wd_turn_emit emit, void *ctx,
                   char **err) {
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
