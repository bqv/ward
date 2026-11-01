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

#include "core/approval.h"
#include "core/labels.h"
#include "core/models.h"
#include "core/session.h"
#include "core/util.h"
#include "net/pki.h"
#include "net/quic.h"
#include "proto/codec.h"
#include "proto/frame.h"

#define WD_MAX_CONNS 256
#define WD_MAX_SUBS 16
#define WD_CACHE 16
#define WD_MAX_OUT (8u << 20)
#define WD_HANDSHAKE_MS 5000u
#define WD_QUIC_DEFAULT_BIND "127.0.0.1:0"
#define WD_PAIR_TTL_MS 300000u
#define WD_APPROVAL_TTL_MS 300000u

typedef struct wd_out {
  struct wd_out *next;
  size_t len;
  size_t off;
  uint8_t *data;
} wd_out;

typedef struct wd_run wd_run;

struct wd_run {
  struct wd_run *next;
  char session_id[WD_ID_LEN];
  wd_turn *turn;
  int worker_fd;
  int http_fd;
};


typedef struct {
  wd_run *run;
  int kind;
} wd_run_token;

#define WD_MAX_RUNS 16

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
  wd_quic_stream *stream;
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
  wd_store store;
  wd_engine *engine;
  wd_run *runs;
  wd_pki *pki;
  wd_quic *quic;
  wd_approvals *approvals;
  wd_models *models;
  wd_labels *labels;
  char *default_model;
  char *quic_endpoint;
  int quic_port;
  wd_conn *conns[WD_MAX_CONNS];
  bool stop;
} wd_server;

static int token_listen;
static int token_signal;
static int token_quic;
static wd_run_token run_tokens[WD_MAX_RUNS];

static void conn_close(wd_server *srv, wd_conn *conn);
static void conn_release(wd_server *srv, wd_conn *conn);
static cbor_item_t *session_cbor(wd_server *srv, wd_session *session);
static wd_run *run_new(wd_server *srv, wd_session *session, char **err);
static wd_run *run_find(wd_server *srv, const char *session_id);
static void run_free(wd_server *srv, wd_run *run);
static void run_park(wd_server *srv, wd_run *run, int kind);
static void run_after(wd_server *srv, wd_run *run, int rc, char *err);
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

