/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/util.h"
#include "net/pki.h"
#include "net/quic.h"
#include "proto/codec.h"

typedef struct {
  int opened;
  int frames;
  int closed;
  bool authed;
  char scope[16];
  char serial[WD_PKI_SERIAL_MAX];
} server_state;

typedef struct {
  int frames;
  uint16_t last_type;
  bool replied;
  bool ok;
  cbor_item_t *result;
} client_state;

static void server_open(void *ctx, wd_quic_stream *stream) {
  server_state *st = ctx;
  st->opened++;
  st->authed = wd_quic_stream_authenticated(stream);
  const char *scope = wd_quic_stream_scope(stream);
  snprintf(st->scope, sizeof(st->scope), "%s", scope != NULL ? scope : "");
  snprintf(st->serial, sizeof(st->serial), "%s", wd_quic_stream_serial(stream));
}

static void server_frame(void *ctx, wd_quic_stream *stream,
                         const wd_frame *frame) {
  server_state *st = ctx;
  st->frames++;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, frame->type, frame->sub, frame->seq, frame->len);
  assert(wd_quic_stream_write(stream, header, sizeof(header)) == 0);
  assert(wd_quic_stream_write(stream, frame->body, frame->len) == 0);
}

static void server_close(void *ctx, wd_quic_stream *stream) {
  server_state *st = ctx;
  (void)stream;
  st->closed++;
}

static void client_frame(void *ctx, const wd_frame *frame) {
  client_state *st = ctx;
  st->frames++;
  cbor_item_t *root = wd_decode(frame->body, frame->len);
  if (root == NULL) return;
  st->last_type = frame->type;
  if (frame->type == WD_FRAME_RESPONSE) {
    uint64_t id = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (wd_read_response(root, &id, &ok, &value)) {
      st->replied = true;
      st->ok = ok;
      if (st->result != NULL) wd_free(st->result);
      st->result = NULL;
      if (cbor_incref(value)) st->result = value;
    }
  }
  wd_free(root);
}

static void pump(wd_quic *server, wd_quic_client *client) {
  struct pollfd fds[2];
  nfds_t n = 0;
  if (server != NULL) {
    fds[n].fd = wd_quic_fd(server);
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    n++;
  }
  if (client != NULL) {
    fds[n].fd = wd_quic_client_fd(client);
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    n++;
  }
  if (n > 0) (void)poll(fds, n, 20);
  if (server != NULL) {
    (void)wd_quic_service(server, wu_now_ms());
    (void)wd_quic_flush(server);
  }
  if (client != NULL) {
    (void)wd_quic_client_service(client, wu_now_ms());
    (void)wd_quic_client_flush(client);
  }
}

static bool wait_state(wd_quic *server, wd_quic_client *client,
                       enum wd_quic_state want, uint64_t deadline_ms) {
  while (wu_now_ms() < deadline_ms) {
    if (wd_quic_client_state(client) == want) return true;
    if (wd_quic_client_state(client) == WD_QUIC_CLOSED && want != WD_QUIC_CLOSED)
      return false;
    pump(server, client);
  }
  return wd_quic_client_state(client) == want;
}

static bool wait_frames(wd_quic *server, wd_quic_client *client,
                        client_state *st, int wanted, uint64_t deadline_ms) {
  while (wu_now_ms() < deadline_ms) {
    if (st->frames >= wanted) return true;
    if (wd_quic_client_state(client) == WD_QUIC_CLOSED) return false;
    pump(server, client);
  }
  return st->frames >= wanted;
}

static int client_send(wd_quic_client *client, uint16_t type, uint64_t sub,
                       uint64_t seq, cbor_item_t *body) {
  size_t len = 0;
  uint8_t *encoded = wd_encode(body, &len);
  if (encoded == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, type, sub, seq, (uint32_t)len);
  int rc = wd_quic_client_write(client, header, sizeof(header));
  if (rc == 0) rc = wd_quic_client_write(client, encoded, len);
  free(encoded);
  if (rc == 0) rc = wd_quic_client_flush(client);
  return rc;
}

static void write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(text, file) >= 0);
  fclose(file);
}

