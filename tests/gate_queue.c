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

int main(void) {
  alarm(120);
  const char *bin = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  if (bin == NULL || modules == NULL) return 1;
  setenv("WARD_FAKE_DELAY_MS", "1200", 1);
  setenv("WARD_MAX_RUNS", "1", 1);
  sweep_test_cgroups();
  int endpoint_port = 0;
  pid_t endpoint = 0;
  fake_endpoint(&endpoint_port, &endpoint);
  char root[] = "/tmp/ward-queue-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char rules_path[512];
  snprintf(rules_path, sizeof(rules_path), "%s/AGENTS.md", root);
  FILE *rules = fopen(rules_path, "wb");
  assert(rules != NULL);
  assert(fputs("be kind to the tools\n", rules) >= 0);
  fclose(rules);
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
  if (strncmp(line, "ready ", 6) != 0) {
    kill(pid, SIGKILL);
    return 1;
  }

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

  cbor_item_t *hold_result = NULL;
  cbor_item_t *hold = wd_map(2);
  wd_put(hold, "id", wd_text(session));
  wd_put(hold, "text", wd_text("/write parked.txt parked"));
  assert(request(&a, a.next_id++, "session.prompt", hold, &hold_result, NULL) == 0);
  cbor_item_t *approval = wd_get(hold_result, "approval");
  assert(approval != NULL);
  uint64_t call = 0;
  uint64_t epoch = 0;
  assert(wd_uint_of(wd_get(approval, "call"), &call));
  assert(wd_uint_of(wd_get(approval, "epoch"), &epoch));
  wd_free(hold_result);

  cbor_item_t *first = NULL;
  cbor_item_t *first_prompt = wd_map(3);
  wd_put(first_prompt, "id", wd_text(session));
  wd_put(first_prompt, "text", wd_text("queue one"));
  wd_put(first_prompt, "queue", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "session.prompt", first_prompt, &first, NULL) == 0);
  bool first_queued = false;
  uint64_t first_ahead = 99;
  assert(wd_bool_of(wd_get(first, "queued"), &first_queued));
  assert(first_queued);
  assert(wd_uint_of(wd_get(first, "ahead"), &first_ahead));
  assert(first_ahead == 0);
  wd_free(first);

  cbor_item_t *duplicate = NULL;
  cbor_item_t *duplicate_prompt = wd_map(2);
  wd_put(duplicate_prompt, "id", wd_text(session));
  wd_put(duplicate_prompt, "text", wd_text("queue without the flag"));
  bool duplicate_ok = true;
  assert(request(&a, a.next_id++, "session.prompt", duplicate_prompt,
                 &duplicate, &duplicate_ok) == 0);
  assert(!duplicate_ok);
  char *duplicate_name = wd_text_copy(wd_get(duplicate, "name"));
  assert(duplicate_name != NULL && strcmp(duplicate_name, "busy") == 0);
  free(duplicate_name);
  wd_free(duplicate);

  cbor_item_t *second = NULL;
  cbor_item_t *second_prompt = wd_map(3);
  wd_put(second_prompt, "id", wd_text(session));
  wd_put(second_prompt, "text", wd_text("queue two"));
  wd_put(second_prompt, "queue", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "session.prompt", second_prompt, &second,
                 NULL) == 0);
  bool second_queued = false;
  uint64_t second_ahead = 99;
  assert(wd_bool_of(wd_get(second, "queued"), &second_queued));
  assert(second_queued);
  assert(wd_uint_of(wd_get(second, "ahead"), &second_ahead));
  assert(second_ahead == 1);
  wd_free(second);

  cbor_item_t *queue = NULL;
  cbor_item_t *queue_params = wd_map(1);
  wd_put(queue_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.queue", queue_params, &queue, NULL) == 0);
  cbor_item_t *items = wd_get(queue, "items");
  assert(cbor_array_size(items) == 2);
  cbor_item_t *first_row = cbor_array_get(items, 0);
  cbor_item_t *second_row = cbor_array_get(items, 1);
  uint64_t first_id = 0;
  uint64_t second_id = 0;
  uint64_t row_ahead = 99;
  assert(wd_uint_of(wd_get(first_row, "item"), &first_id));
  assert(wd_uint_of(wd_get(second_row, "item"), &second_id));
  assert(first_id > 0 && second_id > first_id);
  char *first_text = wd_text_copy(wd_get(first_row, "text"));
  char *second_text = wd_text_copy(wd_get(second_row, "text"));
  assert(first_text != NULL && strcmp(first_text, "queue one") == 0);
  assert(second_text != NULL && strcmp(second_text, "queue two") == 0);
  assert(wd_uint_of(wd_get(first_row, "ahead"), &row_ahead));
  assert(row_ahead == 0);
  assert(wd_uint_of(wd_get(second_row, "ahead"), &row_ahead));
  assert(row_ahead == 1);
  free(first_text);
  free(second_text);
  cbor_decref(&first_row);
  cbor_decref(&second_row);
  wd_free(queue);

  cbor_item_t *wrong = NULL;
  cbor_item_t *wrong_params = wd_map(2);
  wd_put(wrong_params, "id", wd_text(session));
  wd_put(wrong_params, "item", wd_uint(second_id + 1000));
  bool wrong_ok = true;
  assert(request(&a, a.next_id++, "session.queue.remove", wrong_params, &wrong,
                 &wrong_ok) == 0);
  assert(!wrong_ok);
  char *wrong_name = wd_text_copy(wd_get(wrong, "name"));
  assert(wrong_name != NULL && strcmp(wrong_name, "not_found") == 0);
  free(wrong_name);
  wd_free(wrong);

  cbor_item_t *unchanged = NULL;
  cbor_item_t *unchanged_params = wd_map(1);
  wd_put(unchanged_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.queue", unchanged_params,
                 &unchanged, NULL) == 0);
  assert(cbor_array_size(wd_get(unchanged, "items")) == 2);
  wd_free(unchanged);

  cbor_item_t *removed = NULL;
  cbor_item_t *remove_params = wd_map(2);
  wd_put(remove_params, "id", wd_text(session));
  wd_put(remove_params, "item", wd_uint(first_id));
  assert(request(&a, a.next_id++, "session.queue.remove", remove_params,
                 &removed, NULL) == 0);
  bool was_removed = false;
  assert(wd_bool_of(wd_get(removed, "removed"), &was_removed));
  assert(was_removed);
  wd_free(removed);

  cbor_item_t *survivors = NULL;
  cbor_item_t *survivors_params = wd_map(1);
  wd_put(survivors_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.queue", survivors_params,
                 &survivors, NULL) == 0);
  cbor_item_t *survivor_items = wd_get(survivors, "items");
  assert(cbor_array_size(survivor_items) == 1);
  cbor_item_t *survivor = cbor_array_get(survivor_items, 0);
  uint64_t survivor_id = 0;
  assert(wd_uint_of(wd_get(survivor, "item"), &survivor_id));
  assert(survivor_id == second_id);
  char *survivor_text = wd_text_copy(wd_get(survivor, "text"));
  assert(survivor_text != NULL && strcmp(survivor_text, "queue two") == 0);
  free(survivor_text);
  cbor_decref(&survivor);
  wd_free(survivors);

  cbor_item_t *get = NULL;
  cbor_item_t *get_params = wd_map(1);
  wd_put(get_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.get", get_params, &get, NULL) == 0);
  uint64_t before_seq = 0;
  assert(wd_uint_of(wd_get(wd_get(get, "session"), "last_seq"), &before_seq));
  wd_free(get);

  cbor_item_t *steered = NULL;
  cbor_item_t *steer_params = wd_map(2);
  wd_put(steer_params, "id", wd_text(session));
  wd_put(steer_params, "item", wd_uint(second_id));
  assert(request(&a, a.next_id++, "session.queue.steer", steer_params,
                 &steered, NULL) == 0);
  bool steered_ok = false;
  uint64_t steered_id = 0;
  assert(wd_bool_of(wd_get(steered, "steered"), &steered_ok));
  assert(steered_ok);
  assert(wd_uint_of(wd_get(steered, "item"), &steered_id));
  assert(steered_id == second_id);
  wd_free(steered);

  cbor_item_t *empty = NULL;
  cbor_item_t *empty_params = wd_map(1);
  wd_put(empty_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.queue", empty_params, &empty, NULL) == 0);
  assert(cbor_array_size(wd_get(empty, "items")) == 0);
  wd_free(empty);

  uint64_t steer_seq = 0;
  for (;;) {
    char *kind = NULL;
    uint64_t seq = 0;
    cbor_item_t *event = next_event(&a, &kind, &seq);
    assert(event != NULL);
    if (kind != NULL && strcmp(kind, "session.prompted") == 0 &&
        seq > before_seq) {
      char *text = wd_text_copy(wd_get(event, "text"));
      assert(text != NULL && strcmp(text, "queue two") == 0);
      free(text);
      steer_seq = seq;
      free(kind);
      wd_free(event);
      break;
    }
    free(kind);
    wd_free(event);
  }

  cbor_item_t *denied = NULL;
  cbor_item_t *deny = wd_map(4);
  wd_put(deny, "session", wd_text(session));
  wd_put(deny, "call", wd_uint(call));
  wd_put(deny, "epoch", wd_uint(epoch));
  wd_put(deny, "allow", cbor_build_bool(false));
  assert(request(&a, a.next_id++, "approval.respond", deny, &denied, NULL) == 0);
  wd_free(denied);

  uint64_t carried_seq = 0;
  for (;;) {
    char *kind = NULL;
    uint64_t seq = 0;
    cbor_item_t *event = next_event(&a, &kind, &seq);
    assert(event != NULL);
    if (kind != NULL && strcmp(kind, "session.prompted") == 0 &&
        seq > steer_seq) {
      char *text = wd_text_copy(wd_get(event, "text"));
      assert(text != NULL && strcmp(text, "queue two") == 0);
      free(text);
      carried_seq = seq;
      free(kind);
      wd_free(event);
      break;
    }
    free(kind);
    wd_free(event);
  }
  assert(carried_seq > steer_seq);
  cbor_item_t *carried_end = wait_event(&a, "turn.end", NULL);
  assert(carried_end != NULL);
  wd_free(carried_end);

  cbor_item_t *sched = NULL;
  cbor_item_t *sched_params = wd_map(3);
  wd_put(sched_params, "session", wd_text(session));
  wd_put(sched_params, "cron", wd_text("@every 60000"));
  wd_put(sched_params, "text", wd_text("/read parked.txt"));
  assert(request(&a, a.next_id++, "schedule.set", sched_params, &sched, NULL) == 0);
  char *sched_id = wd_text_copy(wd_get(sched, "schedule"));
  assert(sched_id != NULL);
  wd_free(sched);

  cbor_item_t *sched_list = NULL;
  assert(request(&a, a.next_id++, "schedule.list", wd_map(0), &sched_list, NULL) == 0);
  cbor_item_t *schedules = wd_get(sched_list, "schedules");
  assert(cbor_array_size(schedules) == 1);
  cbor_item_t *listed = cbor_array_get(schedules, 0);
  char *listed_prompt = wd_text_copy(wd_get(listed, "prompt"));
  bool listed_active = false;
  char *listed_title = wd_text_copy(wd_get(listed, "title"));
  assert(listed_prompt != NULL && strcmp(listed_prompt, "/read parked.txt") == 0);
  assert(listed_title != NULL && listed_title[0] == '\0');
  assert(wd_bool_of(wd_get(listed, "active"), &listed_active));
  assert(listed_active);
  free(listed_prompt);
  free(listed_title);
  cbor_decref(&listed);
  wd_free(sched_list);

  cbor_item_t *updated = NULL;
  cbor_item_t *update_params = wd_map(3);
  wd_put(update_params, "id", wd_text(sched_id));
  wd_put(update_params, "title", wd_text("queue gate schedule"));
  wd_put(update_params, "prompt", wd_text("/read parked.txt changed"));
  assert(request(&a, a.next_id++, "schedule.update", update_params, &updated,
                 NULL) == 0);
  char *updated_title = wd_text_copy(wd_get(updated, "title"));
  char *updated_prompt = wd_text_copy(wd_get(updated, "prompt"));
  bool updated_active = false;
  assert(updated_title != NULL && strcmp(updated_title, "queue gate schedule") == 0);
  assert(updated_prompt != NULL &&
         strcmp(updated_prompt, "/read parked.txt changed") == 0);
  assert(wd_bool_of(wd_get(updated, "active"), &updated_active));
  assert(updated_active);
  free(updated_title);
  free(updated_prompt);
  wd_free(updated);

  cbor_item_t *after_update = NULL;
  assert(request(&a, a.next_id++, "schedule.list", wd_map(0), &after_update,
                 NULL) == 0);
  cbor_item_t *after_schedules = wd_get(after_update, "schedules");
  assert(cbor_array_size(after_schedules) == 1);
  cbor_item_t *after_entry = cbor_array_get(after_schedules, 0);
  char *after_title = wd_text_copy(wd_get(after_entry, "title"));
  char *after_prompt = wd_text_copy(wd_get(after_entry, "prompt"));
  assert(after_title != NULL && strcmp(after_title, "queue gate schedule") == 0);
  assert(after_prompt != NULL &&
         strcmp(after_prompt, "/read parked.txt changed") == 0);
  free(after_title);
  free(after_prompt);
  cbor_decref(&after_entry);
  wd_free(after_update);

  cbor_item_t *paused = NULL;
  cbor_item_t *pause_params = wd_map(2);
  wd_put(pause_params, "id", wd_text(sched_id));
  wd_put(pause_params, "active", cbor_build_bool(false));
  assert(request(&a, a.next_id++, "schedule.update", pause_params, &paused,
                 NULL) == 0);
  bool paused_active = true;
  uint64_t paused_next = 1;
  assert(wd_bool_of(wd_get(paused, "active"), &paused_active));
  assert(!paused_active);
  assert(wd_uint_of(wd_get(paused, "next_ms"), &paused_next));
  assert(paused_next == 0);
  wd_free(paused);

  cbor_item_t *paused_list = NULL;
  assert(request(&a, a.next_id++, "schedule.list", wd_map(0), &paused_list,
                 NULL) == 0);
  cbor_item_t *paused_schedules = wd_get(paused_list, "schedules");
  assert(cbor_array_size(paused_schedules) == 1);
  cbor_item_t *paused_entry = cbor_array_get(paused_schedules, 0);
  bool listed_paused = true;
  assert(wd_bool_of(wd_get(paused_entry, "active"), &listed_paused));
  assert(!listed_paused);
  cbor_decref(&paused_entry);
  wd_free(paused_list);

  cbor_item_t *missing = NULL;
  cbor_item_t *missing_params = wd_map(2);
  wd_put(missing_params, "id", wd_text("00000000"));
  wd_put(missing_params, "title", wd_text("nothing"));
  bool missing_ok = true;
  assert(request(&a, a.next_id++, "schedule.update", missing_params, &missing,
                 &missing_ok) == 0);
  assert(!missing_ok);
  char *missing_name = wd_text_copy(wd_get(missing, "name"));
  assert(missing_name != NULL && strcmp(missing_name, "not_found") == 0);
  free(missing_name);
  wd_free(missing);

  cbor_item_t *deleted = NULL;
  cbor_item_t *delete_params = wd_map(1);
  wd_put(delete_params, "schedule", wd_text(sched_id));
  assert(request(&a, a.next_id++, "schedule.delete", delete_params, &deleted,
                 NULL) == 0);
  bool was_deleted = false;
  assert(wd_bool_of(wd_get(deleted, "deleted"), &was_deleted));
  assert(was_deleted);
  wd_free(deleted);
  free(sched_id);
  free(session);
  close(a.fd);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  stop_server(endpoint);
  const char *cgroup = getenv("WARD_GATE_CGROUP");
  if (cgroup != NULL && strcmp(cgroup, "off") != 0) drop_test_cgroup(cgroup);
  printf("queue gate passed: ids, row steer, carry-over and schedule update\n");
  return 0;
}