static void conn_broadcast(wd_server *srv, wd_session *session,
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

static cbor_item_t *model_to_cbor(const wd_model *model) {
  cbor_item_t *item = wd_map(5);
  wd_put(item, "id", wd_text(model->id));
  wd_put(item, "provider",
         wd_text(model->provider != NULL ? model->provider : ""));
  wd_put(item, "model", wd_text(model->model != NULL ? model->model : ""));
  wd_put(item, "context",
         wd_text(model->context != NULL ? model->context : ""));
  wd_put(item, "tags", wd_text(model->tags != NULL ? model->tags : ""));
  return item;
}

static void handle_model_list(wd_server *srv, wd_conn *conn, uint64_t id) {
  size_t count = wd_models_count(srv->models);
  cbor_item_t *list = wd_array(count);
  for (size_t i = 0; i < count; i++) {
    cbor_item_t *item = model_to_cbor(wd_models_at(srv->models, i));
    bool pushed = cbor_array_push(list, item);
    cbor_decref(&item);
    if (!pushed) {
      conn->closing = true;
      break;
    }
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "models", list);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_model_select(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  char *name = wd_text_copy(wd_get(params, "id"));
  if (name == NULL) name = wd_text_copy(wd_get(params, "model"));
  if (name == NULL || wd_models_find(srv->models, name) == NULL) {
    free(name);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such model");
    conn_arm(srv, conn);
    return;
  }
  free(srv->default_model);
  srv->default_model = name;
  cbor_item_t *result = wd_map(1);
  wd_put(result, "model", wd_text(name));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_cancel(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  bool cancelled = false;
  if (session_id != NULL) {
    wd_run *run = run_find(srv, session_id);
    if (run != NULL && wd_turn_pending_kind(run->turn) == 2)
      cancelled = wd_turn_cancel_tool(run->turn) != 0;
  }
  free(session_id);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "cancelled", cbor_build_bool(cancelled));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_fork(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  uint64_t from = 0;
  wd_uint_of(wd_get(params, "from"), &from);
  wd_session *parent =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (parent == NULL) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  wd_session *child = NULL;
  if (wd_session_fork(&srv->store, parent, from, &child) != 0) {
    free(session_id);
    respond_internal(srv, conn, id, "cannot fork the session");
    return;
  }
  free(session_id);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, child));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_set_model(wd_server *srv, wd_conn *conn, uint64_t id,
                                     cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *name = wd_text_copy(wd_get(params, "model"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL) {
    free(session_id);
    free(name);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  if (name == NULL || wd_models_find(srv->models, name) == NULL) {
    free(session_id);
    free(name);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such model");
    conn_arm(srv, conn);
    return;
  }
  uint64_t seq = 0;
  if (wd_session_set_model(session, name, &seq) != 0) {
    free(session_id);
    free(name);
    respond_internal(srv, conn, id, "cannot record the model change");
    return;
  }
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "model", wd_text(name));
  cbor_item_t *event = wd_map(1);
  wd_put(event, "model", wd_text(name));
  conn_broadcast(srv, session, "session.model", seq, event);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, session));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
  free(session_id);
  free(name);
}

static cbor_item_t *session_cbor(wd_server *srv, wd_session *session) {
  wd_label label;
  bool tagged = wd_labels_get(srv->labels, session->id, &label) == 0;
  cbor_item_t *item = wd_map(tagged ? 9 : 5);
  wd_put(item, "id", wd_text(session->id));
  wd_put(item, "cwd", wd_text(session->cwd));
  wd_put(item, "model", wd_text(session->model));
  wd_put(item, "created", wd_uint((uint64_t)session->created_ms));
  wd_put(item, "last_seq", wd_uint(session->last_seq));
  if (tagged) {
    wd_put(item, "name", wd_text(label.name));
    wd_put(item, "archived", cbor_build_bool(label.archived));
    wd_put(item, "pinned", cbor_build_bool(label.pinned));
    wd_put(item, "updated_ms", wd_uint(label.updated_ms));
  }
  return item;
}

static void handle_stats_session(wd_server *srv, wd_conn *conn, uint64_t id,
                                 cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  wd_session_stats stats;
  if (wd_session_measure(session, &stats) != 0) {
    respond_internal(srv, conn, id, "cannot read the session log");
    return;
  }
  const wd_model *model = wd_models_find(srv->models, session->model);
  double in_price = 0;
  double out_price = 0;
  bool priced = model != NULL && model->price != NULL &&
                sscanf(model->price, "%lf,%lf", &in_price, &out_price) == 2;
  cbor_item_t *result = wd_map(priced ? 10 : 7);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "model", wd_text(session->model));
  wd_put(result, "events", wd_uint(stats.events));
  wd_put(result, "turns", wd_uint(stats.turns));
  wd_put(result, "text_bytes", wd_uint(stats.text_bytes));
  wd_put(result, "prompt_bytes", wd_uint(stats.prompt_bytes));
  wd_put(result, "tokens_estimated", cbor_build_bool(true));
  if (priced) {
    double prompt_tokens = (double)stats.prompt_bytes / 4.0;
    double text_tokens = (double)stats.text_bytes / 4.0;
    double in_cost = prompt_tokens * in_price / 1e6;
    double out_cost = text_tokens * out_price / 1e6;
    wd_put(result, "cost_in", cbor_build_float8(in_cost));
    wd_put(result, "cost_out", cbor_build_float8(out_cost));
    wd_put(result, "cost_total", cbor_build_float8(in_cost + out_cost));
  }
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_stats_global(wd_server *srv, wd_conn *conn, uint64_t id) {
  wd_session_stats total;
  memset(&total, 0, sizeof(total));
  for (size_t i = 0; i < srv->store.len; i++) {
    wd_session_stats one;
    if (wd_session_measure(srv->store.items[i], &one) == 0) {
      total.events += one.events;
      total.turns += one.turns;
      total.text_bytes += one.text_bytes;
      total.prompt_bytes += one.prompt_bytes;
    }
  }
  cbor_item_t *result = wd_map(5);
  wd_put(result, "sessions", wd_uint(srv->store.len));
  wd_put(result, "events", wd_uint(total.events));
  wd_put(result, "turns", wd_uint(total.turns));
  wd_put(result, "text_bytes", wd_uint(total.text_bytes));
  wd_put(result, "prompt_bytes", wd_uint(total.prompt_bytes));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_label(wd_server *srv, wd_conn *conn, uint64_t id,
                                 cbor_item_t *params, int kind) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  int rc = 0;
  if (kind == 0) {
    char *name = wd_text_copy(wd_get(params, "name"));
    if (name == NULL || strlen(name) >= WD_LABEL_NAME) {
      free(name);
      free(session_id);
      conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                      "name must be 1..127 bytes");
      conn_arm(srv, conn);
      return;
    }
    rc = wd_labels_set_name(srv->labels, session_id, name);
    free(name);
  } else {
    char *key = kind == 1 ? "archived" : "pinned";
    bool value = false;
    if (!wd_bool_of(wd_get(params, key), &value)) {
      free(session_id);
      conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "a boolean is required");
      conn_arm(srv, conn);
      return;
    }
    rc = kind == 1 ? wd_labels_set_archived(srv->labels, session_id, value)
                   : wd_labels_set_pinned(srv->labels, session_id, value);
  }
  if (rc != 0) {
    free(session_id);
    respond_internal(srv, conn, id, "cannot write the session label");
    return;
  }
  cbor_item_t *logged = wd_map(1);
  wd_put(logged, "session", session_cbor(srv, session));
  uint64_t seq = 0;
  if (wd_session_emit(session, "session.updated", logged, &seq) == 0) {
    cbor_item_t *event = wd_map(1);
    wd_put(event, "session", session_cbor(srv, session));
    conn_broadcast(srv, session, "session.updated", seq, event);
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, session));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
  free(session_id);
}

