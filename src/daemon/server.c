/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "daemon/server.h"

#include "core/turn.h"
#include "wasm/host.h"

#include <cbor.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "core/session.h"
#include "core/util.h"
#include "proto/codec.h"
#include "proto/frame.h"

#define WD_MAX_CONNS 256
#define WD_MAX_SUBS 16
#define WD_CACHE 16
#define WD_MAX_OUT (8u << 20)
#define WD_HANDSHAKE_MS 5000u

typedef struct wd_out {
  struct wd_out *next;
  size_t len;
  size_t off;
  uint8_t *data;
} wd_out;

typedef struct {
  uint64_t id;
  char session_id[WD_ID_LEN];
} wd_sub;

typedef struct {
  bool used;
  uint64_t id;
  uint8_t *body;
  size_t len;
} wd_cache;

typedef struct wd_conn {
  int fd;
  bool hello;
  bool closing;
  uint64_t deadline_ms;
  uint8_t *in;
  size_t in_len;
  wd_out *out_head;
  wd_out *out_tail;
  size_t out_bytes;
  wd_sub subs[WD_MAX_SUBS];
  size_t subs_len;
  uint64_t next_sub;
  wd_cache cache[WD_CACHE];
  size_t cache_at;
} wd_conn;

typedef struct {
  int listen_fd;
  int epoll_fd;
  int signal_fd;
  char *socket_path;
  char *state_dir;
  char *lock_path;
  hs_store store;
  wd_engine *engine;
  wd_turn *turn;
  wd_conn *conns[WD_MAX_CONNS];
  bool stop;
} wd_server;

static int token_listen;
static int token_signal;

static void conn_close(wd_server *srv, wd_conn *conn);
static int conn_arm(wd_server *srv, wd_conn *conn);
static int conn_flush(wd_conn *conn);
static int conn_read(wd_server *srv, wd_conn *conn);
static void handle_frame(wd_server *srv, wd_conn *conn, const wd_frame *frame);
static void handle_request(wd_server *srv, wd_conn *conn, uint64_t id,
                           const char *method, cbor_item_t *params);
static void handle_control(wd_server *srv, wd_conn *conn, cbor_item_t *root);

static uint8_t *encode_item(cbor_item_t *item, size_t *len, bool take) {
  size_t need = cbor_serialized_size(item);
  if (need == 0) {
    if (take) wd_free(item);
    return NULL;
  }
  uint8_t *buf = malloc(need);
  if (buf == NULL) {
    if (take) wd_free(item);
    return NULL;
  }
  size_t written = cbor_serialize(item, buf, need);
  if (take) wd_free(item);
  if (written == 0) {
    free(buf);
    return NULL;
  }
  *len = written;
  return buf;
}

static int conn_queue(wd_conn *conn, uint16_t type, uint64_t sub, uint64_t seq,
                      const uint8_t *body, size_t len) {
  if (len == 0 || len > WD_MAX_BODY) return -1;
  size_t total = WD_FRAME_HEADER + len;
  if (conn->out_bytes + total > WD_MAX_OUT) return -1;
  wd_out *out = calloc(1, sizeof(*out));
  if (out == NULL) return -1;
  out->data = malloc(total);
  if (out->data == NULL) {
    free(out);
    return -1;
  }
  out->len = total;
  wd_frame_header(out->data, type, sub, seq, (uint32_t)len);
  memcpy(out->data + WD_FRAME_HEADER, body, len);
  if (conn->out_tail != NULL)
    conn->out_tail->next = out;
  else
    conn->out_head = out;
  conn->out_tail = out;
  conn->out_bytes += total;
  return 0;
}

static int conn_send_item(wd_conn *conn, uint16_t type, uint64_t sub,
                          uint64_t seq, cbor_item_t *item, bool take) {
  size_t len = 0;
  uint8_t *body = encode_item(item, &len, take);
  if (body == NULL) return -1;
  int rc = conn_queue(conn, type, sub, seq, body, len);
  free(body);
  return rc;
}

