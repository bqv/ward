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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "core/approval.h"
#include "core/ids.h"
#include "core/labels.h"
#include "core/models.h"
#include "core/credentials.h"
#include "core/manifest.h"
#include "core/modstate.h"
#include "core/prompt.h"
#include "core/devlock.h"
#include "core/probe.h"
#include "net/http.h"
#include "core/pty.h"
#include "core/registry.h"
#include "core/schedule.h"
#include "core/session.h"
#include "core/util.h"
#include "net/pki.h"
#include "net/quic.h"
#include "proto/codec.h"
#include "proto/frame.h"
#include "proto/sse.h"

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

typedef struct wd_queue {
  struct wd_queue *next;
  char session[WD_ID_LEN];
  char *text;
  char job[WD_ID_LEN];
} wd_queue;

typedef struct wd_sched {
  struct wd_sched *next;
  char id[16];
  char session[WD_ID_LEN];
  char *cron;
  char *text;
  uint64_t next_ms;
} wd_sched;

struct wd_run {
  struct wd_run *next;
  char session_id[WD_ID_LEN];
  wd_turn *turn;
  int worker_fd;
  int http_fd;
  wd_device_lock devlock;
  uint8_t *http_body;
  size_t http_len;
  size_t http_cap;
  wd_sse *sse;
  void *sse_ctx;
  int http_status_seen;
  size_t streamed;
};


typedef struct {
  wd_run *run;
  int kind;
} wd_run_token;

#define WD_MAX_TERMS 8

typedef struct {
  char id[17];
  wd_session *session;
  wd_pty *pty;
  bool used;
} wd_term;

typedef struct {
  wd_term *term;
} wd_term_token;

static wd_term terminals[WD_MAX_TERMS];
static wd_term_token term_tokens[WD_MAX_TERMS];

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
  int tcp_fd;
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
  wd_credentials *credentials;
  wd_labels *labels;
  wd_scheduler *scheduler;
  wd_sched *schedules;
  wd_queue *queue;
  struct {
    char name[32];
    int slots;
    int inflight;
  } devices[8];
  size_t devices_len;
  wd_registry registry;
  char *module_dir;
  size_t max_runs;
  char *default_model;
  char *quic_endpoint;
  int quic_port;
  wd_conn *conns[WD_MAX_CONNS];
  bool stop;
} wd_server;

static int token_listen;
static int token_tcp;
static int token_signal;
static int token_quic;
static wd_run_token run_tokens[WD_MAX_RUNS];

static void conn_close(wd_server *srv, wd_conn *conn);
static void conn_release(wd_server *srv, wd_conn *conn);
static cbor_item_t *session_cbor(wd_server *srv, wd_session *session);
static void handle_session_prompt(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params);
typedef struct {
  wd_server *srv;
  wd_run *run;
} stream_ctx;

static int stream_event(void *ctx, const wd_sse_event *event);
static wd_run *run_new(wd_server *srv, wd_session *session, char **err);
static int apply_state(wd_server *srv, wd_session *session, wd_run *run);
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
  cbor_item_t *item = wd_map(9);
  wd_put(item, "id", wd_text(model->id));
  wd_put(item, "provider",
         wd_text(model->provider != NULL ? model->provider : ""));
  wd_put(item, "model", wd_text(model->model != NULL ? model->model : ""));
  wd_put(item, "context",
         wd_text(model->context != NULL ? model->context : ""));
  wd_put(item, "tags", wd_text(model->tags != NULL ? model->tags : ""));
  wd_put(item, "max_output",
         wd_text(model->max_output != NULL ? model->max_output : ""));
  wd_put(item, "device", wd_text(model->device != NULL ? model->device : ""));
  wd_put(item, "slots", wd_text(model->slots != NULL ? model->slots : ""));
  wd_put(item, "stop", wd_text(model->stop != NULL ? model->stop : ""));
  return item;
}

static int probe_model(wd_server *srv, const wd_model *model, int *n_ctx,
                       int *slots, const char **source) {
  if (model == NULL) return -1;
  *n_ctx = 0;
  *slots = 0;
  *source = "default";
  const char *endpoint = model->endpoint;
  if (endpoint != NULL && endpoint[0] != '\0') {
    const char *key = model->key != NULL
                          ? wd_credentials_find(srv->credentials, model->key)
                          : NULL;
    char url[600];
    snprintf(url, sizeof(url), "%s/props", endpoint);
    wd_http_response response;
    char *err = NULL;
    if (wd_http_request(url, "GET", NULL, key, 2000, 64 * 1024, &response,
                        &err) == 0) {
      if (response.status == 200 && response.body != NULL) {
        char *body = strndup((const char *)response.body, response.len);
        if (body != NULL && wd_probe_scan(body, n_ctx, slots) == 0)
          *source = "server";
        free(body);
      }
      wd_http_response_free(&response);
    }
    free(err);
    if (*n_ctx == 0 && model->model != NULL) {
      snprintf(url, sizeof(url), "%s/v1/models", endpoint);
      wd_http_response list_response;
      char *list_err = NULL;
      if (wd_http_request(url, "GET", NULL, key, 2000, 256 * 1024,
                          &list_response, &list_err) == 0) {
        if (list_response.status == 200 && list_response.body != NULL) {
          char *body =
              strndup((const char *)list_response.body, list_response.len);
          if (body != NULL &&
              wd_probe_router_models(body, model->model, n_ctx, slots) == 0)
            *source = "server";
          free(body);
        }
        wd_http_response_free(&list_response);
      }
      free(list_err);
    }
  }
  if (*n_ctx == 0) {
    int parsed = wd_context_from_text(model->context);
    if (parsed > 0) {
      *n_ctx = parsed;
      *source = "config";
    }
  }
  if (*n_ctx == 0) *n_ctx = 8192;
  if (*slots <= 0) *slots = 1;
  return 0;
}

static size_t history_budget(wd_server *srv, wd_session *session) {
  const wd_model *model = wd_models_find(srv->models, session->model);
  int n_ctx = 0;
  int slots = 0;
  const char *source = "default";
  if (probe_model(srv, model, &n_ctx, &slots, &source) != 0 || n_ctx <= 0)
    return 8192;
  int max_output = model->max_output != NULL ? atoi(model->max_output) : 1024;
  return wd_context_budget(n_ctx, max_output);
}

