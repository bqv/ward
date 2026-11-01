/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "core/util.h"
#include "net/pki.h"
#include "net/quic.h"
#include "proto/codec.h"
#include "proto/frame.h"

typedef struct {
  bool welcome;
  bool response;
  bool ok;
  cbor_item_t *result;
} answer;

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

static int uds_send(int fd, uint16_t type, uint64_t sub, uint64_t seq,
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

static cbor_item_t *uds_next(int fd, wd_frame *frame) {
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
  frame->len = len;
  cbor_item_t *root = wd_decode(body, len);
  free(body);
  return root;
}

static int uds_request(int fd, uint64_t id, const char *method,
                       cbor_item_t *params, cbor_item_t **result,
                       bool *ok_out) {
  if (uds_send(fd, WD_FRAME_REQUEST, 0, 0, wd_request(id, method, params)) != 0)
    return -1;
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = uds_next(fd, &frame);
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
    if (!cbor_incref(value)) {
      wd_free(root);
      return -1;
    }
    wd_free(root);
    *result = value;
    if (ok_out != NULL) *ok_out = ok;
    return 0;
  }
}

static void quic_answer(void *ctx, const wd_frame *frame) {
  answer *state = ctx;
  cbor_item_t *root = wd_decode(frame->body, frame->len);
  if (root == NULL) return;
  if (frame->type == WD_FRAME_WELCOME) state->welcome = true;
  if (frame->type == WD_FRAME_RESPONSE) {
    uint64_t id = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (wd_read_response(root, &id, &ok, &value)) {
      state->response = true;
      state->ok = ok;
      if (state->result != NULL) wd_free(state->result);
      state->result = NULL;
      if (cbor_incref(value)) state->result = value;
    }
  }
  wd_free(root);
}

static void pump(wd_quic_client *client) {
  struct pollfd pfd = {.fd = wd_quic_client_fd(client), .events = POLLIN};
  (void)poll(&pfd, 1, 20);
  (void)wd_quic_client_service(client, wd_now_ms());
  (void)wd_quic_client_flush(client);
}

static bool wait_state(wd_quic_client *client, enum wd_quic_state want,
                       uint64_t deadline_ms) {
  while (wd_now_ms() < deadline_ms) {
    if (wd_quic_client_state(client) == want) return true;
    if (wd_quic_client_state(client) == WD_QUIC_CLOSED) return false;
    pump(client);
  }
  return wd_quic_client_state(client) == want;
}

static bool wait_flag(wd_quic_client *client, const bool *flag, bool want,
                      uint64_t deadline_ms) {
  while (wd_now_ms() < deadline_ms) {
    if (*flag == want) return true;
    if (wd_quic_client_state(client) == WD_QUIC_CLOSED) return *flag == want;
    pump(client);
  }
  return *flag == want;
}

static int quic_send(wd_quic_client *client, uint16_t type, uint64_t sub,
                     uint64_t seq, cbor_item_t *item) {
  size_t len = 0;
  uint8_t *body = wd_encode(item, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, type, sub, seq, (uint32_t)len);
  int rc = wd_quic_client_write(client, header, sizeof(header));
  if (rc == 0) rc = wd_quic_client_write(client, body, len);
  free(body);
  if (rc == 0) rc = wd_quic_client_flush(client);
  return rc;
}

static void write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(text, file) >= 0);
  fclose(file);
}

static wd_quic_client *connect_client(const char *host, uint16_t port,
                                      const char *pin, const char *ca,
                                      const char *cert, const char *key,
                                      answer *state) {
  wd_quic_client_config cfg = {.cert_pem = cert,
                               .key_pem = key,
                               .ca_pem = ca,
                               .pin_sha256 = pin,
                               .server_name = "ward",
                               .frame = quic_answer,
                               .ctx = state};
  char *err = NULL;
  wd_quic_client *client = wd_quic_client_new(&cfg, &err);
  if (client == NULL) {
    fprintf(stderr, "gate: quic client: %s\n", err != NULL ? err : "failed");
    free(err);
    return NULL;
  }
  if (wd_quic_client_start(client, host, port, wd_now_ms()) != 0 ||
      !wait_state(client, WD_QUIC_ESTABLISHED, wd_now_ms() + 5000)) {
    fprintf(stderr, "gate: quic handshake failed\n");
    wd_quic_client_free(client);
    return NULL;
  }
  return client;
}

static bool quic_hello(wd_quic_client *client, answer *state) {
  if (quic_send(client, WD_FRAME_HELLO, 0, 0, wd_hello()) != 0) return false;
  return wait_flag(client, &state->welcome, true, wd_now_ms() + 5000);
}

static bool quic_call(wd_quic_client *client, answer *state, uint64_t id,
                      const char *method, cbor_item_t *params) {
  if (quic_send(client, WD_FRAME_REQUEST, 0, 0, wd_request(id, method, params)) !=
      0)
    return false;
  return wait_flag(client, &state->response, true, wd_now_ms() + 5000);
}

static char *error_name(cbor_item_t *result) {
  return result != NULL ? wd_text_copy(wd_get(result, "name")) : NULL;
}