static void cache_store(wd_conn *conn, uint64_t id, const uint8_t *body,
                        size_t len) {
  wd_cache *slot = &conn->cache[conn->cache_at % WD_CACHE];
  conn->cache_at++;
  free(slot->body);
  slot->body = malloc(len);
  if (slot->body == NULL) {
    slot->used = false;
    return;
  }
  memcpy(slot->body, body, len);
  slot->len = len;
  slot->id = id;
  slot->used = true;
}

static wd_cache *cache_find(wd_conn *conn, uint64_t id) {
  for (size_t i = 0; i < WD_CACHE; i++)
    if (conn->cache[i].used && conn->cache[i].id == id) return &conn->cache[i];
  return NULL;
}

static int conn_send_cached(wd_conn *conn, uint64_t id, cbor_item_t *response) {
  size_t len = 0;
  uint8_t *body = encode_item(response, &len, true);
  if (body == NULL) return -1;
  cache_store(conn, id, body, len);
  int rc = conn_queue(conn, WD_FRAME_RESPONSE, 0, 0, body, len);
  free(body);
  return rc;
}

static int conn_send_error(wd_conn *conn, uint64_t id, wd_error err,
                           const char *message) {
  return conn_send_item(conn, WD_FRAME_RESPONSE, 0, 0,
                        wd_response_err(id, err, message), true);
}

static void conn_broadcast(wd_server *srv, hs_session *session,
                           const char *kind, uint64_t seq,
                           cbor_item_t *payload) {
  cbor_item_t *event = wd_event(kind, payload);
  size_t len = 0;
  uint8_t *body = encode_item(event, &len, true);
  if (body == NULL) return;
  for (size_t i = 0; i < WD_MAX_CONNS; i++) {
    wd_conn *conn = srv->conns[i];
    if (conn == NULL || !conn->hello) continue;
    for (size_t j = 0; j < conn->subs_len; j++) {
      if (strcmp(conn->subs[j].session_id, session->id) != 0) continue;
      if (conn_queue(conn, WD_FRAME_EVENT, conn->subs[j].id, seq, body, len) != 0)
        conn->closing = true;
      conn_arm(srv, conn);
    }
  }
  free(body);
}

static void respond_internal(wd_server *srv, wd_conn *conn, uint64_t id,
                             const char *where) {
  char message[128];
  snprintf(message, sizeof(message), "%s", where);
  conn_send_error(conn, id, WD_ERR_INTERNAL, message);
  conn_arm(srv, conn);
}

static bool valid_cwd(const char *path) {
  if (path == NULL || path[0] != '/') return false;
  struct stat sb;
  return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static void handle_session_create(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *cwd = wd_text_copy(wd_get(params, "cwd"));
  char *model = wd_text_copy(wd_get(params, "model"));
  char buffer[PATH_MAX];
  if (cwd == NULL) {
    if (getcwd(buffer, sizeof(buffer)) == NULL) {
      free(model);
      respond_internal(srv, conn, id, "getcwd failed");
      return;
    }
    cwd = strdup(buffer);
  }
  if (cwd == NULL || !valid_cwd(cwd)) {
    free(cwd);
    free(model);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "cwd is not a directory");
    conn_arm(srv, conn);
    return;
  }
  if (model != NULL && model[0] == '\0') {
    free(model);
    model = NULL;
  }
  hs_session *session = NULL;
  if (hs_create(&srv->store, cwd, model, &session) != 0) {
    free(cwd);
    free(model);
    respond_internal(srv, conn, id, "cannot create session");
    return;
  }
  free(cwd);
  free(model);

  cbor_item_t *logged = wd_map(1);
  wd_put(logged, "session", hs_to_cbor(session));
  uint64_t seq = 0;
  if (hs_emit(session, "session.updated", logged, &seq) != 0) {
    respond_internal(srv, conn, id, "cannot append session event");
    return;
  }
  cbor_item_t *event = wd_map(1);
  wd_put(event, "session", hs_to_cbor(session));
  conn_broadcast(srv, session, "session.updated", seq, event);

  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", hs_to_cbor(session));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_list(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  uint64_t limit = 100;
  if (wd_get(params, "limit") != NULL) {
    if (!wd_uint_of(wd_get(params, "limit"), &limit) || limit == 0 ||
        limit > 1000) {
      conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "limit must be 1..1000");
      conn_arm(srv, conn);
      return;
    }
  }
  size_t count = srv->store.len < limit ? srv->store.len : (size_t)limit;
  cbor_item_t *list = wd_array(count);
  for (size_t i = 0; i < count; i++) {
    cbor_item_t *entry = hs_to_cbor(srv->store.items[i]);
    bool pushed = cbor_array_push(list, entry);
    (void)pushed;
    cbor_decref(&entry);
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "sessions", list);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_get(wd_server *srv, wd_conn *conn, uint64_t id,
                               cbor_item_t *params) {
  char *want = wd_text_copy(wd_get(params, "id"));
  if (want == NULL) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "id is required");
    conn_arm(srv, conn);
    return;
  }
  hs_session *session = hs_find(&srv->store, want);
  free(want);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", hs_to_cbor(session));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