static void handle_model_probe(wd_server *srv, wd_conn *conn, uint64_t id,
                               cbor_item_t *params) {
  char *name = wd_text_copy(wd_get(params, "id"));
  const wd_model *model = wd_models_find(srv->models, name);
  if (model == NULL) {
    free(name);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such model");
    conn_arm(srv, conn);
    return;
  }
  int n_ctx = 0;
  int slots = 0;
  const char *source = "default";
  (void)probe_model(srv, model, &n_ctx, &slots, &source);
  cbor_item_t *result = wd_map(4);
  wd_put(result, "id", wd_text(model->id));
  wd_put(result, "n_ctx", wd_uint((uint64_t)n_ctx));
  wd_put(result, "n_slots", wd_uint((uint64_t)slots));
  wd_put(result, "context_source", wd_text(source));
  free(name);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
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

static void handle_session_set_system(wd_server *srv, wd_conn *conn,
                                      uint64_t id, cbor_item_t *params) {
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
  if (text == NULL) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "text is needed");
    conn_arm(srv, conn);
    return;
  }
  uint64_t seq = 0;
  int rc = wd_session_set_system(session, text, &seq);
  free(session_id);
  free(text);
  if (rc != 0) {
    conn_send_error(conn, id, WD_ERR_INTERNAL, "cannot set the system prompt");
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "session", wd_session_to_cbor(session));
  wd_put(result, "seq", wd_uint(seq));
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
  cbor_item_t *result = wd_map(priced ? 13 : 10);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "model", wd_text(session->model));
  wd_put(result, "events", wd_uint(stats.events));
  wd_put(result, "turns", wd_uint(stats.turns));
  wd_put(result, "text_bytes", wd_uint(stats.text_bytes));
  wd_put(result, "prompt_bytes", wd_uint(stats.prompt_bytes));
  wd_put(result, "prompt_tokens", wd_uint(stats.prompt_tokens));
  wd_put(result, "predicted_tokens", wd_uint(stats.predicted_tokens));
  wd_put(result, "cache_tokens", wd_uint(stats.cache_tokens));
  wd_put(result, "tokens_estimated",
         cbor_build_bool(stats.predicted_tokens == 0));
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
      total.prompt_tokens += one.prompt_tokens;
      total.predicted_tokens += one.predicted_tokens;
      total.cache_tokens += one.cache_tokens;
    }
  }
  cbor_item_t *result = wd_map(8);
  wd_put(result, "sessions", wd_uint(srv->store.len));
  wd_put(result, "events", wd_uint(total.events));
  wd_put(result, "turns", wd_uint(total.turns));
  wd_put(result, "text_bytes", wd_uint(total.text_bytes));
  wd_put(result, "prompt_bytes", wd_uint(total.prompt_bytes));
  wd_put(result, "prompt_tokens", wd_uint(total.prompt_tokens));
  wd_put(result, "predicted_tokens", wd_uint(total.predicted_tokens));
  wd_put(result, "cache_tokens", wd_uint(total.cache_tokens));
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
    conn_send_error(conn, id, WD_ERR_BUSY, "nothing to compact");
    conn_arm(srv, conn);
    return;
  }
  if (run_find(srv, session->id) != NULL) {
    wd_free(history);
    conn_send_error(conn, id, WD_ERR_BUSY, "the session is busy");
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
  bool in_place = false;
  cbor_item_t *flag = wd_get(params, "in_place");
  if (flag != NULL) in_place = cbor_get_bool(flag);
  wd_session *target = session;
  bool branched = false;
  if (!in_place) {
    wd_session *child = NULL;
    if (wd_session_fork(&srv->store, session, session->last_seq, &child) != 0) {
      free(summary);
      respond_internal(srv, conn, id, "cannot branch the session");
      return;
    }
    target = child;
    branched = true;
  }
  cbor_item_t *record = wd_map(2);
  wd_put(record, "through", wd_uint(target->last_seq));
  wd_put(record, "summary", wd_text(summary));
  uint64_t seq = 0;
  if (wd_session_emit(target, "session.compacted", record, &seq) != 0) {
    free(summary);
    respond_internal(srv, conn, id, "cannot record the summary");
    return;
  }
  cbor_item_t *event = wd_map(2);
  wd_put(event, "through", wd_uint(seq - 1));
  wd_put(event, "summary", wd_text(summary));
  conn_broadcast(srv, target, "session.compacted", seq, event);
  cbor_item_t *result = wd_map(5);
  wd_put(result, "session", wd_text(target->id));
  wd_put(result, "parent", wd_text(branched ? session->id : target->id));
  wd_put(result, "branched", cbor_build_bool(branched));
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
    conn_send_error(conn, id, WD_ERR_BUSY,
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
  if (strcmp(event, "text.delta") == 0 && tc->session != NULL) {
    wd_run *streaming_run = run_find(tc->srv, tc->session->id);
    if (streaming_run != NULL && streaming_run->streamed > 0) return 0;
  }
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

static void handle_goal_set(wd_server *srv, wd_conn *conn, uint64_t id,
                            cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "session"));
  char *text = wd_text_copy(wd_get(params, "text"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL || text == NULL || text[0] == '\0') {
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "session and text are required");
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *record = wd_map(1);
  wd_put(record, "text", wd_text(text));
  uint64_t seq = 0;
  if (wd_session_emit(session, "goal.set", record, &seq) != 0) {
    free(text);
    respond_internal(srv, conn, id, "cannot record the goal");
    return;
  }
  cbor_item_t *event = wd_map(1);
  wd_put(event, "text", wd_text(text));
  conn_broadcast(srv, session, "goal.set", seq, event);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "goal", wd_text(text));
  free(text);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_goal_clear(wd_server *srv, wd_conn *conn, uint64_t id,
                              cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "session"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    conn_arm(srv, conn);
    return;
  }
  uint64_t seq = 0;
  if (wd_session_emit(session, "goal.cleared", wd_map(0), &seq) != 0) {
    respond_internal(srv, conn, id, "cannot record the goal");
    return;
  }
  cbor_item_t *event = wd_map(0);
  conn_broadcast(srv, session, "goal.cleared", seq, event);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "cleared", cbor_build_bool(true));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_job_list(wd_server *srv, wd_conn *conn, uint64_t id) {
  size_t count = 0;
  for (wd_run *run = srv->runs; run != NULL; run = run->next) count++;
  for (wd_queue *item = srv->queue; item != NULL; item = item->next) count++;
  cbor_item_t *list = wd_array(count);
  for (wd_run *run = srv->runs; run != NULL; run = run->next) {
    cbor_item_t *job = wd_map(4);
    wd_put(job, "job", wd_text(run->session_id));
    wd_put(job, "session", wd_text(run->session_id));
    wd_put(job, "state", wd_text("running"));
    wd_put(job, "ahead", wd_uint(0));
    bool pushed = cbor_array_push(list, job);
    cbor_decref(&job);
    if (!pushed) break;
  }
  size_t ahead = 1;
  for (wd_queue *item = srv->queue; item != NULL; item = item->next) {
    cbor_item_t *job = wd_map(4);
    wd_put(job, "job", wd_text(item->job));
    wd_put(job, "session", wd_text(item->session));
    wd_put(job, "state", wd_text("queued"));
    wd_put(job, "ahead", wd_uint(ahead++));
    bool pushed = cbor_array_push(list, job);
    cbor_decref(&job);
    if (!pushed) break;
  }
  size_t device_count = srv->devices_len;
  cbor_item_t *devices = wd_array(device_count);
  for (size_t i = 0; i < device_count; i++) {
    cbor_item_t *entry = wd_map(3);
    wd_put(entry, "device", wd_text(srv->devices[i].name));
    wd_put(entry, "inflight", wd_uint((uint64_t)srv->devices[i].inflight));
    wd_put(entry, "slots", wd_uint((uint64_t)srv->devices[i].slots));
    bool pushed = cbor_array_push(devices, entry);
    cbor_decref(&entry);
    if (!pushed) break;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "jobs", list);
  wd_put(result, "devices", devices);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_job_kill(wd_server *srv, wd_conn *conn, uint64_t id,
                            cbor_item_t *params) {
  char *job = wd_text_copy(wd_get(params, "job"));
  wd_run *run = job != NULL ? run_find(srv, job) : NULL;
  bool killed = false;
  for (wd_queue **at = &srv->queue; *at != NULL; at = &(*at)->next) {
    if (job != NULL && strcmp((*at)->job, job) == 0) {
      wd_queue *item = *at;
      *at = item->next;
      free(item->text);
      free(item);
      killed = true;
      break;
    }
  }
  if (run != NULL) {
    wd_session *session = wd_session_find(&srv->store, run->session_id);
    if (session != NULL) {
      wd_turn_ctx tc = {srv, session};
      char *err = NULL;
      if (wd_turn_abort(run->turn, "killed", turn_emit, &tc, &err) == 0) {
        killed = true;
        cbor_item_t *notice = wd_map(2);
        wd_put(notice, "job", wd_text(run->session_id));
        wd_put(notice, "state", wd_text("killed"));
        uint64_t seq = 0;
        if (wd_session_emit(session, "job.updated", notice, &seq) == 0) {
          cbor_item_t *event = wd_map(2);
          wd_put(event, "job", wd_text(run->session_id));
          wd_put(event, "state", wd_text("killed"));
          conn_broadcast(srv, session, "job.updated", seq, event);
        }
        run_free(srv, run);
      }
      free(err);
    }
  }
  free(job);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "killed", cbor_build_bool(killed));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static size_t active_runs(wd_server *srv) {
  size_t count = 0;
  for (wd_run *run = srv->runs; run != NULL; run = run->next) count++;
  return count;
}

static void start_session_turn(wd_server *srv, wd_session *session,
                               const char *text) {
  char *err = NULL;
  wd_run *run = run_new(srv, session, &err);
  if (run != NULL) {
    cbor_item_t *history = NULL;
    if (wd_session_history(session, history_budget(srv, session), &history) == 0)
      wd_turn_set_history(run->turn, history);
    cbor_item_t *asked = wd_map(1);
    wd_put(asked, "text", wd_text(text));
    uint64_t seq = 0;
    char when[64];
    wd_now_text(when, sizeof(when));
    cbor_item_t *time_event = wd_map(1);
    wd_put(time_event, "text", wd_text(when));
    cbor_item_t *time_payload = wd_map(1);
    wd_put(time_payload, "text", wd_text(when));
    uint64_t time_seq = 0;
    if (wd_session_emit(session, "session.time", time_payload, &time_seq) == 0)
      conn_broadcast(srv, session, "session.time", time_seq, time_event);
    if (wd_session_emit(session, "session.prompted", asked, &seq) == 0) {
      cbor_item_t *event = wd_map(1);
      wd_put(event, "text", wd_text(text));
      conn_broadcast(srv, session, "session.prompted", seq, event);
    }
    wd_turn_ctx tc = {srv, session};
    int rc = wd_turn_prompt(run->turn, text, turn_emit, &tc, &err);
    run_after(srv, run, rc, NULL);
  }
  free(err);
}

static int device_slot_for(wd_server *srv, wd_session *session, int *slots_out) {
  if (session == NULL) return -1;
  const wd_model *model = wd_models_find(srv->models, session->model);
  if (model == NULL || model->device == NULL || model->device[0] == '\0')
    return -1;
  int slots = model->slots != NULL ? atoi(model->slots) : 1;
  if (slots <= 0) slots = 1;
  if (slots_out != NULL) *slots_out = slots;
  for (size_t i = 0; i < srv->devices_len; i++) {
    if (strcmp(srv->devices[i].name, model->device) != 0) continue;
    return (int)i;
  }
  if (srv->devices_len >= sizeof(srv->devices) / sizeof(srv->devices[0]))
    return -1;
  size_t at = srv->devices_len++;
  snprintf(srv->devices[at].name, sizeof(srv->devices[at].name), "%s",
           model->device);
  srv->devices[at].slots = slots;
  srv->devices[at].inflight = 0;
  return (int)at;
}

static bool device_free(wd_server *srv, wd_session *session) {
  int slot = device_slot_for(srv, session, NULL);
  if (slot < 0) return true;
  if (srv->devices[slot].inflight >= srv->devices[slot].slots) return false;
  wd_device_lock probe;
  int rc = wd_device_acquire(srv->state_dir, srv->devices[slot].name,
                             srv->devices[slot].slots, &probe);
  if (rc != 0) return false;
  wd_device_release(&probe);
  return true;
}

static int device_acquire(wd_server *srv, wd_session *session,
                          wd_device_lock *lock) {
  int slot = device_slot_for(srv, session, NULL);
  if (slot < 0) return 0;
  int rc = wd_device_acquire(srv->state_dir, srv->devices[slot].name,
                             srv->devices[slot].slots, lock);
  if (rc != 0) return rc;
  srv->devices[slot].inflight++;
  return 0;
}

static void device_release(wd_server *srv, wd_session *session,
                           wd_device_lock *lock) {
  wd_device_release(lock);
  int slot = device_slot_for(srv, session, NULL);
  if (slot >= 0 && srv->devices[slot].inflight > 0) srv->devices[slot].inflight--;
}

static void drain_queue(wd_server *srv) {
  while (srv->queue != NULL && active_runs(srv) < srv->max_runs) {
    wd_queue *item = srv->queue;
    wd_session *head = wd_session_find(&srv->store, item->session);
    if (head != NULL && !device_free(srv, head)) break;
    srv->queue = item->next;
    wd_session *session = wd_session_find(&srv->store, item->session);
    if (session != NULL) start_session_turn(srv, session, item->text);
    free(item->text);
    free(item);
  }
}

static bool enqueue_turn(wd_server *srv, wd_session *session, const char *text,
                         const char *job) {
  for (wd_queue *at = srv->queue; at != NULL; at = at->next)
    if (strcmp(at->session, session->id) == 0) return false;
  wd_queue *item = calloc(1, sizeof(*item));
  if (item == NULL) return false;
  snprintf(item->session, sizeof(item->session), "%s", session->id);
  snprintf(item->job, sizeof(item->job), "%s", job != NULL ? job : session->id);
  item->text = strdup(text);
  if (item->text == NULL) {
    free(item);
    return false;
  }
  item->next = NULL;
  if (srv->queue == NULL) {
    srv->queue = item;
  } else {
    wd_queue *tail = srv->queue;
    while (tail->next != NULL) tail = tail->next;
    tail->next = item;
  }
  return true;
}

static void handle_subagent_prompt(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *parent_id = wd_text_copy(wd_get(params, "session"));
  char *text = wd_text_copy(wd_get(params, "text"));
  wd_session *parent =
      parent_id != NULL ? wd_session_find(&srv->store, parent_id) : NULL;
  if (parent == NULL || text == NULL || text[0] == '\0') {
    free(parent_id);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "session and text are required");
    conn_arm(srv, conn);
    return;
  }
  wd_session *child = NULL;
  if (wd_session_fork(&srv->store, parent, parent->last_seq, &child) != 0) {
    free(parent_id);
    free(text);
    respond_internal(srv, conn, id, "cannot create the subagent");
    return;
  }
  cbor_item_t *notice = wd_map(3);
  wd_put(notice, "subagent", wd_text(child->id));
  wd_put(notice, "parent", wd_text(parent->id));
  wd_put(notice, "state", wd_text("running"));
  uint64_t seq = 0;
  if (wd_session_emit(parent, "subagent.updated", notice, &seq) == 0) {
    cbor_item_t *event = wd_map(3);
    wd_put(event, "subagent", wd_text(child->id));
    wd_put(event, "parent", wd_text(parent->id));
    wd_put(event, "state", wd_text("running"));
    conn_broadcast(srv, parent, "subagent.updated", seq, event);
  }
  free(parent_id);
  if (srv->max_runs != 0 && active_runs(srv) >= srv->max_runs &&
      enqueue_turn(srv, child, text, child->id)) {
    cbor_item_t *queued = wd_map(3);
    wd_put(queued, "session", wd_text(child->id));
    wd_put(queued, "queued", cbor_build_bool(true));
    wd_put(queued, "turn", wd_uint(0));
    free(text);
    conn_send_cached(conn, id, wd_response_ok(id, queued));
    conn_arm(srv, conn);
    return;
  }
  cbor_item_t *sub = wd_map(2);
  wd_put(sub, "id", wd_text(child->id));
  wd_put(sub, "text", wd_text(text));
  free(text);
  handle_session_prompt(srv, conn, id, sub);
  wd_free(sub);
}