int main(void) {
  char template[] = "/tmp/ward-quic-XXXXXX";
  char *dir = mkdtemp(template);
  assert(dir != NULL);
  char *err = NULL;
  wd_pki *pki = wd_pki_open(dir, &err);
  assert(pki != NULL);

  char fingerprint[65];
  assert(wd_pki_fingerprint(pki, fingerprint) == 0);

  server_state srv_state = {0};
  wd_quic_events events = {.stream_open = server_open,
                           .stream_frame = server_frame,
                           .stream_close = server_close};
  wd_quic_config config = {.bind = "127.0.0.1:0", .pki = pki};
  wd_quic *server = wd_quic_new(&config, &events, &srv_state, &err);
  if (server == NULL) {
    fprintf(stderr, "quic: %s\n", err != NULL ? err : "cannot listen");
    return 1;
  }
  int port = wd_quic_port(server);
  assert(port > 0);

  char key[512];
  char csr[512];
  snprintf(key, sizeof(key), "%s/device.key", dir);
  snprintf(csr, sizeof(csr), "%s/device.csr", dir);
  char *key_pem = NULL;
  char *csr_pem = NULL;
  assert(wd_pki_keypair(&key_pem, &csr_pem, err, 0) == 0);
  write_file(key, key_pem);
  char cert_pem[512];
  snprintf(cert_pem, sizeof(cert_pem), "%s/device.crt", dir);
  char *issued = NULL;
  char serial[WD_PKI_SERIAL_MAX] = {0};
  assert(wd_pki_issue(pki, csr_pem, WD_SCOPE_DESKTOP, &issued, serial, err, 0) ==
         0);
  write_file(cert_pem, issued);
  free(key_pem);
  free(csr_pem);
  free(issued);

  client_state state = {0};
  wd_quic_client_config cfg = {.cert_pem = cert_pem,
                               .key_pem = key,
                               .ca_pem = wd_pki_ca_cert(pki),
                               .pin_sha256 = fingerprint,
                               .server_name = "ward",
                               .frame = client_frame,
                               .ctx = &state};
  wd_quic_client *client = wd_quic_client_new(&cfg, &err);
  assert(client != NULL);
  assert(wd_quic_client_start(client, "127.0.0.1", (uint16_t)port,
                              wu_now_ms()) == 0);
  assert(wait_state(server, client, WD_QUIC_ESTABLISHED, wu_now_ms() + 5000));
  assert(wd_quic_client_verified(client) == 1);

  assert(client_send(client, WD_FRAME_HELLO, 0, 0, wd_hello()) == 0);
  assert(wait_frames(server, client, &state, 1, wu_now_ms() + 5000));
  assert(srv_state.opened == 1);
  assert(srv_state.frames == 1);
  assert(srv_state.authed);
  assert(strcmp(srv_state.scope, "desktop") == 0);
  assert(strcmp(srv_state.serial, serial) == 0);
  assert(state.last_type == WD_FRAME_HELLO);

  wd_quic_client_free(client);

  client_state anon = {0};
  wd_quic_client_config anon_cfg = {.ca_pem = wd_pki_ca_cert(pki),
                                    .pin_sha256 = fingerprint,
                                    .server_name = "ward",
                                    .frame = client_frame,
                                    .ctx = &anon};
  wd_quic_client *guest = wd_quic_client_new(&anon_cfg, &err);
  assert(guest != NULL);
  assert(wd_quic_client_start(guest, "127.0.0.1", (uint16_t)port,
                              wu_now_ms()) == 0);
  assert(wait_state(server, guest, WD_QUIC_ESTABLISHED, wu_now_ms() + 5000));
  assert(client_send(guest, WD_FRAME_HELLO, 0, 0, wd_hello()) == 0);
  assert(wait_frames(server, guest, &anon, 1, wu_now_ms() + 5000));
  assert(srv_state.opened == 2);
  assert(!srv_state.authed);
  assert(srv_state.scope[0] == '\0');
  assert(srv_state.serial[0] == '\0');
  wd_quic_client_free(guest);

  assert(wd_pki_revoke(pki, serial, err, 0) == 0);
  client_state revoked = {0};
  wd_quic_client_config revoked_cfg = {.cert_pem = cert_pem,
                                       .key_pem = key,
                                       .ca_pem = wd_pki_ca_cert(pki),
                                       .pin_sha256 = fingerprint,
                                       .server_name = "ward",
                                       .frame = client_frame,
                                       .ctx = &revoked};
  wd_quic_client *banned = wd_quic_client_new(&revoked_cfg, &err);
  assert(banned != NULL);
  assert(wd_quic_client_start(banned, "127.0.0.1", (uint16_t)port,
                              wu_now_ms()) == 0);
  uint64_t deadline = wu_now_ms() + 4000;
  while (wu_now_ms() < deadline && srv_state.opened == 2)
    pump(server, banned);
  assert(srv_state.opened == 2);
  wd_quic_client_free(banned);

  wd_quic_free(server);
  wd_pki_free(pki);
  printf("quic tests passed\n");
  return 0;
}