static void handle_session_compact(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *history = NULL;
  if (wd_session_history(session, 0, &history) != 0 || cbor_array_size(history) == 0) {
    if (history != NULL) wd_free(history);
    conn_send_error(conn, id, WD_ERR_CONFLICT, "nothing to compact");
    conn_arm(srv, conn);
    return;
  }
  if (run_find(srv, session->id) != NULL) {
    wd_free(history);
    conn_send_error(conn, id, WD_ERR_CONFLICT, "the session is busy");
    conn_arm(srv, conn);
    return;
  }
  char *err = NULL;
  wd_run *run = run_new(srv, session, &err);
  if (run == NULL) {
    wd_free(history);
    respond_internal(srv, conn, id, err != NULL ? err : "no modules");
    free(err);
    return;
  }
  char *summary = NULL;
  int rc = wd_turn_compact(run->turn, history, &summary, &err);
  run_free(srv, run);
  wd_free(history);
  if (rc != 0) {
    respond_internal(srv, conn, id,
                     err != NULL ? err : "cannot summarise the session");
    free(err);
    conn_arm(srv, conn);
    return;
  }
  free(err);
  cbor_item_t *record = wd_map(2);
  wd_put(record, "through", wd_uint(session->last_seq));
  wd_put(record, "summary", wd_text(summary));
  uint64_t seq = 0;
  if (wd_session_emit(session, "session.compacted", record, &seq) != 0) {
    free(summary);
    respond_internal(srv, conn, id, "cannot record the summary");
    return;
  }
  cbor_item_t *event = wd_map(2);
  wd_put(event, "through", wd_uint(seq - 1));
  wd_put(event, "summary", wd_text(summary));
  conn_broadcast(srv, session, "session.compacted", seq, event);
  cbor_item_t *result = wd_map(3);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "through", wd_uint(seq - 1));
  wd_put(result, "summary", wd_text(summary));
  free(summary);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_checkpoint(wd_server *srv, wd_conn *conn,
                                      uint64_t id, cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *label = wd_text_copy(wd_get(params, "label"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    free(label);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  uint64_t seq = 0;
  int rc = wd_session_checkpoint(session, label, &seq);
  free(label);
  if (rc != 0) {
    respond_internal(srv, conn, id, "cannot record the checkpoint");
    return;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "seq", wd_uint(seq));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_undo(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  uint64_t to = 0;
  bool have_to = wd_uint_of(wd_get(params, "to"), &to);
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  if (!have_to && wd_session_last_checkpoint(session, &to) != 0) {
    conn_send_error(conn, id, WD_ERR_CONFLICT,
                    "the session has no checkpoint to undo to");
    conn_arm(srv, conn);
    return;
  }
  if (to == 0 || to > session->last_seq) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "the checkpoint is not in this session");
    conn_arm(srv, conn);
    return;
  }
  wd_session *branch = NULL;
  if (wd_session_fork(&srv->store, session, to, &branch) != 0) {
    respond_internal(srv, conn, id, "cannot branch from the checkpoint");
    return;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, branch));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_search(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *query = wd_text_copy(wd_get(params, "query"));
  uint64_t limit = 50;
  wd_uint_of(wd_get(params, "limit"), &limit);
  wd_label *rows = NULL;
  size_t count = 0;
  int rc = wd_labels_search(srv->labels, query, (size_t)limit, &rows, &count);
  free(query);
  if (rc != 0) {
    respond_internal(srv, conn, id, "cannot search the labels");
    return;
  }
  cbor_item_t *list = wd_array(count);
  for (size_t i = 0; i < count; i++) {
    wd_session *session = wd_session_find(&srv->store, rows[i].id);
    if (session == NULL) continue;
    cbor_item_t *entry = session_cbor(srv, session);
    bool pushed = cbor_array_push(list, entry);
    cbor_decref(&entry);
    if (!pushed) break;
  }
  free(rows);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "sessions", list);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
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
  if (model == NULL && srv->default_model != NULL)
    model = strdup(srv->default_model);
  if (model != NULL && wd_models_count(srv->models) > 0 &&
      wd_models_find(srv->models, model) == NULL) {
    free(cwd);
    free(model);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such model");
    conn_arm(srv, conn);
    return;
  }
  wd_session *session = NULL;
  if (wd_session_create(&srv->store, cwd, model, &session) != 0) {
    free(cwd);
    free(model);
    respond_internal(srv, conn, id, "cannot create session");
    return;
  }
  free(cwd);
  free(model);

  wd_labels_touch(srv->labels, session->id);
  cbor_item_t *logged = wd_map(1);
  wd_put(logged, "session", session_cbor(srv, session));
  uint64_t seq = 0;
  if (wd_session_emit(session, "session.updated", logged, &seq) != 0) {
    respond_internal(srv, conn, id, "cannot append session event");
    return;
  }
  cbor_item_t *event = wd_map(1);
  wd_put(event, "session", session_cbor(srv, session));
  conn_broadcast(srv, session, "session.updated", seq, event);

  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, session));
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
  bool include_archived = false;
  wd_bool_of(wd_get(params, "include_archived"), &include_archived);
  size_t show = 0;
  for (size_t i = 0; i < srv->store.len && show < limit; i++) {
    const wd_session *session = srv->store.items[i];
    wd_label label;
    if (wd_labels_get(srv->labels, session->id, &label) == 0 &&
        label.archived && !include_archived)
      continue;
    show++;
  }
  cbor_item_t *list = wd_array(show);
  size_t pushed_count = 0;
  for (size_t i = 0; i < srv->store.len && pushed_count < show; i++) {
    wd_session *session = srv->store.items[i];
    wd_label label;
    if (wd_labels_get(srv->labels, session->id, &label) == 0 &&
        label.archived && !include_archived)
      continue;
    cbor_item_t *entry = session_cbor(srv, session);
    bool pushed = cbor_array_push(list, entry);
    cbor_decref(&entry);
    if (!pushed) break;
    pushed_count++;
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
  wd_session *session = wd_session_find(&srv->store, want);
  free(want);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "session", session_cbor(srv, session));
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

static void replay_into(wd_server *srv, wd_conn *conn, wd_session *session,
                        uint64_t sub, uint64_t from) {
  uint64_t to = session->last_seq;
  replay_ctx rc = {.srv = srv, .conn = conn, .sub = sub};
  wd_session_replay(session, from, replay_event, &rc);
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
  wd_session *session = wd_session_find(&srv->store, want);
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
  wd_put(result, "session", session_cbor(srv, session));
  wd_put(result, "sub", wd_uint(sub));
  wd_put(result, "last_seq", wd_uint(session->last_seq));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
  if (have_from) replay_into(srv, conn, session, sub, from);
}

typedef struct {
  wd_server *srv;
  wd_session *session;
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
  if (logged == NULL || wd_session_emit(tc->session, event, logged, &seq) != 0) {
    *err = strdup("cannot append the turn event");
    return -1;
  }
  cbor_item_t *broadcast = payload != NULL ? dup_item(payload) : wd_map(0);
  if (broadcast != NULL)
    conn_broadcast(tc->srv, tc->session, event, seq, broadcast);
  return 0;
}

static wd_run *run_new(wd_server *srv, wd_session *session, char **err) {
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
  wd_run *run = calloc(1, sizeof(*run));
  if (run == NULL) {
    *err = strdup("out of memory");
    return NULL;
  }
  run->worker_fd = -1;
  run->http_fd = -1;
  snprintf(run->session_id, sizeof(run->session_id), "%s", session->id);
  run->turn = wd_turn_new(srv->engine, dir, session->cwd, err);
  if (run->turn == NULL) {
    free(run);
    return NULL;
  }
  wd_turn_set_root(run->turn, session->cwd);
  run->next = srv->runs;
  srv->runs = run;
  return run;
}