static void handle_subagent_interrupt(wd_server *srv, wd_conn *conn,
                                      uint64_t id, cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "session"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  int stopped = 0;
  if (session != NULL) {
    wd_run *run = run_find(srv, session->id);
    if (run != NULL) {
      wd_turn_ctx tc = {srv, session};
      char *err = NULL;
      stopped = wd_turn_abort(run->turn, "interrupted", turn_emit, &tc, &err) == 0;
      free(err);
      if (stopped) run_free(srv, run);
    }
  }
  free(session_id);
  cbor_item_t *result = wd_map(2);
  wd_put(result, "interrupted", cbor_build_bool(stopped != 0));
  wd_put(result, "stopped", cbor_build_bool(stopped != 0));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static wd_term *term_find(const char *handle) {
  if (handle == NULL) return NULL;
  for (size_t i = 0; i < WD_MAX_TERMS; i++) {
    if (terminals[i].used && strcmp(terminals[i].id, handle) == 0)
      return &terminals[i];
  }
  return NULL;
}

static void term_drop(wd_server *srv, wd_term *term) {
  if (term == NULL || !term->used) return;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  (void)epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, wd_pty_fd(term->pty), &ev);
  wd_pty_close(term->pty);
  memset(term, 0, sizeof(*term));
}

static void term_announce(wd_server *srv, wd_term *term, const char *field) {
  cbor_item_t *payload = wd_map(2);
  wd_put(payload, "terminal", wd_text(term->id));
  wd_put(payload, field, cbor_build_bool(true));
  conn_broadcast(srv, term->session, "terminal.output", 0, payload);
}

static void term_pump(wd_server *srv, wd_term *term) {
  char buf[4096];
  ssize_t n = wd_pty_read(term->pty, buf, sizeof(buf));
  if (n > 0) {
    cbor_item_t *payload = wd_map(2);
    wd_put(payload, "terminal", wd_text(term->id));
    wd_put(payload, "data", wd_text_len(buf, (size_t)n));
    conn_broadcast(srv, term->session, "terminal.output", 0, payload);
    return;
  }
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    term_announce(srv, term, "eof");
    term_drop(srv, term);
    return;
  }
  if (n < 0 && !wd_pty_alive(term->pty)) {
    term_announce(srv, term, "eof");
    term_drop(srv, term);
  }
}