typedef struct {
  wd_server *srv;
  wd_conn *conn;
  uint64_t sub;
} replay_ctx;

static int replay_event(void *ctx, uint64_t seq, cbor_item_t *event) {
  replay_ctx *rc = ctx;
  if (conn_send_item(rc->conn, WD_FRAME_EVENT, rc->sub, seq, event, false) != 0)
    return 1;
  conn_arm(rc->srv, rc->conn);
  return 0;
}

static void replay_into(wd_server *srv, wd_conn *conn, hs_session *session,
                        uint64_t sub, uint64_t from) {
  uint64_t to = session->last_seq;
  replay_ctx rc = {.srv = srv, .conn = conn, .sub = sub};
  hs_replay(session, from, replay_event, &rc);
  cbor_item_t *done = wd_map(2);
  wd_put(done, "from", wd_uint(from));
  wd_put(done, "to", wd_uint(to));
  conn_send_item(conn, WD_FRAME_EVENT, sub, to, wd_event("replay.end", done),
                 true);
  conn_arm(srv, conn);
}

static void handle_session_resume(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *want = wd_text_copy(wd_get(params, "id"));
  if (want == NULL) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "id is required");
    conn_arm(srv, conn);
    return;
  }
  uint64_t from = 0;
  bool have_from = false;
  if (wd_get(params, "from") != NULL) {
    if (!wd_uint_of(wd_get(params, "from"), &from)) {
      free(want);
      conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "from must be a uint");
      conn_arm(srv, conn);
      return;
    }
    have_from = true;
  }
  hs_session *session = hs_find(&srv->store, want);
  free(want);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  if (conn->subs_len == WD_MAX_SUBS) {
    conn_send_error(conn, id, WD_ERR_BUSY, "too many subscriptions");
    conn_arm(srv, conn);
    return;
  }
  uint64_t sub = conn->next_sub++;
  if (sub == 0) sub = conn->next_sub++;
  conn->subs[conn->subs_len].id = sub;
  snprintf(conn->subs[conn->subs_len].session_id, WD_ID_LEN, "%s", session->id);
  conn->subs_len++;

  cbor_item_t *result = wd_map(3);
  wd_put(result, "session", hs_to_cbor(session));
  wd_put(result, "sub", wd_uint(sub));
  wd_put(result, "last_seq", wd_uint(session->last_seq));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
  if (have_from) replay_into(srv, conn, session, sub, from);
}

typedef struct {
  wd_server *srv;
  hs_session *session;
} wd_turn_ctx;

static cbor_item_t *dup_item(cbor_item_t *item) {
  size_t need = cbor_serialized_size(item);
  if (need == 0) return NULL;
  uint8_t *buf = malloc(need);
  if (buf == NULL) return NULL;
  size_t written = cbor_serialize(item, buf, need);
  cbor_item_t *copy = written == 0 ? NULL : wd_decode(buf, written);
  free(buf);
  return copy;
}

static int turn_emit(void *ctx, const char *event, cbor_item_t *payload,
                     char **err) {
  wd_turn_ctx *tc = ctx;
  cbor_item_t *logged = payload != NULL ? dup_item(payload) : wd_map(0);
  uint64_t seq = 0;
  if (logged == NULL || hs_emit(tc->session, event, logged, &seq) != 0) {
    *err = strdup("cannot append the turn event");
    return -1;
  }
  cbor_item_t *broadcast = payload != NULL ? dup_item(payload) : wd_map(0);
  if (broadcast != NULL)
    conn_broadcast(tc->srv, tc->session, event, seq, broadcast);
  return 0;
}