static bool enrol(const char *host, uint16_t port, const char *pin,
                  const char *token, const char *csr, char **cert_out,
                  char **ca_out, char serial[WD_PKI_SERIAL_MAX], bool *ok_out) {
  answer state = {0};
  wd_quic_client *client = connect_client(host, port, pin, NULL, NULL, NULL,
                                          &state);
  if (client == NULL) return false;
  bool reached = quic_hello(client, &state);
  if (reached) {
    cbor_item_t *params = wd_map(2);
    wd_put(params, "token", wd_text(token));
    wd_put(params, "csr", wd_text(csr));
    reached = quic_call(client, &state, 1, "device.enrol", params);
  }
  if (ok_out != NULL) *ok_out = reached && state.ok;
  if (reached && state.ok && state.result != NULL) {
    *cert_out = wd_text_copy(wd_get(state.result, "cert"));
    *ca_out = wd_text_copy(wd_get(state.result, "ca"));
    char *text = wd_text_copy(wd_get(state.result, "serial"));
    if (text != NULL) snprintf(serial, WD_PKI_SERIAL_MAX, "%s", text);
    free(text);
  }
  if (state.result != NULL) wd_free(state.result);
  wd_quic_client_free(client);
  return reached;
}

int main(void) {
  alarm(180);
  const char *bin = getenv("WARD_BIN");
  if (bin == NULL) {
    fprintf(stderr, "gate: WARD_BIN is not set\n");
    return 1;
  }
  char root[] = "/tmp/ward-quic-XXXXXX";
  if (mkdtemp(root) == NULL) return 1;
  char state_dir[256];
  char sock[108];
  snprintf(state_dir, sizeof(state_dir), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    setenv("WARD_QUIC_BIND", "127.0.0.1:0", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(bin, "ward", "up", "--socket", sock, "--state", state_dir,
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
  if (strncmp(line, "ready ", 6) != 0) {
    fprintf(stderr, "gate: daemon did not become ready: %s\n", line);
    kill(pid, SIGKILL);
    return 1;
  }
  used = 0;
  while (used + 1 < sizeof(line)) {
    ssize_t n = read(ready[0], line + used, 1);
    if (n <= 0) break;
    if (line[used++] == '\n') break;
  }
  line[used] = '\0';
  close(ready[0]);
  char endpoint[128];
  char fingerprint[65];
  if (sscanf(line, "quic %127s %64s", endpoint, fingerprint) != 2) {
    fprintf(stderr, "gate: no quic endpoint: %s\n", line);
    kill(pid, SIGKILL);
    return 1;
  }
  char *colon = strrchr(endpoint, ':');
  assert(colon != NULL);
  *colon = '\0';
  uint16_t port = (uint16_t)atoi(colon + 1);
  const char *host = endpoint;
  fprintf(stderr, "gate: quic on %s:%u\n", host, port);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "gate: connect: %s\n", strerror(errno));
    kill(pid, SIGKILL);
    return 1;
  }
  assert(uds_send(fd, WD_FRAME_HELLO, 0, 0, wd_hello()) == 0);
  wd_frame frame;
  cbor_item_t *welcome = uds_next(fd, &frame);
  assert(welcome != NULL);
  assert(frame.type == WD_FRAME_WELCOME);
  wd_free(welcome);

  cbor_item_t *paired = NULL;
  cbor_item_t *pair_params = wd_map(1);
  wd_put(pair_params, "scope", wd_text("phone"));
  assert(uds_request(fd, 1, "device.pair", pair_params, &paired, NULL) == 0);
  char *token = wd_text_copy(wd_get(paired, "token"));
  char *pinned = wd_text_copy(wd_get(paired, "fingerprint"));
  char *paired_endpoint = wd_text_copy(wd_get(paired, "endpoint"));
  char *paired_scope = wd_text_copy(wd_get(paired, "scope"));
  uint64_t expires = 0;
  assert(wd_uint_of(wd_get(paired, "expires_ms"), &expires));
  assert(token != NULL && pinned != NULL && paired_endpoint != NULL);
  assert(strcmp(pinned, fingerprint) == 0);
  assert(strcmp(paired_scope, "phone") == 0);
  assert(expires > wd_now_ms());
  char *paired_colon = strrchr(paired_endpoint, ':');
  assert(paired_colon != NULL && (uint16_t)atoi(paired_colon + 1) == port);
  wd_free(paired);

  char key_path[256];
  char cert_path[256];
  char ca_path[256];
  snprintf(key_path, sizeof(key_path), "%s/device.key", root);
  snprintf(cert_path, sizeof(cert_path), "%s/device.crt", root);
  snprintf(ca_path, sizeof(ca_path), "%s/ca.crt", root);
  char failure[256] = {0};
  char *key_pem = NULL;
  char *csr_pem = NULL;
  assert(wd_pki_keypair(&key_pem, &csr_pem, failure, sizeof(failure)) == 0);
  write_file(key_path, key_pem);
  free(key_pem);

  char serial[WD_PKI_SERIAL_MAX] = {0};
  char *device_cert = NULL;
  char *ca_pem = NULL;
  bool enrol_ok = false;
  assert(enrol(host, port, fingerprint, token, csr_pem, &device_cert, &ca_pem,
               serial, &enrol_ok));
  assert(enrol_ok && device_cert != NULL && ca_pem != NULL);
  write_file(cert_path, device_cert);
  write_file(ca_path, ca_pem);
  free(device_cert);
  free(ca_pem);

  bool spent_ok = true;
  assert(enrol(host, port, fingerprint, token, csr_pem, &device_cert, &ca_pem,
               serial, &spent_ok));
  assert(!spent_ok);
  free(csr_pem);
  free(token);
  free(pinned);
  free(paired_endpoint);
  free(paired_scope);

  answer state = {0};
  wd_quic_client *client =
      connect_client(host, port, fingerprint, ca_path, cert_path, key_path,
                     &state);
  assert(client != NULL);
  assert(quic_hello(client, &state));
  cbor_item_t *create = wd_map(1);
  wd_put(create, "cwd", wd_text(root));
  assert(quic_call(client, &state, 2, "session.create", create));
  assert(state.ok && state.result != NULL);
  cbor_item_t *session = wd_get(state.result, "session");
  char *session_id = wd_text_copy(wd_get(session, "id"));
  assert(session_id != NULL && strlen(session_id) == 16);
  free(session_id);
  wd_free(state.result);
  state.result = NULL;
  wd_quic_client_free(client);

  cbor_item_t *readonly = NULL;
  cbor_item_t *readonly_params = wd_map(1);
  wd_put(readonly_params, "scope", wd_text("readonly"));
  assert(uds_request(fd, 3, "device.pair", readonly_params, &readonly, NULL) ==
         0);
  char *readonly_token = wd_text_copy(wd_get(readonly, "token"));
  wd_free(readonly);
  char *readonly_key = NULL;
  char *readonly_csr = NULL;
  assert(wd_pki_keypair(&readonly_key, &readonly_csr, failure,
                        sizeof(failure)) == 0);
  char readonly_key_path[256];
  char readonly_cert_path[256];
  snprintf(readonly_key_path, sizeof(readonly_key_path), "%s/readonly.key",
           root);
  snprintf(readonly_cert_path, sizeof(readonly_cert_path), "%s/readonly.crt",
           root);
  write_file(readonly_key_path, readonly_key);
  free(readonly_key);
  char readonly_serial[WD_PKI_SERIAL_MAX] = {0};
  char *readonly_cert = NULL;
  char *readonly_ca = NULL;
  bool readonly_ok = false;
  assert(enrol(host, port, fingerprint, readonly_token, readonly_csr,
               &readonly_cert, &readonly_ca, readonly_serial, &readonly_ok));
  assert(readonly_ok && readonly_cert != NULL);
  write_file(readonly_cert_path, readonly_cert);
  free(readonly_cert);
  free(readonly_ca);
  free(readonly_csr);
  free(readonly_token);

  answer denied = {0};
  wd_quic_client *restricted =
      connect_client(host, port, fingerprint, ca_path, readonly_cert_path,
                     readonly_key_path, &denied);
  assert(restricted != NULL);
  assert(quic_hello(restricted, &denied));
  cbor_item_t *prompt = wd_map(2);
  wd_put(prompt, "id", wd_text("0123456789abcdef"));
  wd_put(prompt, "text", wd_text("hello"));
  assert(quic_call(restricted, &denied, 4, "session.prompt", prompt));
  assert(!denied.ok && denied.result != NULL);
  char *denied_name = error_name(denied.result);
  assert(denied_name != NULL);
  assert(strcmp(denied_name, "permission_denied") == 0);
  free(denied_name);
  wd_free(denied.result);
  denied.result = NULL;
  wd_quic_client_free(restricted);

  cbor_item_t *revoked = NULL;
  cbor_item_t *revoke_params = wd_map(1);
  wd_put(revoke_params, "serial", wd_text(readonly_serial));
  assert(uds_request(fd, 5, "device.revoke", revoke_params, &revoked, NULL) ==
         0);
  wd_free(revoked);

  answer banned = {0};
  wd_quic_client *banned_client =
      connect_client(host, port, fingerprint, ca_path, readonly_cert_path,
                     readonly_key_path, &banned);
  if (banned_client != NULL) {
    bool reached = quic_hello(banned_client, &banned);
    assert(!reached);
    wd_quic_client_free(banned_client);
  }
  if (banned.result != NULL) wd_free(banned.result);

  cbor_item_t *listed = NULL;
  assert(uds_request(fd, 6, "device.list", wd_map(0), &listed, NULL) == 0);
  cbor_item_t *devices = wd_get(listed, "devices");
  size_t count = cbor_array_size(devices);
  assert(count == 2);
  size_t revoked_count = 0;
  cbor_item_t **items = cbor_array_handle(devices);
  for (size_t i = 0; i < count; i++) {
    bool is_revoked = false;
    assert(wd_bool_of(wd_get(items[i], "revoked"), &is_revoked));
    if (is_revoked) revoked_count++;
  }
  assert(revoked_count == 1);
  wd_free(listed);

  close(fd);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  printf("gate passed: a paired device over quic created a session\n");
  return 0;
}