static void handle_terminal_open(wd_server *srv, wd_conn *conn, uint64_t id,
                                 cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *command = wd_text_copy(wd_get(params, "command"));
  uint64_t rows = 24;
  uint64_t cols = 80;
  wd_uint_of(wd_get(params, "rows"), &rows);
  wd_uint_of(wd_get(params, "cols"), &cols);
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL) {
    free(session_id);
    free(command);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    return;
  }
  size_t slot = WD_MAX_TERMS;
  for (size_t i = 0; i < WD_MAX_TERMS; i++) {
    if (!terminals[i].used) {
      slot = i;
      break;
    }
  }
  if (slot == WD_MAX_TERMS) {
    free(session_id);
    free(command);
    conn_send_error(conn, id, WD_ERR_BUSY, "too many terminals");
    return;
  }
  const char *argv[4];
  size_t argc = 0;
  const char *shell = command != NULL && command[0] != '\0' ? command : "sh";
  argv[argc++] = shell;
  argv[argc++] = "-i";
  argv[argc] = NULL;
  char *err = NULL;
  wd_pty *pty = wd_pty_open(argv, session->cwd, (uint16_t)(rows == 0 ? 24 : rows),
                            (uint16_t)(cols == 0 ? 80 : cols), &err);
  free(command);
  free(session_id);
  if (pty == NULL) {
    char message[160];
    snprintf(message, sizeof(message), "%s",
             err != NULL ? err : "cannot open a terminal");
    free(err);
    conn_send_error(conn, id, WD_ERR_INTERNAL, message);
    return;
  }
  wd_term *term = &terminals[slot];
  memset(term, 0, sizeof(*term));
  (void)wd_new_id(term->id);
  term->session = session;
  term->pty = pty;
  term->used = true;
  term_tokens[slot].term = term;
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = &term_tokens[slot];
  if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, wd_pty_fd(pty), &ev) != 0) {
    term_drop(srv, term);
    conn_send_error(conn, id, WD_ERR_INTERNAL, "cannot watch the terminal");
    return;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "terminal", wd_text(term->id));
  wd_put(result, "pid", wd_uint((uint64_t)wd_pty_pid(pty)));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_terminal_write(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *handle = wd_text_copy(wd_get(params, "terminal"));
  char *data = wd_text_copy(wd_get(params, "data"));
  wd_term *term = term_find(handle);
  bool written = false;
  if (term != NULL && data != NULL) {
    ssize_t n = wd_pty_write(term->pty, data, strlen(data));
    written = n == (ssize_t)strlen(data);
  }
  free(handle);
  free(data);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "written", cbor_build_bool(written));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_terminal_resize(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *handle = wd_text_copy(wd_get(params, "terminal"));
  uint64_t rows = 24;
  uint64_t cols = 80;
  wd_uint_of(wd_get(params, "rows"), &rows);
  wd_uint_of(wd_get(params, "cols"), &cols);
  wd_term *term = term_find(handle);
  bool resized =
      term != NULL &&
      wd_pty_resize(term->pty, (uint16_t)(rows == 0 ? 24 : rows),
                    (uint16_t)(cols == 0 ? 80 : cols)) == 0;
  free(handle);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "resized", cbor_build_bool(resized));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_terminal_close(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *handle = wd_text_copy(wd_get(params, "terminal"));
  wd_term *term = term_find(handle);
  bool closed = term != NULL;
  term_drop(srv, term);
  free(handle);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "closed", cbor_build_bool(closed));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void attachment_context(wd_session *session, char *buf, size_t cap);

static void handle_prompt_preview(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL) {
    free(session_id);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    return;
  }
  char attachments[512];
  attachment_context(session, attachments, sizeof(attachments));
  wd_prompt *prompt = wd_prompt_build(srv->state_dir, session->cwd,
                                      attachments[0] != '\0' ? attachments : NULL,
                                      session->system);
  cbor_item_t *result = wd_map(2);
  wd_put(result, "session", wd_text(session->id));
  wd_put(result, "sections", wd_prompt_to_cbor(prompt));
  wd_prompt_free(prompt);
  free(session_id);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_upgrade(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *module = wd_text_copy(wd_get(params, "module"));
  char *version = wd_text_copy(wd_get(params, "version"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL || module == NULL || version == NULL) {
    free(session_id);
    free(module);
    free(version);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "a session, a module and a version are needed");
    return;
  }
  if (run_find(srv, session_id) != NULL) {
    free(session_id);
    free(module);
    free(version);
    conn_send_error(conn, id, WD_ERR_BUSY, "the session is already busy");
    return;
  }
  if (wd_registry_resolve(&srv->registry, module, version) == NULL) {
    free(session_id);
    free(module);
    free(version);
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such module version");
    return;
  }
  uint64_t seq = 0;
  int rc = wd_session_upgrade(session, module, version, &seq);
  free(session_id);
  free(module);
  free(version);
  if (rc != 0) {
    conn_send_error(conn, id, WD_ERR_INTERNAL, "cannot upgrade the session");
    return;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "session", wd_session_to_cbor(session));
  wd_put(result, "upgraded", cbor_build_bool(seq != 0));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_module_list(wd_server *srv, wd_conn *conn, uint64_t id) {
  cbor_item_t *items = wd_array(srv->registry.len);
  for (size_t i = 0; i < srv->registry.len; i++) {
    const wd_module_info *info = &srv->registry.items[i];
    cbor_item_t *entry = wd_map(6);
    wd_put(entry, "name", wd_text(info->name));
    wd_put(entry, "version", wd_text(info->version));
    wd_put(entry, "size", wd_uint((uint64_t)info->size));
    wd_put(entry, "hash", wd_uint(info->hash));
    wd_put(entry, "changed", cbor_build_bool(info->changed));
    wd_put(entry, "valid", cbor_build_bool(info->valid));
    bool pushed = cbor_array_push(items, entry);
    cbor_decref(&entry);
    if (!pushed) break;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "modules", items);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_module_reload(wd_server *srv, wd_conn *conn, uint64_t id) {
  char **updated = NULL;
  size_t updated_len = 0;
  cbor_item_t **refused = NULL;
  size_t refused_len = 0;
  for (size_t i = 0; i < srv->registry.len; i++)
    srv->registry.items[i].changed = false;
  (void)wd_registry_scan(&srv->registry, srv->module_dir);
  for (size_t i = 0; i < srv->registry.len; i++) {
    wd_module_info *info = &srv->registry.items[i];
    if (!info->changed) continue;
    char *err = NULL;
    if (wd_registry_validate(srv->module_dir, info->name, info->version, &err) ==
        0) {
      char **grown = realloc(updated, (updated_len + 1) * sizeof(*grown));
      if (grown == NULL) break;
      updated = grown;
      updated[updated_len] = strdup(info->name);
      if (updated[updated_len] != NULL) updated_len++;
      info->changed = false;
      free(err);
    } else {
      cbor_item_t **grown = realloc(refused, (refused_len + 1) * sizeof(*grown));
      if (grown == NULL) break;
      refused = grown;
      cbor_item_t *entry = wd_map(3);
      wd_put(entry, "name", wd_text(info->name));
      wd_put(entry, "version", wd_text(info->version));
      wd_put(entry, "error", wd_text(err != NULL ? err : "cannot load"));
      refused[refused_len] = entry;
      refused_len++;
      info->valid = false;
      snprintf(info->error, sizeof(info->error), "%s",
               err != NULL ? err : "cannot load");
      free(err);
    }
  }
  cbor_item_t *updated_items = wd_array(updated_len);
  for (size_t i = 0; i < updated_len; i++) {
    cbor_item_t *name = wd_text(updated[i]);
    bool pushed = cbor_array_push(updated_items, name);
    cbor_decref(&name);
    if (!pushed) break;
    free(updated[i]);
  }
  free(updated);
  cbor_item_t *refused_items = wd_array(refused_len);
  for (size_t i = 0; i < refused_len; i++) {
    bool pushed = cbor_array_push(refused_items, refused[i]);
    cbor_decref(&refused[i]);
    if (!pushed) break;
  }
  free(refused);
  cbor_item_t *result = wd_map(2);
  wd_put(result, "updated", updated_items);
  wd_put(result, "refused", refused_items);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_attachment_add(wd_server *srv, wd_conn *conn, uint64_t id,
                                 cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  char *name = wd_text_copy(wd_get(params, "name"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  if (session == NULL || name == NULL) {
    free(session_id);
    free(name);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "a session and a name are needed");
    return;
  }
  cbor_item_t *item = wd_get(params, "data");
  const uint8_t *bytes = NULL;
  size_t len = 0;
  char *text = NULL;
  if (item != NULL && item != NULL && cbor_typeof(item) == CBOR_TYPE_BYTESTRING) {
    bytes = cbor_bytestring_handle(item);
    len = cbor_bytestring_length(item);
  } else {
    text = wd_text_copy(item);
    if (text != NULL) {
      bytes = (const uint8_t *)text;
      len = strlen(text);
    }
  }
  if (bytes == NULL) {
    free(session_id);
    free(name);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "no attachment data");
    return;
  }
  if (len > (4u << 20)) {
    free(session_id);
    free(name);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS, "the attachment is too large");
    return;
  }
  char *path = NULL;
  char *err = NULL;
  int rc = wd_session_attach(session, name, bytes, len, &path, &err);
  free(session_id);
  free(name);
  free(text);
  if (rc != 0) {
    char message[160];
    snprintf(message, sizeof(message), "%s",
             err != NULL ? err : "cannot store the attachment");
    free(err);
    conn_send_error(conn, id, WD_ERR_INTERNAL, message);
    return;
  }
  cbor_item_t *result = wd_map(2);
  wd_put(result, "path", wd_text(path));
  wd_put(result, "size", wd_uint((uint64_t)len));
  free(path);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

typedef struct {
  cbor_item_t **items;
  size_t len;
  size_t cap;
} attachment_list;

static int collect_attachment(void *ctx, uint64_t seq, cbor_item_t *event) {
  (void)seq;
  attachment_list *list = ctx;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind == NULL) return 0;
  if (strcmp(kind, "session.attachment") != 0) {
    free(kind);
    return 0;
  }
  free(kind);
  cbor_item_t *payload = wd_get(event, "payload");
  char *name = wd_text_copy(wd_get(payload, "name"));
  uint64_t size = 0;
  wd_uint_of(wd_get(payload, "size"), &size);
  if (name == NULL) return 0;
  cbor_item_t *entry = wd_map(2);
  wd_put(entry, "name", wd_text(name));
  wd_put(entry, "size", wd_uint(size));
  free(name);
  if (list->len == list->cap) {
    size_t cap = list->cap == 0 ? 8 : list->cap * 2;
    cbor_item_t **grown = realloc(list->items, cap * sizeof(*grown));
    if (grown == NULL) {
      cbor_decref(&entry);
      return 0;
    }
    list->items = grown;
    list->cap = cap;
  }
  list->items[list->len++] = entry;
  return 0;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} attachment_note;

static int note_attachment(void *ctx, uint64_t seq, cbor_item_t *event) {
  (void)seq;
  attachment_note *note = ctx;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind == NULL) return 0;
  if (strcmp(kind, "session.attachment") != 0) {
    free(kind);
    return 0;
  }
  free(kind);
  cbor_item_t *payload = wd_get(event, "payload");
  char *name = wd_text_copy(wd_get(payload, "name"));
  uint64_t size = 0;
  wd_uint_of(wd_get(payload, "size"), &size);
  if (name == NULL) return 0;
  if (note->len + strlen(name) + 32 < note->cap)
    note->len += (size_t)snprintf(note->buf + note->len, note->cap - note->len,
                                  "%s%s (%llu bytes)", note->len > 0 ? ", " : "",
                                  name, (unsigned long long)size);
  free(name);
  return 0;
}

static void attachment_context(wd_session *session, char *buf, size_t cap) {
  attachment_note note = {buf, 0, cap};
  buf[0] = '\0';
  (void)wd_session_replay(session, 1, note_attachment, &note);
  if (note.len > 0) {
    char line[1024];
    snprintf(line, sizeof(line), "attached to this session: %s", buf);
    snprintf(buf, cap, "%s", line);
  }
}

static void handle_attachment_list(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL) {
    conn_send_error(conn, id, WD_ERR_NOT_FOUND, "no such session");
    return;
  }
  attachment_list list = {NULL, 0, 0};
  (void)wd_session_replay(session, 1, collect_attachment, &list);
  cbor_item_t *items = wd_array(list.len);
  for (size_t i = 0; i < list.len; i++) {
    bool pushed = cbor_array_push(items, list.items[i]);
    if (!pushed) {
      cbor_decref(&list.items[i]);
      continue;
    }
    cbor_decref(&list.items[i]);
  }
  free(list.items);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "attachments", items);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_session_cancel(wd_server *srv, wd_conn *conn, uint64_t id,
                                  cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "id"));
  bool cancelled = false;
  if (session_id != NULL) {
    wd_run *run = run_find(srv, session_id);
    wd_session *session = wd_session_find(&srv->store, session_id);
    if (run != NULL && session != NULL) {
      wd_turn_ctx tc = {srv, session};
      char *err = NULL;
      cancelled =
          wd_turn_abort(run->turn, "cancelled", turn_emit, &tc, &err) == 0;
      free(err);
      if (cancelled) run_free(srv, run);
    }
  }
  free(session_id);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "cancelled", cbor_build_bool(cancelled));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void sched_emit(wd_server *srv, wd_session *session, const char *id,
                       const char *state, uint64_t next_ms) {
  cbor_item_t *record = wd_map(3);
  wd_put(record, "schedule", wd_text(id));
  wd_put(record, "state", wd_text(state));
  wd_put(record, "next_ms", wd_uint(next_ms));
  uint64_t seq = 0;
  if (wd_session_emit(session, "schedule.updated", record, &seq) != 0) return;
  cbor_item_t *event = wd_map(3);
  wd_put(event, "schedule", wd_text(id));
  wd_put(event, "state", wd_text(state));
  wd_put(event, "next_ms", wd_uint(next_ms));
  conn_broadcast(srv, session, "schedule.updated", seq, event);
}

