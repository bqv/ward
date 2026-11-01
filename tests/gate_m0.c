/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "proto/codec.h"
#include "proto/frame.h"

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
  int rc = 0;
  if (write(fd, header, sizeof(header)) != (ssize_t)sizeof(header)) rc = -1;
  if (rc == 0 && write(fd, body, len) != (ssize_t)len) rc = -1;
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
    (void)cbor_incref(value);
    wd_free(root);
    *result = value;
    return 0;
  }
}

static int session_count(int fd) {
  static uint64_t next_id = 5000;
  cbor_item_t *result = NULL;
  if (request(fd, next_id++, "session.list", wd_map(0), &result) != 0) return -1;
  cbor_item_t *sessions = wd_get(result, "sessions");
  int n = (int)cbor_array_size(sessions);
  wd_free(result);
  return n;
}

static double seconds_since(struct timespec *start) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (double)(now.tv_sec - start->tv_sec) +
         (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int main(void) {
  alarm(120);
  const char *bin = getenv("WARD_BIN");
  if (bin == NULL) {
    fprintf(stderr, "gate: WARD_BIN is not set\n");
    return 1;
  }
  char root[] = "/tmp/ward-gate-XXXXXX";
  if (mkdtemp(root) == NULL) return 1;
  char *state = malloc(strlen(root) + 8);
  char *sock = malloc(strlen(root) + 8);
  assert(state != NULL && sock != NULL);
  sprintf(state, "%s/state", root);
  sprintf(sock, "%s/sock", root);

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    execl(bin, "ward", "up", "--socket", sock, "--state", state,
          (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[512];
  size_t used = 0;
  while (used + 1 < sizeof(line)) {
    ssize_t n = read(ready[0], line + used, 1);
    if (n <= 0) break;
    if (line[used++] == '\n') break;
  }
  line[used] = '\0';
  close(ready[0]);
  if (strncmp(line, "ready ", 6) != 0) {
    fprintf(stderr, "gate: daemon did not become ready: %s\n", line);
    kill(pid, SIGKILL);
    return 1;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "gate: connect failed: %s\n", strerror(errno));
    kill(pid, SIGKILL);
    return 1;
  }

  if (send_item(fd, WD_FRAME_HELLO, 0, 0, wd_hello()) != 0) return 1;
  wd_frame frame;
  cbor_item_t *welcome = next_frame(fd, &frame);
  assert(welcome != NULL);
  assert(frame.type == WD_FRAME_WELCOME);
  unsigned version = 0;
  uint64_t uid = 0;
  assert(wd_read_welcome(welcome, &version, &uid));
  assert(version == WD_PROTOCOL_VERSION);
  wd_free(welcome);

  cbor_item_t *created = NULL;
  assert(request(fd, 1, "session.create", wd_map(0), &created) == 0);
  cbor_item_t *session = wd_get(created, "session");
  char *id = wd_text_copy(wd_get(session, "id"));
  assert(id != NULL && strlen(id) == 16);
  wd_free(created);
  assert(session_count(fd) == 1);

  cbor_item_t *params = wd_map(1);
  wd_put(params, "id", wd_text(id));
  cbor_item_t *got = NULL;
  assert(request(fd, 2, "session.get", params, &got) == 0);
  char *got_id = wd_text_copy(wd_get(wd_get(got, "session"), "id"));
  assert(got_id != NULL && strcmp(got_id, id) == 0);
  free(got_id);
  wd_free(got);

  cbor_item_t *resume_params = wd_map(2);
  wd_put(resume_params, "id", wd_text(id));
  wd_put(resume_params, "from", wd_uint(0));
  cbor_item_t *resumed = NULL;
  assert(request(fd, 3, "session.resume", resume_params, &resumed) == 0);
  uint64_t sub = 0;
  uint64_t last_seq = 0;
  assert(wd_uint_of(wd_get(resumed, "sub"), &sub) && sub >= 1);
  assert(wd_uint_of(wd_get(resumed, "last_seq"), &last_seq) && last_seq >= 1);
  wd_free(resumed);

  cbor_item_t *event = next_frame(fd, &frame);
  assert(event != NULL && frame.type == WD_FRAME_EVENT && frame.sub == sub);
  char *kind = NULL;
  cbor_item_t *payload = NULL;
  assert(wd_read_event(event, &kind, &payload));
  assert(strcmp(kind, "session.updated") == 0);
  assert(frame.seq == 1);
  free(kind);
  wd_free(event);
  event = next_frame(fd, &frame);
  assert(event != NULL && frame.type == WD_FRAME_EVENT);
  assert(wd_read_event(event, &kind, &payload));
  assert(strcmp(kind, "replay.end") == 0);
  wd_free(event);
  free(kind);

  cbor_item_t *replay = wd_map(2);
  wd_put(replay, "replay", wd_uint(sub));
  wd_put(replay, "from", wd_uint(0));
  assert(send_item(fd, WD_FRAME_CONTROL, 0, 0, replay) == 0);
  event = next_frame(fd, &frame);
  assert(event != NULL && frame.type == WD_FRAME_EVENT && frame.seq == 1);
  assert(wd_read_event(event, &kind, &payload));
  assert(strcmp(kind, "session.updated") == 0);
  free(kind);
  wd_free(event);
  event = next_frame(fd, &frame);
  assert(event != NULL && wd_read_event(event, &kind, &payload));
  assert(strcmp(kind, "replay.end") == 0);
  assert(frame.seq == last_seq);
  free(kind);
  wd_free(event);

  cbor_item_t *dup = wd_map(0);
  cbor_item_t *first_reply = NULL;
  assert(request(fd, 77, "session.create", dup, &first_reply) == 0);
  char *dup_id = wd_text_copy(wd_get(wd_get(first_reply, "session"), "id"));
  wd_free(first_reply);
  int before = session_count(fd);
  cbor_item_t *second_reply = NULL;
  assert(request(fd, 77, "session.create", wd_map(0), &second_reply) == 0);
  char *dup_id2 = wd_text_copy(wd_get(wd_get(second_reply, "session"), "id"));
  wd_free(second_reply);
  assert(dup_id != NULL && dup_id2 != NULL && strcmp(dup_id, dup_id2) == 0);
  int after = session_count(fd);
  assert(before == 2 && after == 2);
  free(dup_id);
  free(dup_id2);

  const int rounds = 2000;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int i = 0; i < rounds; i++) {
    cbor_item_t *result = NULL;
    if (request(fd, 1000 + (uint64_t)i, "session.list", wd_map(0), &result) != 0)
      return 1;
    wd_free(result);
  }
  double elapsed = seconds_since(&start);
  double rate = (double)rounds / elapsed;
  printf("m0 gate: %d round-trips in %.3f s = %.0f/s\n", rounds, elapsed, rate);
#if !defined(__SANITIZE_ADDRESS__)
  assert(rate >= 1000.0);
#endif

  close(fd);
  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(access(sock, F_OK) != 0);

  free(id);
  free(state);
  free(sock);
  printf("m0 gate passed\n");
  return 0;
}