static wd_run *run_find(wd_server *srv, const char *session_id) {
  for (wd_run *run = srv->runs; run != NULL; run = run->next) {
    if (strcmp(run->session_id, session_id) == 0) return run;
  }
  return NULL;
}

static void run_unwatch(wd_server *srv, wd_run *run, int kind) {
  int *fd = kind == 2 ? &run->worker_fd : &run->http_fd;
  if (*fd < 0) return;
  (void)epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, *fd, NULL);
  for (int i = 0; i < WD_MAX_RUNS; i++) {
    if (run_tokens[i].run == run && run_tokens[i].kind == kind) {
      run_tokens[i].run = NULL;
      run_tokens[i].kind = 0;
    }
  }
  *fd = -1;
}

static void run_watch(wd_server *srv, wd_run *run, int kind) {
  int fd = kind == 2 ? wd_turn_worker_fd(run->turn) : wd_turn_http_fd(run->turn);
  int *slot = kind == 2 ? &run->worker_fd : &run->http_fd;
  if (fd < 0) {
    run_unwatch(srv, run, kind);
    return;
  }
  if (*slot == fd) return;
  run_unwatch(srv, run, kind);
  for (int i = 0; i < WD_MAX_RUNS; i++) {
    if (run_tokens[i].run != NULL) continue;
    run_tokens[i].run = run;
    run_tokens[i].kind = kind;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &run_tokens[i];
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
      run_tokens[i].run = NULL;
      run_tokens[i].kind = 0;
      return;
    }
    *slot = fd;
    return;
  }
}

static void run_free(wd_server *srv, wd_run *run) {
  run_unwatch(srv, run, 2);
  run_unwatch(srv, run, 3);
  for (wd_run **at = &srv->runs; *at != NULL; at = &(*at)->next) {
    if (*at == run) {
      *at = run->next;
      break;
    }
  }
  wd_turn_free(run->turn);
  free(run);
}

static void run_park(wd_server *srv, wd_run *run, int kind) {
  if (kind == 3) {
    run_unwatch(srv, run, 2);
    run_watch(srv, run, 3);
  } else if (kind == 2) {
    run_unwatch(srv, run, 3);
    run_watch(srv, run, 2);
  } else {
    run_unwatch(srv, run, 2);
    run_unwatch(srv, run, 3);
  }
}

static void run_after(wd_server *srv, wd_run *run, int rc, char *err) {
  if (rc == WD_TURN_PENDING) {
    int kind = wd_turn_pending_kind(run->turn);
    if (kind == 1) {
      wd_session *session = wd_session_find(&srv->store, run->session_id);
      uint64_t call = 0;
      uint64_t epoch = 0;
      char *reason = NULL;
      if (session != NULL &&
          wd_turn_pending(run->turn, &call, &epoch, &reason) == 0) {
        wd_approval_request(srv->approvals, session->id, call, epoch,
                            wd_now_ms(), WD_APPROVAL_TTL_MS);
        free(reason);
      }
    }
    run_park(srv, run, kind);
    return;
  }
  if (rc != 0)
    fprintf(stderr, "ward: the turn failed: %s\n",
            err != NULL ? err : "unknown");
  free(err);
  run_free(srv, run);
}