static wd_turn *server_turn(wd_server *srv, char **err) {
  if (srv->turn != NULL) return srv->turn;
  if (srv->engine == NULL) srv->engine = wd_engine_new(srv->state_dir);
  if (srv->engine == NULL) {
    *err = strdup("cannot create the wasm engine");
    return NULL;
  }
  const char *dir = getenv("WARD_MODULE_DIR");
  char path[PATH_MAX];
  if (dir == NULL) {
    snprintf(path, sizeof(path), "%s/modules", srv->state_dir);
    dir = path;
  }
  srv->turn = wd_turn_new(srv->engine, dir, err);
  return srv->turn;
}

static void handle_session_prompt(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *text = wd_text_copy(wd_get(params, "text"));
  hs_session *session =
      session_id != NULL ? hs_find(&srv->store, session_id) : NULL;
  if (session == NULL) {
    free(session_id);
    free(text);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  if (text == NULL || text[0] == '\0') {
    free(session_id);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "text is empty");
    conn_arm(srv, conn);
    return;
  }
  char *err = NULL;
  wd_turn *turn = server_turn(srv, &err);
  if (turn == NULL) {
    free(session_id);
    free(text);
    respond_internal(srv, conn, id, err != NULL ? err : "no modules");
    free(err);
    return;
  }
  wd_turn_ctx tc = {srv, session};
  int rc = wd_turn_prompt(turn, text, turn_emit, &tc, &err);
  free(session_id);
  free(text);
  if (rc != 0) {
    respond_internal(srv, conn, id, err != NULL ? err : "turn failed");
    free(err);
    return;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", wd_text(session->id));
  conn_send_cached(conn, id, wd_response_ok(id, result));
}

static void handle_request(wd_server *srv, wd_conn *conn, uint64_t id,
                           const char *method, cbor_item_t *params) {
  if (strcmp(method, "session.create") == 0) {
    handle_session_create(srv, conn, id, params);
  } else if (strcmp(method, "session.list") == 0) {
    handle_session_list(srv, conn, id, params);
  } else if (strcmp(method, "session.get") == 0) {
    handle_session_get(srv, conn, id, params);
  } else if (strcmp(method, "session.resume") == 0) {
    handle_session_resume(srv, conn, id, params);
  } else if (strcmp(method, "session.prompt") == 0) {
    handle_session_prompt(srv, conn, id, params);
  } else {
    conn_send_error(conn, id, WD_ERR_NOT_SUPPORTED, "method not served yet");
    conn_arm(srv, conn);
  }
}

static void handle_control(wd_server *srv, wd_conn *conn, cbor_item_t *root) {
  uint64_t sub = 0;
  if (!wd_uint_of(wd_get(root, "replay"), &sub) || sub == 0) return;
  uint64_t from = 0;
  if (!wd_uint_of(wd_get(root, "from"), &from)) return;
  for (size_t i = 0; i < conn->subs_len; i++) {
    if (conn->subs[i].id != sub) continue;
    hs_session *session = hs_find(&srv->store, conn->subs[i].session_id);
    if (session != NULL) replay_into(srv, conn, session, sub, from);
    return;
  }
}

static void handle_frame(wd_server *srv, wd_conn *conn, const wd_frame *frame) {
  cbor_item_t *root = wd_decode(frame->body, frame->len);
  if (root == NULL) {
    conn_send_error(conn, 0, WD_ERR_INVALID_FRAME, "body is not cbor");
    conn->closing = true;
    conn_arm(srv, conn);
    return;
  }
  if (!conn->hello) {
    unsigned version = 0;
    char *client = NULL;
    if (frame->type != WD_FRAME_HELLO ||
        !wd_read_hello(root, &version, &client)) {
      free(client);
      wd_free(root);
      conn_send_error(conn, 0, WD_ERR_INVALID_FRAME, "hello expected first");
      conn->closing = true;
      conn_arm(srv, conn);
      return;
    }
    free(client);
    if (version != WD_PROTOCOL_VERSION) {
      wd_free(root);
      conn_send_error(conn, 0, WD_ERR_VERSION, "only protocol version 1");
      conn->closing = true;
      conn_arm(srv, conn);
      return;
    }
    conn->hello = true;
    conn->deadline_ms = 0;
    conn_send_item(conn, WD_FRAME_WELCOME, 0, 0,
                   wd_welcome(geteuid(), WD_MAX_BODY), true);
    conn_arm(srv, conn);
    wd_free(root);
    return;
  }
  if (frame->type == WD_FRAME_REQUEST) {
    uint64_t id = 0;
    char method[WD_MAX_METHOD];
    cbor_item_t *params = NULL;
    if (!wd_read_request(root, &id, method, &params)) {
      conn_send_error(conn, 0, WD_ERR_INVALID_PARAMS, "malformed request");
      wd_free(root);
      conn_arm(srv, conn);
      return;
    }
    wd_cache *hit = cache_find(conn, id);
    if (hit != NULL) {
      conn_queue(conn, WD_FRAME_RESPONSE, 0, 0, hit->body, hit->len);
      conn_arm(srv, conn);
      wd_free(root);
      return;
    }
    handle_request(srv, conn, id, method, params);
  } else if (frame->type == WD_FRAME_CONTROL) {
    handle_control(srv, conn, root);
    conn_arm(srv, conn);
  } else {
    conn_send_error(conn, 0, WD_ERR_INVALID_FRAME, "unexpected frame type");
    conn->closing = true;
    conn_arm(srv, conn);
  }
  wd_free(root);
}

static void conn_close(wd_server *srv, wd_conn *conn) {
  epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
  close(conn->fd);
  for (size_t i = 0; i < WD_CACHE; i++) free(conn->cache[i].body);
  wd_out *out = conn->out_head;
  while (out != NULL) {
    wd_out *next = out->next;
    free(out->data);
    free(out);
    out = next;
  }
  free(conn->in);
  for (size_t i = 0; i < WD_MAX_CONNS; i++)
    if (srv->conns[i] == conn) srv->conns[i] = NULL;
  free(conn);
}

static int conn_flush(wd_conn *conn) {
  while (conn->out_head != NULL) {
    wd_out *out = conn->out_head;
    ssize_t n = write(conn->fd, out->data + out->off, out->len - out->off);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return -1;
    }
    out->off += (size_t)n;
    conn->out_bytes -= (size_t)n;
    if (out->off < out->len) return 0;
    conn->out_head = out->next;
    if (conn->out_head == NULL) conn->out_tail = NULL;
    free(out->data);
    free(out);
  }
  return 0;
}

