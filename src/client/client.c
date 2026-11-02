/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "client/client.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/util.h"
#include "proto/codec.h"

static int read_full(int fd, void *buf, size_t len) {
  size_t off = 0;
  uint8_t *p = buf;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n == 0) return -1;
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static int write_full(int fd, const void *buf, size_t len) {
  return wd_write_all(fd, buf, len);
}

static void set_read_timeout(int fd, int seconds) {
  struct timeval timeout = {seconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static int connect_tcp(const char *target) {
  const char *colon = strrchr(target, ':');
  if (colon == NULL) return -1;
  char host[64];
  size_t host_len = (size_t)(colon - target);
  if (host_len == 0 || host_len >= sizeof(host)) return -1;
  memcpy(host, target, host_len);
  host[host_len] = '\0';
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)atoi(colon + 1));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "ward: no daemon at %s: %s\n", target, strerror(errno));
    close(fd);
    return -1;
  }
  return fd;
}

int wd_client_connect(wd_client *client, const char *path) {
  if (strncmp(path, "tcp://", 6) == 0) {
    int tcp_fd = connect_tcp(path + 6);
    if (tcp_fd < 0) return -1;
    set_read_timeout(tcp_fd, 120);
    client->fd = tcp_fd;
    client->next_id = 1;
    return 0;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    fprintf(stderr, "ward: socket path too long\n");
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "ward: no daemon at %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }
  set_read_timeout(fd, 120);
  client->fd = fd;
  client->next_id = 1;
  return 0;
}

static void dump_sent(const char *method, cbor_item_t *item) {
  if (getenv("WARD_DUMP_FRAMES") == NULL) return;
  size_t len = 0;
  unsigned char *bytes = NULL;
  size_t written = cbor_serialize_alloc(item, &bytes, &len);
  if (written == 0 || bytes == NULL) return;
  if (strcmp(method, "frame") == 0) {
    fprintf(stderr, "sent frame len=%zu body=", len);
    for (size_t i = 0; i < len; i++) fprintf(stderr, "%02x", bytes[i]);
    fputc('\n', stderr);
  } else {
    fprintf(stderr, "sent %s\n", method);
  }
  free(bytes);
}

int wd_client_send_item(wd_client *client, cbor_item_t *item) {
  if (getenv("WARD_DUMP_FRAMES") != NULL) dump_sent("frame", item);
  size_t len = 0;
  uint8_t *body = wd_encode(item, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, WD_FRAME_REQUEST, 0, 0, (uint32_t)len);
  int rc = write_full(client->fd, header, sizeof(header));
  if (rc == 0) rc = write_full(client->fd, body, len);
  free(body);
  return rc;
}

static void dump_frame(const wd_frame *frame, const uint8_t *body) {
  if (getenv("WARD_DUMP_FRAMES") == NULL) return;
  fprintf(stderr, "frame type=%u sub=%llu seq=%llu len=%zu body=", frame->type,
          (unsigned long long)frame->sub, (unsigned long long)frame->seq,
          (size_t)frame->len);
  for (size_t i = 0; i < frame->len; i++) fprintf(stderr, "%02x", body[i]);
  fputc('\n', stderr);
}

uint8_t *wd_client_read_frame(int fd, wd_frame *frame) {
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
  frame->body = body;
  dump_frame(frame, body);
  return body;
}

int wd_client_handshake(wd_client *client) {
  cbor_item_t *hello = wd_hello();
  size_t len = 0;
  uint8_t *body = wd_encode(hello, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, WD_FRAME_HELLO, 0, 0, (uint32_t)len);
  int rc = write_full(client->fd, header, sizeof(header));
  if (rc == 0) rc = write_full(client->fd, body, len);
  free(body);
  if (rc != 0) return -1;
  wd_frame frame;
  uint8_t *frame_body = wd_client_read_frame(client->fd, &frame);
  if (frame_body == NULL) return -1;
  cbor_item_t *root = wd_decode(frame_body, frame.len);
  free(frame_body);
  if (root == NULL) return -1;
  unsigned version = 0;
  uint64_t uid = 0;
  int ok = frame.type == WD_FRAME_WELCOME &&
           wd_read_welcome(root, &version, &uid) && version == WD_PROTOCOL_VERSION;
  if (!ok) {
    char *name = wd_text_copy(wd_get(root, "error"));
    if (name != NULL) free(name);
    fprintf(stderr, "ward: handshake refused\n");
  }
  wd_free(root);
  return ok ? 0 : -1;
}

int wd_client_call(wd_client *client, const char *method,
                          cbor_item_t *params, cbor_item_t **result) {
  if (getenv("WARD_DUMP_FRAMES") != NULL) dump_sent(method, params);
  uint64_t id = client->next_id++;
  if (wd_client_send_item(client, wd_request(id, method, params)) != 0) return -1;
  for (;;) {
    wd_frame frame;
    uint8_t *body = wd_client_read_frame(client->fd, &frame);
    if (body == NULL) {
      fprintf(stderr, "ward: no answer from the daemon\n");
      return -1;
    }
    cbor_item_t *root = wd_decode(body, frame.len);
    free(body);
    if (root == NULL) return -1;
    if (frame.type == WD_FRAME_EVENT) {
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (wd_read_event(root, &kind, &payload) && strcmp(kind, "replay.end") != 0)
        continue;
      free(kind);
      wd_free(root);
      continue;
    }
    if (frame.type != WD_FRAME_RESPONSE) {
      wd_free(root);
      return -1;
    }
    uint64_t got = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (!wd_read_response(root, &got, &ok, &value) || got != id) {
      wd_free(root);
      return -1;
    }
    if (!ok) {
      char *name = wd_text_copy(wd_get(value, "name"));
      char *message = wd_text_copy(wd_get(value, "message"));
      fprintf(stderr, "ward: %s: %s\n", name != NULL ? name : "error",
              message != NULL ? message : "");
      free(name);
      free(message);
      wd_free(root);
      return -1;
    }
    (void)cbor_incref(value);
    wd_free(root);
    *result = value;
    return 0;
  }
}

