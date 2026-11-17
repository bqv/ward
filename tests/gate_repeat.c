/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "core/util.h"
#include "fake_endpoint.h"
#include <sys/stat.h>
#include "proto/codec.h"
#include "proto/frame.h"

typedef struct {
  int fd;
  uint64_t next_id;
} client;

typedef struct pending {
  struct pending *next;
  char *kind;
  cbor_item_t *payload;
  uint64_t seq;
} pending;

static pending *queue_head;
static pending *queue_tail;

static void queue_push(char *kind, cbor_item_t *payload, uint64_t seq) {
  pending *item = calloc(1, sizeof(*item));
  assert(item != NULL);
  item->kind = kind;
  item->payload = payload;
  item->seq = seq;
  if (queue_tail != NULL)
    queue_tail->next = item;
  else
    queue_head = item;
  queue_tail = item;
}

static pending *queue_pop(void) {
  pending *item = queue_head;
  if (item == NULL) return NULL;
  queue_head = item->next;
  if (queue_head == NULL) queue_tail = NULL;
  return item;
}

static int read_full(int fd, void *buf, size_t len) {
  size_t off = 0;
  uint8_t *p = buf;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static int send_item(int fd, uint16_t type, uint64_t sub, uint64_t seq,
                     cbor_item_t *item) {
  size_t len = 0;
  uint8_t *body = wd_encode(item, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, type, sub, seq, (uint32_t)len);
  int rc = wd_write_all(fd, header, sizeof(header));
  if (rc == 0) rc = wd_write_all(fd, body, len);
  free(body);
  return rc;
}

static cbor_item_t *next_frame(int fd, wd_frame *frame) {
  uint8_t header[WD_FRAME_HEADER];
  if (read_full(fd, header, sizeof(header)) != 0) return NULL;
  uint32_t len = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                 ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
  if (len == 0 || len > WD_MAX_BODY) return NULL;
  uint8_t *body = malloc(len);
  if (body == NULL) return NULL;
  if (read_full(fd, body, len) != 0) {
    free(body);
    return NULL;
  }
  frame->type = (uint16_t)((uint16_t)header[4] | ((uint16_t)header[5] << 8));
  frame->sub = 0;
  frame->seq = 0;
  for (unsigned i = 0; i < 8; i++) {
    frame->sub |= (uint64_t)header[6 + i] << (8 * i);
    frame->seq |= (uint64_t)header[14 + i] << (8 * i);
  }
  frame->len = len;
  cbor_item_t *root = wd_decode(body, len);
  free(body);
  return root;
}

static uint64_t last_turn;

static int request(client *c, uint64_t id, const char *method,
                   cbor_item_t *params, cbor_item_t **result, bool *ok_out) {
  if (send_item(c->fd, WD_FRAME_REQUEST, 0, 0, wd_request(id, method, params)) !=
      0)
    return -1;
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(c->fd, &frame);
    if (root == NULL) return -1;
    if (frame.type == WD_FRAME_EVENT) {
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (wd_read_event(root, &kind, &payload) && cbor_incref(payload))
        queue_push(kind, payload, frame.seq);
      else
        free(kind);
      wd_free(root);
      continue;
    }
    uint64_t got = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (frame.type != WD_FRAME_RESPONSE ||
        !wd_read_response(root, &got, &ok, &value) || got != id) {
      wd_free(root);
      return -1;
    }
    if (!cbor_incref(value)) {
      wd_free(root);
      return -1;
    }
    wd_free(root);
    *result = value;
    uint64_t turn = 0;
    if (wd_uint_of(wd_get(value, "turn"), &turn)) last_turn = turn;
    if (ok_out != NULL) *ok_out = ok;
    return 0;
  }
}

static int connect_client(client *c, const char *sock, uint64_t first_id) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (c->fd < 0) return -1;
  if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
  c->next_id = first_id;
  if (send_item(c->fd, WD_FRAME_HELLO, 0, 0, wd_hello()) != 0) return -1;
  wd_frame frame;
  cbor_item_t *welcome = next_frame(c->fd, &frame);
  if (welcome == NULL || frame.type != WD_FRAME_WELCOME) return -1;
  wd_free(welcome);
  return 0;
}

