/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/session.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/util.h"
#include "proto/codec.h"

typedef struct {
  hs_session session;
  bool header;
} load_ctx;

static int load_record(void *ctx, const uint8_t *data, size_t len) {
  load_ctx *lc = ctx;
  cbor_item_t *root = hx_decode(data, len);
  if (root == NULL) return 1;
  int stop = 0;
  if (!lc->header) {
    uint64_t v = 0;
    char *id = hx_text_copy(hx_get(root, "id"));
    char *cwd = hx_text_copy(hx_get(root, "cwd"));
    char *model = hx_text_copy(hx_get(root, "model"));
    uint64_t created = 0;
    bool ok = hx_uint_of(hx_get(root, "v"), &v) && v == 1 && id != NULL &&
              cwd != NULL && model != NULL &&
              hx_uint_of(hx_get(root, "created"), &created);
    if (ok) {
      snprintf(lc->session.id, sizeof(lc->session.id), "%s", id);
      lc->session.cwd = cwd;
      lc->session.model = model;
      lc->session.created_ms = (int64_t)created;
      lc->header = true;
    } else {
      free(cwd);
      free(model);
      stop = 1;
    }
    free(id);
  } else {
    uint64_t seq = 0;
    if (hx_uint_of(hx_get(root, "seq"), &seq) && seq > lc->session.last_seq)
      lc->session.last_seq = seq;
  }
  hx_free(root);
  return stop;
}

static void session_free(hs_session *s) {
  if (s->log.fd >= 0) hlog_close(&s->log);
  free(s->dir);
  free(s->log_path);
  free(s->cwd);
  free(s->model);
  free(s);
}

static int session_load(const char *root, const char *id, hs_session **out) {
  hs_session *s = calloc(1, sizeof(*s));
  if (s == NULL) return -1;
  s->log.fd = -1;
  s->dir = hu_path_join(root, id);
  if (s->dir == NULL) goto fail;
  s->log_path = hu_path_join(s->dir, "events.zst");
  if (s->log_path == NULL) goto fail;
  load_ctx lc = {.header = false};
  lc.session.log.fd = -1;
  bool truncated = false;
  if (hlog_read(s->log_path, load_record, &lc, &truncated) <= 0 || !lc.header)
    goto fail;
  snprintf(s->id, sizeof(s->id), "%s", lc.session.id);
  if (strcmp(s->id, id) != 0) goto fail;
  s->cwd = lc.session.cwd;
  s->model = lc.session.model;
  s->created_ms = lc.session.created_ms;
  s->last_seq = lc.session.last_seq;
  if (hlog_open_append(&s->log, s->log_path) != 0) goto fail;
  *out = s;
  return 0;
fail:
  session_free(s);
  return -1;
}

static int compare_sessions(const void *a, const void *b) {
  const hs_session *x = *(hs_session *const *)a;
  const hs_session *y = *(hs_session *const *)b;
  if (x->created_ms != y->created_ms)
    return x->created_ms > y->created_ms ? -1 : 1;
  return strcmp(x->id, y->id);
}

static void store_push(hs_store *st, hs_session *s) {
  if (st->len == st->cap) {
    size_t cap = st->cap ? st->cap * 2 : 8;
    hs_session **items = realloc(st->items, cap * sizeof(*items));
    if (items == NULL) return;
    st->items = items;
    st->cap = cap;
  }
  st->items[st->len++] = s;
}

int hs_open(hs_store *st, const char *root) {
  memset(st, 0, sizeof(*st));
  st->root = strdup(root);
  if (st->root == NULL) return -1;
  if (hu_mkdir_p(st->root, 0700) != 0) return -1;
  DIR *dir = opendir(st->root);
  if (dir == NULL) return -1;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    char *path = hu_path_join(st->root, entry->d_name);
    if (path == NULL) continue;
    struct stat sb;
    bool is_dir = stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
    free(path);
    if (!is_dir) continue;
    hs_session *s = NULL;
    if (session_load(st->root, entry->d_name, &s) == 0) {
      store_push(st, s);
    } else {
      fprintf(stderr, "harness: skipping unreadable session %s\n",
              entry->d_name);
    }
  }
  closedir(dir);
  if (st->len > 1)
    qsort(st->items, st->len, sizeof(*st->items), compare_sessions);
  return 0;
}

void hs_close(hs_store *st) {
  for (size_t i = 0; i < st->len; i++) session_free(st->items[i]);
  free(st->items);
  free(st->root);
  memset(st, 0, sizeof(*st));
}

hs_session *hs_find(hs_store *st, const char *id) {
  if (id == NULL) return NULL;
  for (size_t i = 0; i < st->len; i++)
    if (strcmp(st->items[i]->id, id) == 0) return st->items[i];
  return NULL;
}