static void handle_schedule_set(wd_server *srv, wd_conn *conn, uint64_t id,
                                cbor_item_t *params) {
  char *session_id = wd_text_copy(wd_get(params, "session"));
  char *cron = wd_text_copy(wd_get(params, "cron"));
  char *text = wd_text_copy(wd_get(params, "text"));
  wd_session *session =
      session_id != NULL ? wd_session_find(&srv->store, session_id) : NULL;
  free(session_id);
  if (session == NULL || cron == NULL || text == NULL || text[0] == '\0') {
    free(cron);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "session, cron and text are required");
    conn_arm(srv, conn);
    return;
  }
  if (srv->scheduler == NULL) {
    free(cron);
    free(text);
    conn_send_error(conn, id, WD_ERR_NOT_SUPPORTED, "schedules are disabled");
    conn_arm(srv, conn);
    return;
  }
  uint64_t next_ms = 0;
  char *err = NULL;
  if (wd_scheduler_next(srv->scheduler, cron, wd_now_ms(), &next_ms, &err) != 0 ||
      next_ms == 0) {
    free(err);
    free(cron);
    free(text);
    conn_send_error(conn, id, WD_ERR_INVALID_PARAMS,
                    "the cron expression does not parse");
    conn_arm(srv, conn);
    return;
  }
  free(err);
  wd_sched *entry = calloc(1, sizeof(*entry));
  if (entry == NULL) {
    free(cron);
    free(text);
    respond_internal(srv, conn, id, "out of memory");
    return;
  }
  char generated[WD_ID_LEN];
  wd_new_id(generated);
  snprintf(entry->id, sizeof(entry->id), "%.8s", generated);
  snprintf(entry->session, sizeof(entry->session), "%s", session->id);
  entry->cron = cron;
  entry->text = text;
  entry->next_ms = next_ms;
  entry->next = srv->schedules;
  srv->schedules = entry;
  sched_emit(srv, session, entry->id, "set", next_ms);
  cbor_item_t *result = wd_map(2);
  wd_put(result, "schedule", wd_text(entry->id));
  wd_put(result, "next_ms", wd_uint(next_ms));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_schedule_list(wd_server *srv, wd_conn *conn, uint64_t id) {
  size_t count = 0;
  for (wd_sched *entry = srv->schedules; entry != NULL; entry = entry->next)
    count++;
  cbor_item_t *list = wd_array(count);
  for (wd_sched *entry = srv->schedules; entry != NULL; entry = entry->next) {
    cbor_item_t *item = wd_map(4);
    wd_put(item, "schedule", wd_text(entry->id));
    wd_put(item, "session", wd_text(entry->session));
    wd_put(item, "cron", wd_text(entry->cron));
    wd_put(item, "next_ms", wd_uint(entry->next_ms));
    bool pushed = cbor_array_push(list, item);
    cbor_decref(&item);
    if (!pushed) break;
  }
  cbor_item_t *result = wd_map(1);
  wd_put(result, "schedules", list);
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
}