static cbor_item_t *next_event(client *c, char **kind, uint64_t *seq) {
  pending *queued = queue_pop();
  if (queued != NULL) {
    *kind = queued->kind;
    if (seq != NULL) *seq = queued->seq;
    cbor_item_t *payload = queued->payload;
    free(queued);
    return payload;
  }
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(c->fd, &frame);
    if (root == NULL) return NULL;
    if (frame.type != WD_FRAME_EVENT) {
      wd_free(root);
      continue;
    }
    cbor_item_t *payload = NULL;
    if (!wd_read_event(root, kind, &payload)) {
      wd_free(root);
      return NULL;
    }
    if (!cbor_incref(payload)) {
      free(*kind);
      wd_free(root);
      return NULL;
    }
    if (seq != NULL) *seq = frame.seq;
    wd_free(root);
    return payload;
  }
}

static cbor_item_t *wait_turn_end(client *c, uint64_t turn) {
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, NULL);
    if (payload == NULL) return NULL;
    if (kind != NULL && strcmp(kind, "turn.end") == 0) {
      uint64_t got = 0;
      wd_uint_of(wd_get(payload, "turn"), &got);
      if (turn == 0 || got == turn) {
        free(kind);
        return payload;
      }
    }
    free(kind);
    wd_free(payload);
  }
}

static int turn_deltas;
static int turn_reasoning;

static void turn_text(client *c, uint64_t turn, char *out, size_t cap) {
  out[0] = '\0';
  turn_deltas = 0;
  turn_reasoning = 0;
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, NULL);
    assert(payload != NULL);
    if (kind != NULL && strcmp(kind, "reasoning.delta") == 0) {
      turn_reasoning++;
      wd_free(payload);
      free(kind);
      continue;
    }
    if (kind != NULL && strcmp(kind, "text.delta") == 0) {
      turn_deltas++;
      char *text = wd_text_copy(wd_get(payload, "text"));
      if (text != NULL) {
        size_t have = strlen(out);
        snprintf(out + have, cap - have, "%s", text);
        free(text);
      }
      wd_free(payload);
      free(kind);
      continue;
    }
    if (kind != NULL && strcmp(kind, "turn.end") == 0) {
      uint64_t ended = 0;
      if (wd_uint_of(wd_get(payload, "turn"), &ended) && ended == turn) {
        wd_free(payload);
        free(kind);
        return;
      }
    }
    wd_free(payload);
    free(kind);
  }
}

static cbor_item_t *wait_event(client *c, const char *want, uint64_t *seq) {
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, seq);
    if (payload == NULL) return NULL;
    if (kind != NULL && strcmp(kind, want) == 0) {
      free(kind);
      return payload;
    }
    free(kind);
    wd_free(payload);
  }
}


/* A PATH with every host tool except the Python interpreter.  The /py
 * section must still print 42, so the embedded guest, not python3, ran. */
static char *pythonless_path(void) {
  char template[] = "/tmp/ward-no-python-XXXXXX";
  char *dir = mkdtemp(template);
  assert(dir != NULL);
  DIR *tools = opendir("/usr/bin");
  assert(tools != NULL);
  struct dirent *entry;
  while ((entry = readdir(tools)) != NULL) {
    if (strncmp(entry->d_name, "python", 6) == 0) continue;
    char from[1024];
    char to[1024];
    snprintf(from, sizeof(from), "/usr/bin/%s", entry->d_name);
    snprintf(to, sizeof(to), "%s/%s", dir, entry->d_name);
    struct stat st;
    if (stat(from, &st) == 0 && S_ISREG(st.st_mode) &&
        (st.st_mode & 0111) != 0)
      (void)symlink(from, to);
  }
  closedir(tools);
  return strdup(dir);
}


static long proc_status_kb(pid_t pid, const char *key) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
  FILE *file = fopen(path, "r");
  if (file == NULL) return -1;
  char line[256];
  long value = -1;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, key, strlen(key)) == 0) {
      sscanf(line + strlen(key), "%ld", &value);
      break;
    }
  }
  fclose(file);
  return value;
}
static void seed_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  assert(fd >= 0);
  size_t len = strlen(text);
  assert(write(fd, text, len) == (ssize_t)len);
  close(fd);
}

static void prompt_text(client *c, const char *session, const char *text) {
  cbor_item_t *ack = NULL;
  cbor_item_t *params = wd_map(2);
  wd_put(params, "id", wd_text(session));
  wd_put(params, "text", wd_text(text));
  assert(request(c, c->next_id++, "session.prompt", params, &ack, NULL) == 0);
  wd_free(ack);
}