static void handle_session_prompt(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *text = wd_text_copy(wd_get(params, "text"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
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
  if (run_find(srv, session->id) != NULL) {
    free(session_id);
    free(text);
    conn_send_error(conn, id, WD_ERR_CONFLICT, "the session is already busy");
    conn_arm(srv, conn);
    return;
  }
  char *err = NULL;
  wd_run *run = run_new(srv, session, &err);
  if (run == NULL) {
    free(session_id);
    free(text);
    respond_internal(srv, conn, id, err != NULL ? err : "no modules");
    free(err);
    return;
  }
  cbor_item_t *history = NULL;
  if (wd_session_history(session, 8192, &history) == 0)
    wd_turn_set_history(run->turn, history);

  cbor_item_t *asked = wd_map(1);
  wd_put(asked, "text", wd_text(text));
  uint64_t asked_seq = 0;
  if (wd_session_emit(session, "session.prompted", asked, &asked_seq) == 0) {
    cbor_item_t *event = wd_map(1);
    wd_put(event, "text", wd_text(text));
    conn_broadcast(srv, session, "session.prompted", asked_seq, event);
  }
  wd_turn_ctx tc = {srv, session};
  int rc = wd_turn_prompt(run->turn, text, turn_emit, &tc, &err);
  free(session_id);
  free(text);
  if (rc != 0 && rc != WD_TURN_PENDING) {
    run_after(srv, run, rc, err);
    respond_internal(srv, conn, id, "turn failed");
    return;
  }
  int kind = rc == WD_TURN_PENDING ? wd_turn_pending_kind(run->turn) : 0;
  cbor_item_t *result = wd_map(kind == 1 ? 3 : 2);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "turn", wd_uint(wd_turn_id(run->turn)));
  if (kind == 1) {
    uint64_t call = 0;
    uint64_t epoch = 0;
    char *reason = NULL;
    if (wd_turn_pending(run->turn, &call, &epoch, &reason) != 0) {
      wd_free(result);
      respond_internal(srv, conn, id, "the turn asked for an unnamed approval");
      return;
    }
    wd_approval_request(srv->approvals, session->id, call, epoch, wd_now_ms(),
                        WD_APPROVAL_TTL_MS);
    cbor_item_t *notice = wd_map(3);
    wd_put(notice, "call", wd_uint(call));
    wd_put(notice, "epoch", wd_uint(epoch));
    wd_put(notice, "reason", wd_text(reason != NULL ? reason : ""));
    wd_put(result, "approval", notice);
    free(reason);
  }
  if (kind == 0)
    run_free(srv, run);
  else
    run_park(srv, run, kind);
  conn_send_cached(conn, id, wd_response_ok(id, result));
}

static void tool_done(wd_server *srv, wd_run *run) {
  wd_session *session = wd_session_find(&srv->store, run->session_id);
  if (session == NULL) {
    run_free(srv, run);
    return;
  }
  wd_turn_ctx tc = {srv, session};
  char *err = NULL;
  int rc = wd_turn_resume_tool(run->turn, turn_emit, &tc, &err);
  run_after(srv, run, rc, err);
}

static void http_done(wd_server *srv, wd_run *run) {
  wd_session *session = wd_session_find(&srv->store, run->session_id);
  if (session == NULL) {
    run_free(srv, run);
    return;
  }
  wd_turn_ctx tc = {srv, session};
  char *err = NULL;
  int rc = wd_turn_resume_http(run->turn, turn_emit, &tc, &err);
  run_after(srv, run, rc, err);
}

static bool resume_turn(wd_server *srv, const char *session_id, bool allow,
                        char **err) {
  wd_run *run = run_find(srv, session_id);
  if (run == NULL) return false;
  wd_session *session = wd_session_find(&srv->store, session_id);
  if (session == NULL) return false;
  wd_turn_ctx tc = {srv, session};
  int rc = wd_turn_resume(run->turn, allow, turn_emit, &tc, err);
  run_after(srv, run, rc, *err != NULL ? strdup(*err) : NULL);
  if (*err != NULL) {
    free(*err);
    *err = NULL;
  }
  return true;
}

static void handle_approval_respond(wd_server *srv, wd_conn *conn, uint64_t id,
                                    cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "session"));
  uint64_t call = 0;
  uint64_t epoch = 0;
  bool allow = false;
  if (session_id == NULL || !wd_uint_of(wd_get(params, "call"), &call) ||
      !wd_uint_of(wd_get(params, "epoch"), &epoch) ||
      !wd_bool_of(wd_get(params, "allow"), &allow)) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "session, call, epoch and allow are required");
    conn_arm(srv, conn);
    return;
  }
  wd_approval_state state =
      wd_approval_answer(srv->approvals, session_id, call, epoch, allow,
                         wd_now_ms());
  if (state == WD_APPROVAL_ALREADY_RESOLVED) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_ALREADY_RESOLVED,
                    "this approval was already answered");
    conn_arm(srv, conn);
    return;
  }
  if (state != WD_APPROVAL_GRANTED && state != WD_APPROVAL_DENIED) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such approval");
    conn_arm(srv, conn);
    return;
  }
  wd_session *session = wd_session_find(&srv->store, session_id);
  if (session != NULL) {
    wd_turn_ctx tc = {srv, session};
    cbor_item_t *resolved = wd_map(3);
    wd_put(resolved, "call", wd_uint(call));
    wd_put(resolved, "epoch", wd_uint(epoch));
    wd_put(resolved, "allow", cbor_build_bool(allow));
    char *err = NULL;
    if (turn_emit(&tc, "approval.resolved", resolved, &err) != 0) {
      free(err);
      wd_free(resolved);
      free(session_id);
      respond_internal(srv, conn, id, "cannot record the resolution");
      return;
    }
    wd_free(resolved);
  }
  char *err = NULL;
  bool resumed =
      session != NULL && resume_turn(srv, session_id, allow, &err);
  if (!resumed && err != NULL) {
    respond_internal(srv, conn, id, err);
    free(err);
    free(session_id);
    return;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "resolved", cbor_build_bool(true));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  free(session_id);
}

static bool admin_method(const char *method) {
  return strcmp(method, "device.pair") == 0 ||
         strcmp(method, "device.list") == 0 ||
         strcmp(method, "device.revoke") == 0;
}

static char *read_text_file(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return NULL;
  size_t cap = 8192;
  size_t len = 0;
  char *buf = malloc(cap);
  if (buf == NULL) {
    close(fd);
    return NULL;
  }
  for (;;) {
    if (len + 1 == cap) {
      char *grown = realloc(buf, cap * 2);
      if (grown == NULL) {
        free(buf);
        close(fd);
        return NULL;
      }
      buf = grown;
      cap *= 2;
    }
    ssize_t n = read(fd, buf + len, cap - len - 1);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf);
      close(fd);
      return NULL;
    }
    if (n == 0) break;
    len += (size_t)n;
  }
  close(fd);
  buf[len] = '\0';
  return buf;
}

static void handle_device_pair(wd_server *srv, wd_conn *conn, uint64_t id,
                               cbor_item_t *params) {
  if (srv->quic == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_SUPPORTED, "quic is disabled");
    conn_arm(srv, conn);
    return;
  }
  char *name = wd_text_copy(wd_get(params, "scope"));
  int scope = name == NULL ? WD_SCOPE_DESKTOP : wd_scope_parse(name);
  free(name);
  if (scope == WD_SCOPE_NONE) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "scope is readonly, phone or desktop");
    conn_arm(srv, conn);
    return;
  }
  char *token = wd_pki_token_mint(srv->pki, wd_now_ms(), WD_PAIR_TTL_MS, scope);
  if (token == NULL) {
    conn_send_error(conn, id, WD_ERR_INTERNAL, "cannot mint a token");
    conn_arm(srv, conn);
    return;
  }
  char fingerprint[65];
  wd_pki_fingerprint(srv->pki, fingerprint);
  cbor_item_t *result = wd_map(5);
  wd_put(result, "token", wd_text(token));
  wd_put(result, "fingerprint", wd_text(fingerprint));
  wd_put(result, "scope", wd_text(wd_scope_name(scope)));
  wd_put(result, "endpoint",
         wd_text(srv->quic_endpoint != NULL ? srv->quic_endpoint : ""));
  wd_put(result, "expires_ms", wd_uint(wd_now_ms() + WD_PAIR_TTL_MS));
  free(token);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_device_enrol(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  char *token = wd_text_copy(wd_get(params, "token"));
  char *csr = wd_text_copy(wd_get(params, "csr"));
  if (token == NULL || csr == NULL) {
    free(token);
    free(csr);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "token and csr are required");
    conn_arm(srv, conn);
    return;
  }
  int scope = WD_SCOPE_NONE;
  if (wd_pki_token_redeem(srv->pki, token, wd_now_ms(), &scope) != 0) {
    free(token);
    free(csr);
    conn_send_error(conn, id, WD_ERR_PERMISSION_DENIED,
                    "the enrolment token is unknown, spent or expired");
    conn->closing = true;
    conn_arm(srv, conn);
    return;
  }
  free(token);
  char failure[256] = {0};
  char *cert = NULL;
  char serial[WD_PKI_SERIAL_MAX] = {0};
  int rc = wd_pki_issue(srv->pki, csr, scope, &cert, serial, failure,
                        sizeof(failure));
  free(csr);
  if (rc != 0) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, failure);
    conn_arm(srv, conn);
    return;
  }
  char *ca = read_text_file(wd_pki_ca_cert(srv->pki));
  cbor_item_t *result = wd_map(3);
  wd_put(result, "cert", wd_text(cert));
  wd_put(result, "ca", wd_text(ca != NULL ? ca : ""));
  wd_put(result, "serial", wd_text(serial));
  free(cert);
  free(ca);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn->closing = true;
  conn_arm(srv, conn);
}