static void handle_schedule_delete(wd_server *srv, wd_conn *conn, uint64_t id,
                                   cbor_item_t *params) {
  char *want = wd_text_copy(wd_get(params, "schedule"));
  bool removed = false;
  for (wd_sched **at = &srv->schedules; *at != NULL; at = &(*at)->next) {
    if (want != NULL && strcmp((*at)->id, want) == 0) {
      wd_sched *entry = *at;
      *at = entry->next;
      wd_session *session = wd_session_find(&srv->store, entry->session);
      if (session != NULL) sched_emit(srv, session, entry->id, "deleted", 0);
      free(entry->cron);
      free(entry->text);
      free(entry);
      removed = true;
      break;
    }
  }
  free(want);
  cbor_item_t *result = wd_map(1);
  wd_put(result, "deleted", cbor_build_bool(removed));
  conn_send_cached(conn, id, wd_response_ok(id, result));
  conn_arm(srv, conn);
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
  run->devlock.fd = -1;
  run->http_body = NULL;
  run->http_len = 0;
  run->http_cap = 0;
  run->sse = NULL;
  run->sse_ctx = NULL;
  run->http_status_seen = 0;
  run->streamed = 0;
  run->devlock.fd = -1;
  snprintf(run->session_id, sizeof(run->session_id), "%s", session->id);
  run->turn = wd_turn_new_pinned(srv->engine, dir, session->cwd, session->pins,
                                session->pins_len, err);
  if (run->turn == NULL) {
    free(run);
    return NULL;
  }
  if (device_acquire(srv, session, &run->devlock) != 0) {
    wd_turn_free(run->turn);
    free(run->sse_ctx);
    if (run->sse != NULL) wd_sse_destroy(run->sse);
    free(run);
    if (err != NULL) *err = strdup("the device has no free slot");
    return NULL;
  }
  if (session->system != NULL)
    wd_turn_set_session_system(run->turn, session->system);
  if (session->head == NULL || session->head[0] == '\0') {
    char attach_buf[512];
    attachment_context(session, attach_buf, sizeof(attach_buf));
    wd_prompt *prompt = wd_prompt_build(
        srv->state_dir, session->cwd,
        attach_buf[0] != '\0' ? attach_buf : NULL, session->system);
    char *rendered = wd_prompt_render(prompt);
    wd_prompt_free(prompt);
    if (rendered != NULL) {
      uint64_t head_seq = 0;
      if (wd_session_set_head(session, rendered, &head_seq) == 0) {
        cbor_item_t *head_event = wd_map(1);
        wd_put(head_event, "text", wd_text(rendered));
        conn_broadcast(srv, session, "session.head", head_seq, head_event);
      }
      free(rendered);
    }
  }
  if (session->head != NULL) wd_turn_set_head(run->turn, session->head);
  const wd_model *chosen = wd_models_find(srv->models, session->model);
  if (chosen != NULL) {
    if (chosen->key != NULL)
      wd_turn_set_api_key(run->turn,
                          wd_credentials_find(srv->credentials, chosen->key));
    if (chosen->model != NULL) wd_turn_set_model_name(run->turn, chosen->model);
    if (chosen->endpoint != NULL) {
      wd_turn_set_endpoint(run->turn, chosen->endpoint);
      wd_turn_allow_remote(run->turn);
    }
  }
  if (apply_state(srv, session, run) != 0) {
    wd_turn_free(run->turn);
    run->turn = wd_turn_new_pinned(srv->engine, dir, session->cwd,
                                   session->pins, session->pins_len, err);
    if (run->turn == NULL) {
      free(run);
      return NULL;
    }
    if (session->system != NULL)
      wd_turn_set_session_system(run->turn, session->system);
    if (session->head != NULL) wd_turn_set_head(run->turn, session->head);
    if (chosen != NULL) {
      if (chosen->key != NULL)
        wd_turn_set_api_key(run->turn,
                            wd_credentials_find(srv->credentials, chosen->key));
      if (chosen->model != NULL)
        wd_turn_set_model_name(run->turn, chosen->model);
      if (chosen->endpoint != NULL) {
        wd_turn_set_endpoint(run->turn, chosen->endpoint);
        wd_turn_allow_remote(run->turn);
      }
    }
    (void)apply_state(srv, session, run);
  }
  wd_turn_set_root(run->turn, session->cwd);
  char attach_buf[768];
  attachment_context(session, attach_buf, sizeof(attach_buf));
  wd_turn_set_context(run->turn, attach_buf);
  run->sse_ctx = calloc(1, sizeof(stream_ctx));
  if (run->sse_ctx != NULL) {
    stream_ctx *sc = run->sse_ctx;
    sc->srv = srv;
    sc->run = run;
    run->sse = wd_sse_create(stream_event, sc);
  }
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

static void emit_state(wd_server *srv, wd_run *run, const char *state);

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

static uint32_t module_schema(wd_server *srv, const char *name) {
  wd_manifest manifest;
  char *err = NULL;
  uint32_t schema = 1;
  if (wd_manifest_load(srv->module_dir, name, &manifest, &err) == 0)
    schema = manifest.state_schema;
  free(err);
  return schema;
}

static int apply_state(wd_server *srv, wd_session *session, wd_run *run) {
  if (srv->state_dir == NULL || run->turn == NULL) return 0;
  static const char *const names[] = {"engine",  "driver", "provider", "tools",
                                      "policy", "skills", "judge"};
  int failed = 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    char stored[32];
    if (wd_module_state_find(srv->state_dir, session->id, names[i], stored,
                             sizeof(stored)) != 0)
      continue;
    const char *pinned = "1";
    for (size_t p = 0; p < session->pins_len; p++)
      if (strcmp(session->pins[p].name, names[i]) == 0)
        pinned = session->pins[p].version;
    if (strcmp(stored, pinned) == 0) continue;
    char *path = wd_module_state_path(srv->state_dir, session->id, names[i],
                                      stored);
    uint8_t *blob = NULL;
    size_t len = 0;
    if (path == NULL || wd_module_state_read(path, &blob, &len) != 0) {
      free(path);
      free(blob);
      continue;
    }
    uint32_t schema = module_schema(srv, names[i]);
    char *err = NULL;
    if (wd_turn_state_import(run->turn, names[i], blob, len, schema, schema,
                             &err) == 0) {
      remove(path);
    } else {
      uint64_t seq = 0;
      wd_session_note_migration(session, names[i], stored, pinned,
                                err != NULL ? err : "refused", &seq);
      failed = 1;
    }
    free(err);
    free(blob);
    free(path);
  }
  return failed ? -1 : 0;
}

