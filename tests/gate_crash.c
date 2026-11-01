/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/util.h"
#include "fake_endpoint.h"
#include "proto/codec.h"
#include "proto/frame.h"

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

static int request(int fd, uint64_t id, const char *method, cbor_item_t *params,
                   cbor_item_t **result) {
  if (send_item(fd, WD_FRAME_REQUEST, 0, 0, wd_request(id, method, params)) != 0)
    return -1;
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(fd, &frame);
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
    if (!ok) {
      char *name = wd_text_copy(wd_get(value, "name"));
      fprintf(stderr, "gate: %s failed: %s\n", method,
              name != NULL ? name : "?");
      free(name);
      wd_free(root);
      return -1;
    }
    if (!cbor_incref(value)) {
      wd_free(root);
      return -1;
    }
    wd_free(root);
    *result = value;
    return 0;
  }
}

static pid_t start_daemon(const char *bin, const char *sock, const char *state) {
  unlink(sock);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    execl(bin, "ward", "up", "--socket", sock, "--state", state, (char *)NULL);
    _exit(127);
  }
  for (int i = 0; i < 2000; i++) {
    if (access(sock, F_OK) == 0) {
      int fd = socket(AF_UNIX, SOCK_STREAM, 0);
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
      int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
      close(fd);
      if (rc == 0) return pid;
    }
    usleep(5000);
  }
  kill(pid, SIGKILL);
  return -1;
}

static int connect_client(const char *sock) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(send_item(fd, WD_FRAME_HELLO, 0, 0, wd_hello()) == 0);
  wd_frame frame;
  cbor_item_t *welcome = next_frame(fd, &frame);
  assert(welcome != NULL);
  assert(frame.type == WD_FRAME_WELCOME);
  wd_free(welcome);
  return fd;
}

static int read_until_event(int fd, const char *want, uint64_t *count) {
  pending *queued = queue_pop();
  if (queued != NULL) {
    int hit = queued->kind != NULL && strcmp(queued->kind, want) == 0;
    if (count != NULL) (*count)++;
    free(queued->kind);
    wd_free(queued->payload);
    free(queued);
    if (!hit) return read_until_event(fd, want, count);
    return 0;
  }
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(fd, &frame);
    if (root == NULL) return -1;
    if (frame.type != WD_FRAME_EVENT) {
      wd_free(root);
      continue;
    }
    char *kind = NULL;
    cbor_item_t *payload = NULL;
    if (!wd_read_event(root, &kind, &payload)) {
      wd_free(root);
      return -1;
    }
    if (count != NULL) (*count)++;
    int hit = kind != NULL && strcmp(kind, want) == 0;
    free(kind);
    wd_free(root);
    if (hit) return 0;
  }
}

int main(void) {
  alarm(120);
  const char *bin = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  if (bin == NULL || modules == NULL) {
    fprintf(stderr, "gate: WARD_BIN and WARD_MODULE_DIR are required\n");
    return 1;
  }
  int port = 0;
  pid_t endpoint = 0;
  fake_endpoint(&port, &endpoint);

  char root[] = "/tmp/ward-crash-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);

  pid_t daemon = start_daemon(bin, sock, state);
  assert(daemon > 0);

  int a = connect_client(sock);
  cbor_item_t *created = NULL;
  assert(request(a, 1, "session.create", wd_map(0), &created) == 0);
  char *session = wd_text_copy(wd_get(wd_get(created, "session"), "id"));
  assert(session != NULL);
  wd_free(created);

  cbor_item_t *resumed = NULL;
  cbor_item_t *focus = wd_map(1);
  wd_put(focus, "id", wd_text(session));
  assert(request(a, 2, "session.resume", focus, &resumed) == 0);
  wd_free(resumed);

  cbor_item_t *turn = NULL;
  cbor_item_t *prompt = wd_map(2);
  wd_put(prompt, "id", wd_text(session));
  wd_put(prompt, "text", wd_text("hello before the crash"));
  assert(request(a, 3, "session.prompt", prompt, &turn) == 0);
  wd_free(turn);
  assert(read_until_event(a, "turn.end", NULL) == 0);

  cbor_item_t *parked = NULL;
  cbor_item_t *second = wd_map(2);
  wd_put(second, "id", wd_text(session));
  wd_put(second, "text", wd_text("/write note.txt after the crash"));
  assert(request(a, 4, "session.prompt", second, &parked) == 0);
  cbor_item_t *approval = wd_get(parked, "approval");
  assert(approval != NULL);
  wd_free(parked);

  close(a);
  kill(daemon, SIGKILL);
  int status = 0;
  waitpid(daemon, &status, 0);

  daemon = start_daemon(bin, sock, state);
  assert(daemon > 0);

  int b = connect_client(sock);
  cbor_item_t *got = NULL;
  cbor_item_t *get = wd_map(1);
  wd_put(get, "id", wd_text(session));
  assert(request(b, 1, "session.get", get, &got) == 0);
  uint64_t last_seq = 0;
  assert(wd_uint_of(wd_get(wd_get(got, "session"), "last_seq"), &last_seq));
  assert(last_seq >= 4);
  wd_free(got);

  cbor_item_t *replayed = NULL;
  cbor_item_t *resume = wd_map(2);
  wd_put(resume, "id", wd_text(session));
  wd_put(resume, "from", wd_uint(0));
  assert(request(b, 2, "session.resume", resume, &replayed) == 0);
  wd_free(replayed);

  uint64_t count = 0;
  int saw_turn_end = 0;
  int saw_approval = 0;
  uint64_t expect = 1;
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(b, &frame);
    assert(root != NULL);
    assert(frame.type == WD_FRAME_EVENT);
    char *kind = NULL;
    cbor_item_t *payload = NULL;
    assert(wd_read_event(root, &kind, &payload));
    if (strcmp(kind, "replay.end") == 0) {
      free(kind);
      wd_free(root);
      break;
    }
    assert(frame.seq == expect);
    expect++;
    count++;
    if (strcmp(kind, "turn.end") == 0) saw_turn_end = 1;
    if (strcmp(kind, "approval.requested") == 0) saw_approval = 1;
    free(kind);
    wd_free(root);
  }
  assert(count == last_seq);
  assert(saw_turn_end);
  assert(saw_approval);

  cbor_item_t *after = NULL;
  cbor_item_t *third = wd_map(2);
  wd_put(third, "id", wd_text(session));
  wd_put(third, "text", wd_text("alive after the restart"));
  assert(request(b, 3, "session.prompt", third, &after) == 0);
  wd_free(after);
  assert(read_until_event(b, "turn.end", NULL) == 0);

  free(session);
  close(b);
  kill(daemon, SIGKILL);
  waitpid(daemon, NULL, 0);
  stop_server(endpoint);
  printf("crash gate passed: %llu durable events survived kill -9\n",
         (unsigned long long)count);
  return 0;
}