static int conn_arm(wd_server *srv, wd_conn *conn) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN | (conn->out_head != NULL ? EPOLLOUT : 0);
  ev.data.ptr = conn;
  return epoll_ctl(srv->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

static int conn_read(wd_server *srv, wd_conn *conn) {
  for (;;) {
    if (conn->in_len == WD_FRAME_HEADER + WD_MAX_BODY) return -1;
    ssize_t n = read(conn->fd, conn->in + conn->in_len,
                     WD_FRAME_HEADER + WD_MAX_BODY - conn->in_len);
    if (n == 0) return -1;
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return -1;
    }
    conn->in_len += (size_t)n;
    size_t off = 0;
    while (conn->in_len - off >= WD_FRAME_HEADER) {
      wd_frame frame;
      size_t used = 0;
      enum wd_frame_status status =
          wd_frame_parse(conn->in + off, conn->in_len - off, &frame, &used);
      if (status == WD_FRAME_PARTIAL) break;
      if (status == WD_FRAME_INVALID) {
        conn_send_error(conn, 0, WD_ERR_INVALID_FRAME, "frame rejected");
        conn->closing = true;
        conn_arm(srv, conn);
        return 0;
      }
      handle_frame(srv, conn, &frame);
      off += used;
      if (conn->closing) break;
    }
    if (off > 0) {
      memmove(conn->in, conn->in + off, conn->in_len - off);
      conn->in_len -= off;
    }
    if (conn->closing) return 0;
  }
  return 0;
}