static void handle_device_list(wd_server *srv, wd_conn *conn, uint64_t id) {
  wd_pki_device *devices = NULL;
  size_t count = 0;
  char failure[256] = {0};
  if (wd_pki_devices(srv->pki, &devices, &count, failure, sizeof(failure)) != 0) {
    conn_send_error(conn, id, WD_ERR_INTERNAL, failure);
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *list = wd_array(count);
  for (size_t i = 0; i < count; i++) {
    cbor_item_t *entry = wd_map(5);
    wd_put(entry, "serial", wd_text(devices[i].serial));
    wd_put(entry, "name", wd_text(devices[i].name));
    wd_put(entry, "scope", wd_text(wd_scope_name(devices[i].scope)));
    wd_put(entry, "issued_ms", wd_uint(devices[i].issued_ms));
    wd_put(entry, "revoked", cbor_build_bool(devices[i].revoked));
    bool pushed = cbor_array_push(list, entry);
    cbor_decref(&entry);
    if (!pushed) {
      conn->closing = true;
      break;
    }
  }
  free(devices);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "devices", list);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_device_revoke(wd_server *srv, wd_conn *conn, uint64_t id,
                                 cbor_item_t *params) {
  char *serial = wd_text_copy(wd_get(params, "serial"));
  if (serial == NULL) {
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "serial is required");
    conn_arm(srv, conn);
    return;
  }
  char failure[256] = {0};
  if (wd_pki_revoke(srv->pki, serial, failure, sizeof(failure)) != 0) {
    free(serial);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, failure);
    conn_arm(srv, conn);
    return;
  }
  for (size_t i = 0; i < WD_MAX_CONNS; i++) {
    wd_conn *other = srv->conns[i];
    if (other == NULL || other->stream == NULL) continue;
    const char *live = wd_quic_stream_serial(other->stream);
    if (live != NULL && strcmp(live, serial) == 0) conn_close(srv, other);
  }
  free(serial);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "revoked", cbor_build_bool(true));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_request(wd_server *srv, wd_conn *conn, uint64_t id,
                           const char *method, cbor_item_t *params) {
  if (conn->stream != NULL) {
    if (!wd_quic_stream_authenticated(conn->stream) &&
        strcmp(method, "device.enrol") != 0) {
      conn_send_error(conn, id, WD_ERR_UNAUTHENTICATED,
                      "enrol a device certificate first");
      conn->closing = true;
      conn_arm(srv, conn);
      return;
    }
    if (admin_method(method)) {
      conn_send_error(conn, id, WD_ERR_PERMISSION_DENIED,
                      "device administration is local only");
      conn_arm(srv, conn);
      return;
    }
    const char *scope = wd_quic_stream_scope(conn->stream);
    if (scope != NULL && strcmp(scope, "readonly") == 0 &&
        (strcmp(method, "session.prompt") == 0 ||
         strcmp(method, "approval.respond") == 0 ||
         strcmp(method, "session.cancel") == 0)) {
      conn_send_error(conn, id, WD_ERR_PERMISSION_DENIED,
                      "the device scope is read only");
      conn_arm(srv, conn);
      return;
    }
  } else if (strcmp(method, "device.enrol") == 0) {
    conn_send_error(conn, id, WD_ERR_NOT_SUPPORTED,
                    "enrolment is served over quic only");
    conn_arm(srv, conn);
    return;
  }
  if (strcmp(method, "session.create") == 0) {
    handle_session_create(srv, conn, id, params);
  } else if (strcmp(method, "session.list") == 0) {
    handle_session_list(srv, conn, id, params);
  } else if (strcmp(method, "session.get") == 0) {
    handle_session_get(srv, conn, id, params);
  } else if (strcmp(method, "session.resume") == 0) {
    handle_session_resume(srv, conn, id, params);
  } else if (strcmp(method, "stats.session") == 0) {
    handle_stats_session(srv, conn, id, params);
  } else if (strcmp(method, "stats.global") == 0) {
    handle_stats_global(srv, conn, id);
  } else if (strcmp(method, "session.rename") == 0) {
    handle_session_label(srv, conn, id, params, 0);
  } else if (strcmp(method, "session.archive") == 0) {
    handle_session_label(srv, conn, id, params, 1);
  } else if (strcmp(method, "session.pin") == 0) {
    handle_session_label(srv, conn, id, params, 2);
  } else if (strcmp(method, "session.compact") == 0) {
    handle_session_compact(srv, conn, id, params);
  } else if (strcmp(method, "session.checkpoint") == 0) {
    handle_session_checkpoint(srv, conn, id, params);
  } else if (strcmp(method, "session.undo") == 0) {
    handle_session_undo(srv, conn, id, params);
  } else if (strcmp(method, "session.search") == 0) {
    handle_session_search(srv, conn, id, params);
  } else if (strcmp(method, "session.cancel") == 0) {
    handle_session_cancel(srv, conn, id, params);
  } else if (strcmp(method, "session.fork") == 0) {
    handle_session_fork(srv, conn, id, params);
  } else if (strcmp(method, "session.set_model") == 0) {
    handle_session_set_model(srv, conn, id, params);
  } else if (strcmp(method, "model.list") == 0) {
    handle_model_list(srv, conn, id);
  } else if (strcmp(method, "model.select") == 0) {
    handle_model_select(srv, conn, id, params);
  } else if (strcmp(method, "session.prompt") == 0) {
    handle_session_prompt(srv, conn, id, params);
  } else if (strcmp(method, "device.pair") == 0) {
    handle_device_pair(srv, conn, id, params);
  } else if (strcmp(method, "device.enrol") == 0) {
    handle_device_enrol(srv, conn, id, params);
  } else if (strcmp(method, "device.list") == 0) {
    handle_device_list(srv, conn, id);
  } else if (strcmp(method, "approval.respond") == 0) {
    handle_approval_respond(srv, conn, id, params);
  } else if (strcmp(method, "device.revoke") == 0) {
    handle_device_revoke(srv, conn, id, params);
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
    wd_session *session = wd_session_find(&srv->store, conn->subs[i].session_id);
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

static void conn_release(wd_server *srv, wd_conn *conn) {
  if (conn->fd >= 0) close(conn->fd);
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

static void conn_close(wd_server *srv, wd_conn *conn) {
  if (conn->stream != NULL) {
    wd_quic_stream *stream = conn->stream;
    conn->stream = NULL;
    wd_quic_stream_close(stream);
    conn_release(srv, conn);
    return;
  }
  epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
  conn_release(srv, conn);
}

static wd_conn *conn_for_stream(wd_server *srv, wd_quic_stream *stream) {
  for (size_t i = 0; i < WD_MAX_CONNS; i++) {
    wd_conn *conn = srv->conns[i];
    if (conn != NULL && conn->stream == stream) return conn;
  }
  return NULL;
}

static void quic_open(void *ctx, wd_quic_stream *stream) {
  wd_server *srv = ctx;
  size_t slot = WD_MAX_CONNS;
  for (size_t i = 0; i < WD_MAX_CONNS; i++)
    if (srv->conns[i] == NULL) {
      slot = i;
      break;
    }
  if (slot == WD_MAX_CONNS) {
    wd_quic_stream_close(stream);
    return;
  }
  wd_conn *conn = calloc(1, sizeof(*conn));
  if (conn == NULL) {
    wd_quic_stream_close(stream);
    return;
  }
  conn->fd = -1;
  conn->stream = stream;
  conn->deadline_ms = wd_now_ms() + WD_HANDSHAKE_MS;
  conn->next_sub = 1;
  srv->conns[slot] = conn;
}

static void quic_frame(void *ctx, wd_quic_stream *stream, const wd_frame *frame) {
  wd_server *srv = ctx;
  wd_conn *conn = conn_for_stream(srv, stream);
  if (conn == NULL) return;
  handle_frame(srv, conn, frame);
  if (srv->quic != NULL) wd_quic_flush(srv->quic);
}

static void quic_closed(void *ctx, wd_quic_stream *stream) {
  wd_server *srv = ctx;
  wd_conn *conn = conn_for_stream(srv, stream);
  if (conn == NULL) return;
  conn->stream = NULL;
  conn_release(srv, conn);
}

static int conn_flush(wd_conn *conn) {
  while (conn->out_head != NULL) {
    wd_out *out = conn->out_head;
    if (conn->stream != NULL) {
      if (wd_quic_stream_write(conn->stream, out->data + out->off,
                               out->len - out->off) != 0)
        return -1;
      conn->out_bytes -= out->len - out->off;
    } else {
      ssize_t n = write(conn->fd, out->data + out->off, out->len - out->off);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
      }
      out->off += (size_t)n;
      conn->out_bytes -= (size_t)n;
      if (out->off < out->len) return 0;
    }
    conn->out_head = out->next;
    if (conn->out_head == NULL) conn->out_tail = NULL;
    free(out->data);
    free(out);
  }
  return 0;
}

static int conn_arm(wd_server *srv, wd_conn *conn) {
  if (conn->stream != NULL) {
    if (conn_flush(conn) != 0) return -1;
    if (srv->quic != NULL) wd_quic_flush(srv->quic);
    return 0;
  }
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
  int quic_timeout = srv->quic != NULL ? wd_quic_next_timeout(srv->quic, now) : -1;
  if (best == 0 && quic_timeout < 0) return -1;
  int socket_timeout = -1;
  if (best != 0) {
    if (best <= now)
      socket_timeout = 0;
    else {
      uint64_t delta = best - now;
      socket_timeout = delta > 60000 ? 60000 : (int)delta;
    }
  }
  if (quic_timeout >= 0 && (socket_timeout < 0 || quic_timeout < socket_timeout))
    socket_timeout = quic_timeout;
  return socket_timeout;
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
    conn->deadline_ms = wd_now_ms() + WD_HANDSHAKE_MS;
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
  char *boot = wd_boot_id();
  if (boot == NULL) return -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) {
      char body[256];
      int n = snprintf(body, sizeof(body),
                       "{\"pid\":%ld,\"boot_id\":\"%s\",\"started_at\":%llu}\n",
                       (long)getpid(), boot, (unsigned long long)wd_now_ms());
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
    if (same_boot && wd_pid_alive(pid)) {
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

static char *endpoint_text(const char *bind, int port) {
  const char *colon = strrchr(bind, ':');
  size_t host_len = colon != NULL ? (size_t)(colon - bind) : strlen(bind);
  char *text = malloc(host_len + 16);
  if (text == NULL) return NULL;
  memcpy(text, bind, host_len);
  snprintf(text + host_len, 16, ":%d", port);
  return text;
}

static int setup_quic(wd_server *srv) {
  const char *bind = getenv("WARD_QUIC_BIND");
  if (bind != NULL && strcmp(bind, "off") == 0) return 0;
  const char *spec = bind != NULL && bind[0] != '\0' ? bind : WD_QUIC_DEFAULT_BIND;
  char *text = strdup(spec);
  if (text == NULL) return -1;
  wd_quic_config cfg = {.bind = text, .pki = srv->pki};
  wd_quic_events events = {.stream_open = quic_open,
                           .stream_frame = quic_frame,
                           .stream_close = quic_closed};
  char *err = NULL;
  srv->quic = wd_quic_new(&cfg, &events, srv, &err);
  free(text);
  if (srv->quic == NULL) {
    fprintf(stderr, "ward: quic: %s\n", err != NULL ? err : "cannot listen");
    free(err);
    return -1;
  }
  srv->quic_port = wd_quic_port(srv->quic);
  srv->quic_endpoint = endpoint_text(spec, srv->quic_port);
  if (srv->quic_endpoint == NULL) return -1;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = &token_quic;
  if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, wd_quic_fd(srv->quic), &ev) != 0)
    return -1;
  return 0;
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
  if (wd_mkdir_p(srv.state_dir, 0700) != 0) {
    fprintf(stderr, "ward: cannot create %s: %s\n", srv.state_dir,
            strerror(errno));
    goto cleanup;
  }
  srv.lock_path = wd_path_join(srv.state_dir, "ward.lock");
  if (srv.lock_path == NULL) goto cleanup;
  if (acquire_lock(srv.lock_path) != 0) {
    fprintf(stderr, "ward: %s is already served by another daemon\n",
            srv.state_dir);
    goto cleanup;
  }
  if (wd_store_open(&srv.store, srv.state_dir) != 0) {
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
  srv.approvals = wd_approvals_new();
  if (srv.approvals == NULL) goto cleanup;
  char *labels_err = NULL;
  srv.labels = wd_labels_open(srv.state_dir, &labels_err);
  if (srv.labels == NULL) {
    fprintf(stderr, "ward: labels: %s\n",
            labels_err != NULL ? labels_err : "cannot open labels.db");
    free(labels_err);
    goto cleanup;
  }
  char *models_path = wd_models_path(srv.state_dir);
  if (models_path != NULL) {
    char *models_err = NULL;
    srv.models = wd_models_load(models_path, &models_err);
    free(models_path);
    if (srv.models == NULL) {
      fprintf(stderr, "ward: models: %s\n",
              models_err != NULL ? models_err : "cannot read models.toml");
      free(models_err);
      goto cleanup;
    }
  }
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

  char *pki_dir = wd_path_join(srv.state_dir, "pki");
  char *pki_err = NULL;
  if (pki_dir == NULL) goto cleanup;
  srv.pki = wd_pki_open(pki_dir, &pki_err);
  free(pki_dir);
  if (srv.pki == NULL) {
    fprintf(stderr, "ward: pki: %s\n",
            pki_err != NULL ? pki_err : "cannot open");
    free(pki_err);
    goto cleanup;
  }
  free(pki_err);
  if (setup_quic(&srv) != 0) goto cleanup;
  char fingerprint[65];
  wd_pki_fingerprint(srv.pki, fingerprint);
  printf("ready %s\nquic %s %s\n", srv.socket_path,
         srv.quic_endpoint != NULL ? srv.quic_endpoint : "off", fingerprint);
  fflush(stdout);

  struct epoll_event events[64];
  while (!srv.stop) {
    uint64_t now = wd_now_ms();
    int timeout = next_timeout(&srv, now);
    int n = epoll_wait(srv.epoll_fd, events, 64, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0 && srv.quic != NULL) {
      (void)wd_quic_service(srv.quic, wd_now_ms());
      (void)wd_quic_flush(srv.quic);
    }
    for (int i = 0; i < n; i++) {
      void *ptr = events[i].data.ptr;
      if (ptr == &token_listen) {
        server_accept(&srv);
        continue;
      }
      if ((uintptr_t)ptr >= (uintptr_t)&run_tokens[0] &&
          (uintptr_t)ptr < (uintptr_t)&run_tokens[WD_MAX_RUNS]) {
        wd_run_token *tok = ptr;
        if (tok->run != NULL) {
          if (tok->kind == 2)
            tool_done(&srv, tok->run);
          else
            http_done(&srv, tok->run);
        }
        continue;
      }
      if (ptr == &token_quic) {
        if (wd_quic_service(srv.quic, wd_now_ms()) != 0) {
          fprintf(stderr, "ward: quic socket failed\n");
          srv.stop = true;
          break;
        }
        wd_quic_flush(srv.quic);
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
    sweep(&srv, wd_now_ms());
  }

cleanup:
  for (size_t i = 0; i < WD_MAX_CONNS; i++)
    if (srv.conns[i] != NULL && srv.epoll_fd >= 0) conn_close(&srv, srv.conns[i]);
  if (srv.socket_path != NULL) unlink(srv.socket_path);
  if (srv.lock_path != NULL) unlink(srv.lock_path);
  if (srv.listen_fd >= 0) close(srv.listen_fd);
  if (srv.epoll_fd >= 0) close(srv.epoll_fd);
  if (srv.signal_fd >= 0) close(srv.signal_fd);
  while (srv.runs != NULL) run_free(&srv, srv.runs);
  wd_engine_free(srv.engine);
  wd_quic_free(srv.quic);
  wd_pki_free(srv.pki);
  wd_approvals_free(srv.approvals);
  wd_models_free(srv.models);
  wd_labels_free(srv.labels);
  free(srv.default_model);
  free(srv.quic_endpoint);
  wd_store_close(&srv.store);
  free(srv.socket_path);
  free(srv.state_dir);
  free(srv.lock_path);
  return srv.stop ? 0 : 1;
}