int main(void) {
  alarm(120);
  const char *bin = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  if (bin == NULL || modules == NULL) return 1;
  setenv("WARD_FAKE_DELAY_MS", "100", 1);
  sweep_test_cgroups();
  char root[] = "/tmp/ward-repeat-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char note_path[512];
  snprintf(note_path, sizeof(note_path), "%s/note.txt", root);
  seed_file(note_path, "a note the model may read\n");
  char always_path[512];
  snprintf(always_path, sizeof(always_path), "%s/always-tool", root);
  seed_file(always_path, "always\n");
  setenv("WARD_FAKE_ALWAYS_TOOL", always_path, 1);
  int endpoint_port = 0;
  pid_t endpoint = 0;
  fake_endpoint(&endpoint_port, &endpoint);
  char estate_url[128];
  snprintf(estate_url, sizeof(estate_url),
           "http://127.0.0.1:%d/v1/systemone", endpoint_port);
  setenv("WARD_ESTATE_URL", estate_url, 1);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);
  char models_path[512];
  snprintf(models_path, sizeof(models_path), "%s/models.toml", state);
  FILE *models = fopen(models_path, "w");
  assert(models != NULL);
  assert(fputs("[[model]]\nid = \"local:qwen\"\nprovider = \"stadi\"\n"
               "model = \"qwen3\"\ncontext = \"32768\"\n"
               "price = \"0.04,2.0\"\n",
               models) >= 0);
  fclose(models);
  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    const char *cgroup = getenv("WARD_GATE_CGROUP");
    setenv("WARD_CGROUP", cgroup != NULL ? cgroup : "off", 1);
    setenv("WARD_MAX_RUNS", "2", 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(bin, "ward", "up", "--socket", sock, "--state", state, (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[256];
  size_t used = 0;
  while (used + 1 < sizeof(line)) {
    ssize_t n = read(ready[0], line + used, 1);
    if (n <= 0) break;
    if (line[used++] == '\n') break;
  }
  line[used] = '\0';
  close(ready[0]);
  assert(strncmp(line, "ready ", 6) == 0);
  client a;
  assert(connect_client(&a, sock, 1) == 0);
  cbor_item_t *created = NULL;
  cbor_item_t *create = wd_map(1);
  wd_put(create, "cwd", wd_text(root));
  assert(request(&a, a.next_id++, "session.create", create, &created, NULL) == 0);
  char *session = wd_text_copy(wd_get(wd_get(created, "session"), "id"));
  assert(session != NULL);
  wd_free(created);
  cbor_item_t *resumed = NULL;
  cbor_item_t *resume = wd_map(2);
  wd_put(resume, "id", wd_text(session));
  wd_put(resume, "from", wd_uint(0));
  assert(request(&a, a.next_id++, "session.resume", resume, &resumed, NULL) == 0);
  wd_free(resumed);
  prompt_text(&a, session, "use a tool now");
  int tool_calls = 0;
  int tool_results = 0;
  int repeated = 0;
  bool ended = false;
  for (;;) {
    char *kind = NULL;
    cbor_item_t *event = next_event(&a, &kind, NULL);
    assert(event != NULL);
    if (kind != NULL && strcmp(kind, "tool.call") == 0) {
      tool_calls++;
    } else if (kind != NULL && strcmp(kind, "tool.result") == 0) {
      tool_results++;
    } else if (kind != NULL && strcmp(kind, "tool.repeated") == 0) {
      repeated++;
      char *tool = wd_text_copy(wd_get(event, "tool"));
      uint64_t count = 0;
      assert(tool != NULL && strcmp(tool, "fs.read") == 0);
      assert(wd_uint_of(wd_get(event, "count"), &count));
      assert(count == 3);
      free(tool);
    } else if (kind != NULL && strcmp(kind, "turn.end") == 0) {
      ended = true;
      wd_free(event);
      free(kind);
      break;
    }
    wd_free(event);
    free(kind);
  }
  assert(ended);
  assert(repeated == 1);
  assert(tool_calls >= 3 && tool_calls <= 4);
  assert(tool_results >= 3);
  free(session);
  close(a.fd);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  stop_server(endpoint);
  printf("repeat gate passed: the third identical tool call warns once and runs\n");
  return 0;
}