static void sweep(wd_server *srv, uint64_t now) {
  for (size_t i = 0; i < WD_MAX_CONNS; i++) {
    wd_conn *conn = srv->conns[i];
    if (conn == NULL) continue;
    if (!conn->hello && conn->deadline_ms != 0 && now >= conn->deadline_ms)
      conn_close(srv, conn);
    else if (conn->closing && conn->out_head == NULL)
      conn_close(srv, conn);
  }
}

static int next_timeout(wd_server *srv, uint64_t now) {
  uint64_t best = 0;
  for (size_t i = 0; i < WD_MAX_CONNS; i++) {
    wd_conn *conn = srv->conns[i];
    if (conn == NULL || conn->hello || conn->deadline_ms == 0) continue;
    if (best == 0 || conn->deadline_ms < best) best = conn->deadline_ms;
  }
  if (best == 0) return -1;
  if (best <= now) return 0;
  uint64_t delta = best - now;
  return delta > 60000 ? 60000 : (int)delta;
}

static void server_accept(wd_server *srv) {
  for (;;) {
    int fd = accept4(srv->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;
    }
    struct ucred cred;
    socklen_t len = sizeof(cred);
    bool ok = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
              cred.uid == geteuid();
    if (!ok) {
      wd_conn temp;
      memset(&temp, 0, sizeof(temp));
      temp.fd = fd;
      conn_send_error(&temp, 0, WD_ERR_UNAUTHENTICATED, "peer uid mismatch");
      conn_flush(&temp);
      wd_out *out = temp.out_head;
      while (out != NULL) {
        wd_out *next = out->next;
        free(out->data);
        free(out);
        out = next;
      }
      close(fd);
      continue;
    }
    size_t slot = WD_MAX_CONNS;
    for (size_t i = 0; i < WD_MAX_CONNS; i++)
      if (srv->conns[i] == NULL) {
        slot = i;
        break;
      }
    if (slot == WD_MAX_CONNS) {
      close(fd);
      continue;
    }
    wd_conn *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
      close(fd);
      continue;
    }
    conn->fd = fd;
    conn->in = malloc(WD_FRAME_HEADER + WD_MAX_BODY);
    if (conn->in == NULL) {
      free(conn);
      close(fd);
      continue;
    }
    conn->deadline_ms = wu_now_ms() + WD_HANDSHAKE_MS;
    conn->next_sub = 1;
    srv->conns[slot] = conn;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
      conn_close(srv, conn);
      continue;
    }
  }
}

static int acquire_lock(const char *path) {
  char *boot = wu_boot_id();
  if (boot == NULL) return -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) {
      char body[256];
      int n = snprintf(body, sizeof(body),
                       "{\"pid\":%ld,\"boot_id\":\"%s\",\"started_at\":%llu}\n",
                       (long)getpid(), boot, (unsigned long long)wu_now_ms());
      ssize_t written = write(fd, body, (size_t)n);
      close(fd);
      free(boot);
      return written == n ? 0 : -1;
    }
    if (errno != EEXIST) {
      free(boot);
      return -1;
    }
    int rfd = open(path, O_RDONLY | O_CLOEXEC);
    if (rfd < 0) {
      free(boot);
      return -1;
    }
    char buf[256];
    ssize_t got = read(rfd, buf, sizeof(buf) - 1);
    close(rfd);
    if (got <= 0) {
      unlink(path);
      continue;
    }
    buf[got] = '\0';
    char *pid_text = strstr(buf, "\"pid\":");
    char *boot_text = strstr(buf, "\"boot_id\":\"");
    long pid = pid_text != NULL ? strtol(pid_text + 6, NULL, 10) : 0;
    bool same_boot = false;
    if (boot_text != NULL) {
      char *start = boot_text + 11;
      char *end = strchr(start, '"');
      same_boot = end != NULL && (size_t)(end - start) == strlen(boot) &&
                  memcmp(start, boot, strlen(boot)) == 0;
    }
    if (same_boot && wu_pid_alive(pid)) {
      free(boot);
      errno = EEXIST;
      return -1;
    }
    unlink(path);
  }
  free(boot);
  return -1;
}

static int setup_listener(const char *path) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (chmod(path, 0600) != 0) {
    close(fd);
    unlink(path);
    return -1;
  }
  if (listen(fd, 64) != 0) {
    close(fd);
    unlink(path);
    return -1;
  }
  return fd;
}