static void persist_state(wd_server *srv, wd_run *run) {
  if (run->turn == NULL || srv->state_dir == NULL) return;
  wd_session *session = wd_session_find(&srv->store, run->session_id);
  if (session == NULL) return;
  static const char *const names[] = {"engine",  "driver", "provider", "tools",
                                      "policy", "skills", "judge"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    const char *version = "1";
    for (size_t p = 0; p < session->pins_len; p++)
      if (strcmp(session->pins[p].name, names[i]) == 0)
        version = session->pins[p].version;
    uint8_t *blob = NULL;
    size_t len = 0;
    char *err = NULL;
    if (wd_turn_state_export(run->turn, names[i], &blob, &len, &err) != 0) {
      free(err);
      continue;
    }
    char *path = wd_module_state_path(srv->state_dir, run->session_id,
                                      names[i], version);
    if (path != NULL) {
      if (wd_module_state_write(path, blob, len) == 0)
        wd_module_state_forget(srv->state_dir, run->session_id, names[i],
                               version);
      free(path);
    }
    free(blob);
    free(err);
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
  char trapped[64];
  if (run->turn != NULL &&
      wd_turn_trapped(run->turn, trapped, sizeof(trapped))) {
    wd_session *session = wd_session_find(&srv->store, run->session_id);
    const char *version = "1";
    if (session != NULL)
      for (size_t p = 0; p < session->pins_len; p++)
        if (strcmp(session->pins[p].name, trapped) == 0)
          version = session->pins[p].version;
    wd_registry_trap(&srv->registry, trapped, version);
  }
  {
    wd_session *owner = wd_session_find(&srv->store, run->session_id);
    if (owner != NULL) device_release(srv, owner, &run->devlock);
  }
  persist_state(srv, run);
  wd_turn_free(run->turn);
  free(run->http_body);
  if (run->sse != NULL) wd_sse_destroy(run->sse);
  free(run->sse_ctx);
  free(run);
}

static void emit_state(wd_server *srv, wd_run *run, const char *state) {
  wd_session *session = wd_session_find(&srv->store, run->session_id);
  if (session == NULL) return;
  cbor_item_t *logged = wd_map(2);
  wd_put(logged, "turn", wd_uint(wd_turn_id(run->turn)));
  wd_put(logged, "state", wd_text(state));
  uint64_t seq = 0;
  if (wd_session_emit(session, "turn.state", logged, &seq) != 0) return;
  cbor_item_t *out = wd_map(2);
  wd_put(out, "turn", wd_uint(wd_turn_id(run->turn)));
  wd_put(out, "state", wd_text(state));
  conn_broadcast(srv, session, "turn.state", seq, out);
}

static void run_park(wd_server *srv, wd_run *run, int kind) {
  if (kind == 3) {
    emit_state(srv, run, "prefilling");
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
  drain_queue(srv);
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
    conn_send_error(conn, id, WD_ERR_BUSY, "the session is already busy");
    conn_arm(srv, conn);
    return;
  }
  if (!device_free(srv, session)) {
    if (!enqueue_turn(srv, session, text, NULL)) {
      free(session_id);
      free(text);
      conn_send_error(conn, id, WD_ERR_BUSY, "the session is already queued");
      conn_arm(srv, conn);
      return;
    }
    cbor_item_t *queued = wd_map(2);
    wd_put(queued, "session", wd_session_to_cbor(session));
    wd_put(queued, "queued", cbor_build_bool(true));
    free(session_id);
    free(text);
    conn_send_cached(conn, id, wd_response_ok(id, queued));
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
  size_t budget = history_budget(srv, session);
  if (wd_session_history(session, budget, &history) == 0) {
    wd_turn_set_history(run->turn, history);
    size_t bytes = 0;
    for (size_t i = 0; i < cbor_array_size(history); i++) {
      cbor_item_t *entry = cbor_array_get(history, i);
      if (entry == NULL) continue;
      char *text = wd_text_copy(wd_get(entry, "text"));
      if (text != NULL) {
        bytes += strlen(text);
        free(text);
      }
      cbor_decref(&entry);
    }
    if (bytes > budget) {
      cbor_item_t *over_payload = wd_map(2);
      wd_put(over_payload, "bytes", wd_uint((uint64_t)bytes));
      wd_put(over_payload, "budget", wd_uint((uint64_t)budget));
      cbor_item_t *over_event = wd_map(2);
      wd_put(over_event, "bytes", wd_uint((uint64_t)bytes));
      wd_put(over_event, "budget", wd_uint((uint64_t)budget));
      uint64_t over_seq = 0;
      if (wd_session_emit(session, "session.overflow", over_payload, &over_seq) ==
          0)
        conn_broadcast(srv, session, "session.overflow", over_seq, over_event);
    }
  }

  cbor_item_t *asked = wd_map(1);
  wd_put(asked, "text", wd_text(text));
  uint64_t asked_seq = 0;
  char when[64];
  wd_now_text(when, sizeof(when));
  cbor_item_t *time_event = wd_map(1);
  wd_put(time_event, "text", wd_text(when));
  cbor_item_t *time_payload = wd_map(1);
  wd_put(time_payload, "text", wd_text(when));
  uint64_t time_seq = 0;
  if (wd_session_emit(session, "session.time", time_payload, &time_seq) == 0)
    conn_broadcast(srv, session, "session.time", time_seq, time_event);
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
  conn_arm(srv, conn);
}

static void drain_schedules(wd_server *srv, uint64_t now) {
  for (wd_sched *entry = srv->schedules; entry != NULL; entry = entry->next) {
    if (entry->next_ms == 0 || entry->next_ms > now) continue;
    wd_session *session = wd_session_find(&srv->store, entry->session);
    if (session != NULL) {
      wd_session *child = NULL;
      if (wd_session_fork(&srv->store, session, session->last_seq, &child) == 0) {
        char *err = NULL;
        wd_run *run = run_new(srv, child, &err);
        if (run != NULL) {
          cbor_item_t *history = NULL;
          if (wd_session_history(child, history_budget(srv, child), &history) == 0)
            wd_turn_set_history(run->turn, history);
          cbor_item_t *asked = wd_map(1);
          wd_put(asked, "text", wd_text(entry->text));
          uint64_t seq = 0;
          char when[64];
          wd_now_text(when, sizeof(when));
          cbor_item_t *time_event = wd_map(1);
          wd_put(time_event, "text", wd_text(when));
          cbor_item_t *time_payload = wd_map(1);
          wd_put(time_payload, "text", wd_text(when));
          uint64_t time_seq = 0;
          if (wd_session_emit(child, "session.time", time_payload, &time_seq) == 0)
            conn_broadcast(srv, child, "session.time", time_seq, time_event);
          if (wd_session_emit(child, "session.prompted", asked, &seq) == 0) {
            cbor_item_t *event = wd_map(1);
            wd_put(event, "text", wd_text(entry->text));
            conn_broadcast(srv, child, "session.prompted", seq, event);
          }
          wd_turn_ctx tc = {srv, child};
          int rc = wd_turn_prompt(run->turn, entry->text, turn_emit, &tc, &err);
          run_after(srv, run, rc, NULL);
        }
        free(err);
        cbor_item_t *notice = wd_map(2);
        wd_put(notice, "schedule", wd_text(entry->id));
        wd_put(notice, "session", wd_text(child->id));
        uint64_t seq = 0;
        if (wd_session_emit(session, "schedule.fired", notice, &seq) == 0) {
          cbor_item_t *event = wd_map(2);
          wd_put(event, "schedule", wd_text(entry->id));
          wd_put(event, "session", wd_text(child->id));
          conn_broadcast(srv, session, "schedule.fired", seq, event);
        }
      }
    }
    uint64_t next = 0;
    char *err = NULL;
    if (srv->scheduler != NULL &&
        wd_scheduler_next(srv->scheduler, entry->cron, now, &next, &err) == 0 &&
        next > 0)
      entry->next_ms = next;
    else
      entry->next_ms = 0;
    free(err);
    if (session != NULL)
      sched_emit(srv, session, entry->id, "fired", entry->next_ms);
  }
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

static int stream_event(void *ctx, const wd_sse_event *event) {
  stream_ctx *sc = ctx;
  if (getenv("WARD_DEBUG") != NULL)
    fprintf(stderr, "ward: sse event %zu bytes\n", event->data_len);
  if (event->data == NULL || event->data_len < 12) return 0;
  const char *at = NULL;
  for (size_t i = 0; i + 11 < event->data_len; i++) {
    if (memcmp(event->data + i, "\"content\":\"", 11) == 0) {
      at = event->data + i + 11;
      break;
    }
  }
  if (at == NULL) return 0;
  size_t room = (size_t)(event->data + event->data_len - at);
  char text[2048];
  size_t len = 0;
  for (size_t i = 0; i < room && len + 1 < sizeof(text); i++) {
    char c = at[i];
    if (c == '\\' && i + 1 < room) {
      char next = at[i + 1];
      text[len++] = next == 'n' ? '\n' : next;
      i++;
      continue;
    }
    if (c == '"') break;
    text[len++] = c;
  }
  if (len == 0) return 0;
  wd_session *session = wd_session_find(&sc->srv->store, sc->run->session_id);
  if (session == NULL) return 0;
  cbor_item_t *logged = wd_map(1);
  wd_put(logged, "text", wd_text_len(text, len));
  uint64_t seq = 0;
  if (wd_session_emit(session, "text.delta", logged, &seq) != 0) return 0;
  cbor_item_t *out = wd_map(1);
  wd_put(out, "text", wd_text_len(text, len));
  conn_broadcast(sc->srv, session, "text.delta", seq, out);
  sc->run->streamed++;
  return 0;
}

static int read_stream(wd_server *srv, wd_run *run, bool *eof) {
  (void)srv;
  *eof = false;
  for (;;) {
    if (run->http_len + 65536 > run->http_cap) {
      size_t grown = run->http_cap == 0 ? 131072 : run->http_cap * 2;
      uint8_t *next = realloc(run->http_body, grown);
      if (next == NULL) return -1;
      run->http_body = next;
      run->http_cap = grown;
    }
    ssize_t got = read(run->http_fd, run->http_body + run->http_len,
                       run->http_cap - run->http_len);
    if (got < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      return -1;
    }
    if (got == 0) {
      *eof = true;
      return 0;
    }
    size_t skip = 0;
    if (!run->http_status_seen) {
      if (run->http_len + (size_t)got < 4) {
        run->http_len += (size_t)got;
        return 0;
      }
      skip = 4 - run->http_len;
      run->http_status_seen = 1;
    }
    if (run->sse != NULL && (size_t)got > skip)
      (void)wd_sse_feed(run->sse,
                        (const char *)run->http_body + run->http_len + skip,
                        (size_t)got - skip);
    run->http_len += (size_t)got;
    return 0;
  }
}

static void http_done(wd_server *srv, wd_run *run) {
  wd_session *session = wd_session_find(&srv->store, run->session_id);
  if (session == NULL) {
    run_free(srv, run);
    return;
  }
  bool eof = false;
  if (read_stream(srv, run, &eof) != 0) {
    run_free(srv, run);
    return;
  }
  if (!eof) return;
  wd_turn_ctx tc = {srv, session};
  char *err = NULL;
  emit_state(srv, run, "answering");
  long status = 0;
  if (run->http_len >= 4) {
    status = (long)((uint32_t)run->http_body[0] |
                    ((uint32_t)run->http_body[1] << 8) |
                    ((uint32_t)run->http_body[2] << 16) |
                    ((uint32_t)run->http_body[3] << 24));
  }
  size_t had = run->http_len;
  int rc = wd_turn_resume_http_body(
      run->turn, status,
      run->http_len > 4 ? (const char *)run->http_body + 4 : "", 
      run->http_len > 4 ? run->http_len - 4 : 0, turn_emit, &tc, &err);
  if (rc == WD_TURN_PENDING) {
    run->http_len = 0;
    run->http_status_seen = 0;
    if (run->sse != NULL) {
      wd_sse_destroy(run->sse);
      stream_ctx *sc = run->sse_ctx;
      run->sse = wd_sse_create(stream_event, sc);
    }
  } else {
    emit_state(srv, run, "idle");
  }
  (void)had;
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
  } else if (strcmp(method, "goal.set") == 0) {
    handle_goal_set(srv, conn, id, params);
  } else if (strcmp(method, "goal.clear") == 0) {
    handle_goal_clear(srv, conn, id, params);
  } else if (strcmp(method, "schedule.set") == 0) {
    handle_schedule_set(srv, conn, id, params);
  } else if (strcmp(method, "schedule.list") == 0) {
    handle_schedule_list(srv, conn, id);
  } else if (strcmp(method, "schedule.delete") == 0) {
    handle_schedule_delete(srv, conn, id, params);
  } else if (strcmp(method, "job.list") == 0) {
    handle_job_list(srv, conn, id);
  } else if (strcmp(method, "job.kill") == 0) {
    handle_job_kill(srv, conn, id, params);
  } else if (strcmp(method, "subagent.prompt") == 0) {
    handle_subagent_prompt(srv, conn, id, params);
  } else if (strcmp(method, "subagent.interrupt") == 0) {
    handle_subagent_interrupt(srv, conn, id, params);
  } else if (strcmp(method, "session.cancel") == 0) {
    handle_session_cancel(srv, conn, id, params);
  } else if (strcmp(method, "prompt.preview") == 0) {
    handle_prompt_preview(srv, conn, id, params);
  } else if (strcmp(method, "session.upgrade") == 0) {
    handle_session_upgrade(srv, conn, id, params);
  } else if (strcmp(method, "module.list") == 0) {
    handle_module_list(srv, conn, id);
  } else if (strcmp(method, "module.reload") == 0) {
    handle_module_reload(srv, conn, id);
  } else if (strcmp(method, "attachment.add") == 0) {
    handle_attachment_add(srv, conn, id, params);
  } else if (strcmp(method, "attachment.list") == 0) {
    handle_attachment_list(srv, conn, id, params);
  } else if (strcmp(method, "terminal.open") == 0) {
    handle_terminal_open(srv, conn, id, params);
  } else if (strcmp(method, "terminal.write") == 0) {
    handle_terminal_write(srv, conn, id, params);
  } else if (strcmp(method, "terminal.resize") == 0) {
    handle_terminal_resize(srv, conn, id, params);
  } else if (strcmp(method, "terminal.close") == 0) {
    handle_terminal_close(srv, conn, id, params);
  } else if (strcmp(method, "session.fork") == 0) {
    handle_session_fork(srv, conn, id, params);
  } else if (strcmp(method, "session.set_model") == 0) {
    handle_session_set_model(srv, conn, id, params);
  } else if (strcmp(method, "session.set_system") == 0) {
    handle_session_set_system(srv, conn, id, params);
  } else if (strcmp(method, "model.probe") == 0) {
    handle_model_probe(srv, conn, id, params);
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

static void server_accept(wd_server *srv, int listener) {
  for (;;) {
    int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;
    }
    struct ucred cred;
    socklen_t len = sizeof(cred);
    bool ok = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
              cred.uid == geteuid();
    if (!ok && listener == srv->tcp_fd) {
      struct sockaddr_storage peer;
      socklen_t peer_len = sizeof(peer);
      if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) == 0 &&
          peer.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&peer;
        ok = ntohl(v4->sin_addr.s_addr) == 0x7f000001u;
      }
    }
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

static int setup_tcp_listener(const char *bind_text, int *port_out) {
  const char *colon = strrchr(bind_text, ':');
  if (colon == NULL) {
    errno = EINVAL;
    return -1;
  }
  char host[64];
  size_t host_len = (size_t)(colon - bind_text);
  if (host_len == 0 || host_len >= sizeof(host)) {
    errno = EINVAL;
    return -1;
  }
  memcpy(host, bind_text, host_len);
  host[host_len] = '\0';
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)atoi(colon + 1));
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    errno = EINVAL;
    return -1;
  }
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 64) != 0) {
    close(fd);
    return -1;
  }
  socklen_t size = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &size) == 0)
    *port_out = ntohs(addr.sin_port);
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
  srv.tcp_fd = -1;
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
  if (srv.engine == NULL) srv.engine = wd_engine_new(srv.state_dir);
  const char *module_dir = getenv("WARD_MODULE_DIR");
  char module_path[PATH_MAX];
  if (module_dir == NULL) {
    snprintf(module_path, sizeof(module_path), "%s/modules", srv.state_dir);
    module_dir = module_path;
  }
  srv.module_dir = strdup(module_dir);
  wd_registry_init(&srv.registry);
  (void)wd_registry_scan(&srv.registry, module_dir);
  if (srv.engine != NULL) {
    char *sched_err = NULL;
    srv.scheduler = wd_scheduler_new(srv.engine, module_dir, &sched_err);
    if (srv.scheduler == NULL) {
      fprintf(stderr, "ward: schedules disabled: %s\n",
              sched_err != NULL ? sched_err : "cannot load schedule.wasm");
      free(sched_err);
    }
  }
  const char *max_runs = getenv("WARD_MAX_RUNS");
  srv.max_runs = 8;
  if (max_runs != NULL) {
    long value = strtol(max_runs, NULL, 10);
    if (value > 0 && value <= WD_MAX_RUNS) srv.max_runs = (size_t)value;
  }
  char *models_path = wd_models_path(srv.state_dir);
  char *credentials_path = wd_credentials_path(srv.state_dir);
  if (credentials_path != NULL) {
    char *credentials_err = NULL;
    srv.credentials = wd_credentials_load(credentials_path, &credentials_err);
    free(credentials_err);
    free(credentials_path);
  }
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
  const char *tcp_bind = getenv("WARD_TCP_BIND");
  if (tcp_bind != NULL && tcp_bind[0] != '\0') {
    int tcp_port = 0;
    srv.tcp_fd = setup_tcp_listener(tcp_bind, &tcp_port);
    if (srv.tcp_fd < 0) {
      fprintf(stderr, "ward: cannot listen on %s\n", tcp_bind);
      goto cleanup;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &token_tcp;
    if (epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, srv.tcp_fd, &ev) != 0)
      goto cleanup;
    printf("tcp 127.0.0.1:%d\n", tcp_port);
  }
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
    for (wd_sched *entry = srv.schedules; entry != NULL; entry = entry->next) {
      if (entry->next_ms == 0) continue;
      uint64_t delta = entry->next_ms > now ? entry->next_ms - now : 0;
      int wait = delta > 60000 ? 60000 : (int)delta;
      if (timeout < 0 || wait < timeout) timeout = wait;
    }
    int n = epoll_wait(srv.epoll_fd, events, 64, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (srv.schedules != NULL) drain_schedules(&srv, wd_now_ms());
    if (n == 0 && srv.quic != NULL) {
      (void)wd_quic_service(srv.quic, wd_now_ms());
      (void)wd_quic_flush(srv.quic);
    }
    for (int i = 0; i < n; i++) {
      void *ptr = events[i].data.ptr;
      if (ptr == &token_tcp) {
        server_accept(&srv, srv.tcp_fd);
        continue;
      }
      if (ptr == &token_listen) {
        server_accept(&srv, srv.listen_fd);
        continue;
      }
      if ((uintptr_t)ptr >= (uintptr_t)&term_tokens[0] &&
          (uintptr_t)ptr < (uintptr_t)&term_tokens[WD_MAX_TERMS]) {
        wd_term_token *tok = ptr;
        if (tok->term != NULL && tok->term->used) term_pump(&srv, tok->term);
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
  if (srv.tcp_fd >= 0) close(srv.tcp_fd);
  if (srv.listen_fd >= 0) close(srv.listen_fd);
  if (srv.epoll_fd >= 0) close(srv.epoll_fd);
  if (srv.signal_fd >= 0) close(srv.signal_fd);
  while (srv.runs != NULL) run_free(&srv, srv.runs);
  while (srv.queue != NULL) {
    wd_queue *item = srv.queue;
    srv.queue = item->next;
    free(item->text);
    free(item);
  }
  while (srv.schedules != NULL) {
    wd_sched *entry = srv.schedules;
    srv.schedules = entry->next;
    free(entry->cron);
    free(entry->text);
    free(entry);
  }
  wd_scheduler_free(srv.scheduler);
  wd_engine_free(srv.engine);
  wd_quic_free(srv.quic);
  wd_pki_free(srv.pki);
  wd_approvals_free(srv.approvals);
  wd_models_free(srv.models);
  wd_credentials_free(srv.credentials);
  wd_labels_free(srv.labels);
  free(srv.default_model);
  free(srv.quic_endpoint);
  free(srv.module_dir);
  wd_store_close(&srv.store);
  free(srv.socket_path);
  free(srv.state_dir);
  free(srv.lock_path);
  return srv.stop ? 0 : 1;
}