int hs_create(hs_store *st, const char *cwd, const char *model,
              hs_session **out) {
  hs_session *s = calloc(1, sizeof(*s));
  if (s == NULL) return -1;
  s->log.fd = -1;
  for (int attempt = 0; attempt < 16; attempt++) {
    if (hx_new_id(s->id) != 0) goto fail;
    if (hs_find(st, s->id) != NULL) continue;
    s->dir = hu_path_join(st->root, s->id);
    if (s->dir == NULL) goto fail;
    if (mkdir(s->dir, 0700) == 0) break;
    free(s->dir);
    s->dir = NULL;
    if (errno != EEXIST) goto fail;
  }
  if (s->dir == NULL) goto fail;
  s->log_path = hu_path_join(s->dir, "events.zst");
  if (s->log_path == NULL) goto fail;
  s->cwd = strdup(cwd);
  s->model = strdup(model != NULL ? model : "unset");
  if (s->cwd == NULL || s->model == NULL) goto fail;
  s->created_ms = (int64_t)hu_now_ms();
  if (hlog_create(&s->log, s->log_path) != 0) goto fail;
  cbor_item_t *header = hx_map(5);
  hx_put(header, "v", hx_uint(1));
  hx_put(header, "id", hx_text(s->id));
  hx_put(header, "cwd", hx_text(s->cwd));
  hx_put(header, "created", hx_uint((uint64_t)s->created_ms));
  hx_put(header, "model", hx_text(s->model));
  size_t len = 0;
  uint8_t *bytes = hx_encode(header, &len);
  if (bytes == NULL) goto fail;
  int rc = hlog_append(&s->log, bytes, len);
  free(bytes);
  if (rc != 0) goto fail;
  if (hu_fsync_dir(s->dir) != 0) goto fail;
  store_push(st, s);
  if (st->len > 1) qsort(st->items, st->len, sizeof(*st->items), compare_sessions);
  *out = s;
  return 0;
fail:
  session_free(s);
  return -1;
}

cbor_item_t *hs_to_cbor(const hs_session *s) {
  cbor_item_t *root = hx_map(5);
  hx_put(root, "id", hx_text(s->id));
  hx_put(root, "cwd", hx_text(s->cwd));
  hx_put(root, "model", hx_text(s->model));
  hx_put(root, "created", hx_uint((uint64_t)s->created_ms));
  hx_put(root, "last_seq", hx_uint(s->last_seq));
  return root;
}

int hs_emit(hs_session *s, const char *kind, cbor_item_t *payload,
            uint64_t *seq_out) {
  uint64_t seq = s->last_seq + 1;
  cbor_item_t *record = hx_map(3);
  hx_put(record, "seq", hx_uint(seq));
  hx_put(record, "kind", hx_text(kind));
  hx_put(record, "payload", payload != NULL ? payload : hx_map(0));
  size_t len = 0;
  uint8_t *bytes = hx_encode(record, &len);
  if (bytes == NULL) return -1;
  int rc = hlog_append(&s->log, bytes, len);
  free(bytes);
  if (rc != 0) return -1;
  s->last_seq = seq;
  if (seq_out != NULL) *seq_out = seq;
  return 0;
}

typedef struct {
  uint64_t from;
  hs_replay_cb cb;
  void *ctx;
  int emitted;
} replay_ctx;

static int replay_record(void *ctx, const uint8_t *data, size_t len) {
  replay_ctx *rc = ctx;
  cbor_item_t *root = hx_decode(data, len);
  if (root == NULL) return 1;
  uint64_t seq = 0;
  int stop = 0;
  if (hx_uint_of(hx_get(root, "seq"), &seq) && seq > rc->from) {
    cbor_item_t *kind = hx_get(root, "kind");
    cbor_item_t *payload = hx_get(root, "payload");
    char *name = hx_text_copy(kind);
    if (name != NULL && payload != NULL) {
      (void)cbor_incref(payload);
      cbor_item_t *event = hx_event(name, payload);
      stop = rc->cb(rc->ctx, seq, event) != 0;
      hx_free(event);
      rc->emitted++;
    }
    free(name);
  }
  hx_free(root);
  return stop;
}

int hs_replay(hs_session *s, uint64_t from, hs_replay_cb cb, void *ctx) {
  replay_ctx rc = {.from = from, .cb = cb, .ctx = ctx, .emitted = 0};
  bool truncated = false;
  int n = hlog_read(s->log_path, replay_record, &rc, &truncated);
  if (n < 0) return -1;
  return rc.emitted;
}