static int setup_signals(void) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) return -1;
  return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

int wd_up_run(const wd_up_config *cfg) {
  wd_server srv;
  memset(&srv, 0, sizeof(srv));
  srv.listen_fd = -1;
  srv.epoll_fd = -1;
  srv.signal_fd = -1;
  srv.socket_path = strdup(cfg->socket_path);
  srv.state_dir = strdup(cfg->state_dir);
  if (srv.socket_path == NULL || srv.state_dir == NULL) goto cleanup;
  if (wu_mkdir_p(srv.state_dir, 0700) != 0) {
    fprintf(stderr, "ward: cannot create %s: %s\n", srv.state_dir,
            strerror(errno));
    goto cleanup;
  }
  srv.lock_path = wu_path_join(srv.state_dir, "ward.lock");
  if (srv.lock_path == NULL) goto cleanup;
  if (acquire_lock(srv.lock_path) != 0) {
    fprintf(stderr, "ward: %s is already served by another daemon\n",
            srv.state_dir);
    goto cleanup;
  }
  if (hs_open(&srv.store, srv.state_dir) != 0) {
    fprintf(stderr, "ward: cannot open state directory\n");
    goto cleanup;
  }
  srv.listen_fd = setup_listener(srv.socket_path);
  if (srv.listen_fd < 0) {
    fprintf(stderr, "ward: cannot listen on %s: %s\n", srv.socket_path,
            strerror(errno));
    goto cleanup;
  }
  srv.signal_fd = setup_signals();
  if (srv.signal_fd < 0) goto cleanup;
  srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (srv.epoll_fd < 0) goto cleanup;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = &token_listen;
  if (epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, srv.listen_fd, &ev) != 0)
    goto cleanup;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = &token_signal;
  if (epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, srv.signal_fd, &ev) != 0)
    goto cleanup;

  printf("ready %s\n", srv.socket_path);
  fflush(stdout);

  struct epoll_event events[64];
  while (!srv.stop) {
    uint64_t now = wu_now_ms();
    int timeout = next_timeout(&srv, now);
    int n = epoll_wait(srv.epoll_fd, events, 64, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < n; i++) {
      void *ptr = events[i].data.ptr;
      if (ptr == &token_listen) {
        server_accept(&srv);
        continue;
      }
      if (ptr == &token_signal) {
        struct signalfd_siginfo info;
        while (read(srv.signal_fd, &info, sizeof(info)) == sizeof(info))
          srv.stop = true;
        continue;
      }
      wd_conn *conn = ptr;
      uint32_t mask = events[i].events;
      if (mask & (EPOLLERR | EPOLLHUP)) {
        conn_close(&srv, conn);
        continue;
      }
      if (mask & EPOLLOUT) {
        if (conn_flush(conn) != 0) {
          conn_close(&srv, conn);
          continue;
        }
        if (conn->closing && conn->out_head == NULL) {
          conn_close(&srv, conn);
          continue;
        }
        conn_arm(&srv, conn);
      }
      if (mask & EPOLLIN) {
        if (conn_read(&srv, conn) != 0) {
          conn_close(&srv, conn);
          continue;
        }
        if (conn->closing && conn->out_head == NULL) {
          conn_close(&srv, conn);
          continue;
        }
      }
    }
    sweep(&srv, wu_now_ms());
  }

cleanup:
  for (size_t i = 0; i < WD_MAX_CONNS; i++)
    if (srv.conns[i] != NULL && srv.epoll_fd >= 0) conn_close(&srv, srv.conns[i]);
  if (srv.socket_path != NULL) unlink(srv.socket_path);
  if (srv.lock_path != NULL) unlink(srv.lock_path);
  if (srv.listen_fd >= 0) close(srv.listen_fd);
  if (srv.epoll_fd >= 0) close(srv.epoll_fd);
  if (srv.signal_fd >= 0) close(srv.signal_fd);
  wd_turn_free(srv.turn);
  wd_engine_free(srv.engine);
  hs_close(&srv.store);
  free(srv.socket_path);
  free(srv.state_dir);
  free(srv.lock_path);
  return srv.stop ? 0 : 1;
}
