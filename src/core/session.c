/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/session.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/util.h"
#include "proto/codec.h"

typedef struct {
  wd_session session;
  bool header;
} load_ctx;

static int load_record(void *ctx, const uint8_t *data, size_t len) {
  load_ctx *lc = ctx;
  cbor_item_t *root = wd_decode(data, len);
  if (root == NULL) return 1;
  int stop = 0;
  if (!lc->header) {
    uint64_t v = 0;
    char *id = wd_text_copy(wd_get(root, "id"));
    char *cwd = wd_text_copy(wd_get(root, "cwd"));
    char *model = wd_text_copy(wd_get(root, "model"));
    uint64_t created = 0;
    bool ok = wd_uint_of(wd_get(root, "v"), &v) && v == 1 && id != NULL &&
              cwd != NULL && model != NULL &&
              wd_uint_of(wd_get(root, "created"), &created);
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
    if (wd_uint_of(wd_get(root, "seq"), &seq) && seq > lc->session.last_seq)
      lc->session.last_seq = seq;
  }
  wd_free(root);
  return stop;
}

static void session_free(wd_session *s) {
  if (s->log.fd >= 0) hlog_close(&s->log);
  free(s->dir);
  free(s->log_path);
  free(s->cwd);
  free(s->model);
  free(s->system);
  free(s->head);
  free(s);
}

static int apply_model(void *ctx, uint64_t seq, cbor_item_t *event) {
  wd_session *s = ctx;
  (void)seq;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind != NULL && strcmp(kind, "session.head") == 0) {
    char *text = wd_text_copy(wd_get(wd_get(event, "payload"), "text"));
    if (text != NULL) {
      free(s->head);
      s->head = text;
    }
  }
  if (kind != NULL && strcmp(kind, "session.system") == 0) {
    char *text = wd_text_copy(wd_get(wd_get(event, "payload"), "text"));
    if (text != NULL) {
      free(s->system);
      s->system = text;
    }
  }
  if (kind != NULL && strcmp(kind, "session.model") == 0) {
    char *model = wd_text_copy(wd_get(wd_get(event, "payload"), "model"));
    if (model != NULL) {
      free(s->model);
      s->model = model;
    }
  }
  free(kind);
  return 0;
}

static int apply_upgrade(void *ctx, uint64_t seq, cbor_item_t *event) {
  wd_session *s = ctx;
  (void)seq;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind != NULL && strcmp(kind, "session.migration_failed") == 0) {
    cbor_item_t *payload = wd_get(event, "payload");
    char *name = wd_text_copy(wd_get(payload, "module"));
    char *from = wd_text_copy(wd_get(payload, "from"));
    if (name != NULL && from != NULL && from[0] != '\0') {
      for (size_t i = 0; i < s->pins_len; i++) {
        if (strcmp(s->pins[i].name, name) != 0) continue;
        snprintf(s->pins[i].version, sizeof(s->pins[i].version), "%s", from);
        break;
      }
    }
    free(name);
    free(from);
  }
  if (kind != NULL && strcmp(kind, "session.upgraded") == 0) {
    cbor_item_t *payload = wd_get(event, "payload");
    char *name = wd_text_copy(wd_get(payload, "module"));
    char *version = wd_text_copy(wd_get(payload, "to"));
    if (name != NULL && version != NULL) {
      bool found = false;
      for (size_t i = 0; i < s->pins_len; i++) {
        if (strcmp(s->pins[i].name, name) != 0) continue;
        snprintf(s->pins[i].version, sizeof(s->pins[i].version), "%s", version);
        found = true;
        break;
      }
      if (!found && s->pins_len < WD_PIN_MAX) {
        snprintf(s->pins[s->pins_len].name, sizeof(s->pins[0].name), "%s", name);
        snprintf(s->pins[s->pins_len].version, sizeof(s->pins[0].version), "%s",
                 version);
        s->pins_len++;
      }
    }
    free(name);
    free(version);
  }
  free(kind);
  return 0;
}

static int apply_pins(void *ctx, uint64_t seq, cbor_item_t *event) {
  wd_session *s = ctx;
  (void)seq;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind != NULL && strcmp(kind, "session.modules") == 0) {
    cbor_item_t *modules = wd_get(wd_get(event, "payload"), "modules");
    if (modules != NULL && cbor_isa_map(modules)) {
      s->pins_len = 0;
      struct cbor_pair *pairs = cbor_map_handle(modules);
      for (size_t i = 0; i < cbor_map_size(modules) && i < WD_PIN_MAX; i++) {
        char *name = wd_text_copy(pairs[i].key);
        char *version = wd_text_copy(pairs[i].value);
        if (name == NULL || version == NULL) {
          free(name);
          free(version);
          continue;
        }
        snprintf(s->pins[s->pins_len].name, sizeof(s->pins[0].name), "%s",
                 name);
        snprintf(s->pins[s->pins_len].version, sizeof(s->pins[0].version),
                 "%s", version);
        s->pins_len++;
        free(name);
        free(version);
      }
    }
  }
  free(kind);
  return 0;
}

int wd_session_upgrade(wd_session *s, const char *name, const char *version,
                       uint64_t *seq_out) {
  if (name == NULL || version == NULL || name[0] == '\0' || version[0] == '\0')
    return -1;
  const char *was = "";
  bool found = false;
  for (size_t i = 0; i < s->pins_len; i++) {
    if (strcmp(s->pins[i].name, name) != 0) continue;
    if (strcmp(s->pins[i].version, version) == 0) return 0;
    was = s->pins[i].version;
    snprintf(s->pins[i].version, sizeof(s->pins[i].version), "%s", version);
    found = true;
    break;
  }
  if (!found) {
    if (s->pins_len >= WD_PIN_MAX) return -1;
    snprintf(s->pins[s->pins_len].name, sizeof(s->pins[0].name), "%s", name);
    snprintf(s->pins[s->pins_len].version, sizeof(s->pins[0].version), "%s",
             version);
    s->pins_len++;
  }
  cbor_item_t *payload = wd_map(3);
  wd_put(payload, "module", wd_text(name));
  wd_put(payload, "from", wd_text(was));
  wd_put(payload, "to", wd_text(version));
  return wd_session_emit(s, "session.upgraded", payload, seq_out);
}

int wd_session_note_migration(wd_session *s, const char *name,
                              const char *from, const char *to,
                              const char *reason, uint64_t *seq_out) {
  if (name == NULL || name[0] == '\0') return -1;
  cbor_item_t *payload = wd_map(4);
  wd_put(payload, "module", wd_text(name));
  wd_put(payload, "from", wd_text(from != NULL ? from : ""));
  wd_put(payload, "to", wd_text(to != NULL ? to : ""));
  wd_put(payload, "reason", wd_text(reason != NULL ? reason : "refused"));
  return wd_session_emit(s, "session.migration_failed", payload, seq_out);
}

int wd_session_set_pins(wd_session *s, const wd_pin *pins, size_t n,
                        uint64_t *seq_out) {
  if (n > WD_PIN_MAX) return -1;
  cbor_item_t *modules = wd_map(n);
  for (size_t i = 0; i < n; i++)
    wd_put(modules, pins[i].name, wd_text(pins[i].version));
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "modules", modules);
  if (wd_session_emit(s, "session.modules", payload, seq_out) != 0) return -1;
  memcpy(s->pins, pins, n * sizeof(*pins));
  s->pins_len = n;
  return 0;
}

static int session_load(const char *root, const char *id, wd_session **out) {
  wd_session *s = calloc(1, sizeof(*s));
  if (s == NULL) return -1;
  s->log.fd = -1;
  s->dir = wd_path_join(root, id);
  if (s->dir == NULL) goto fail;
  s->log_path = wd_path_join(s->dir, "events.zst");
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
  if (wd_session_replay(s, 0, apply_model, s) < 0) goto fail;
  if (wd_session_replay(s, 0, apply_pins, s) < 0) goto fail;
  if (wd_session_replay(s, 0, apply_upgrade, s) < 0) goto fail;
  *out = s;
  return 0;
fail:
  session_free(s);
  return -1;
}

static int compare_sessions(const void *a, const void *b) {
  const wd_session *x = *(wd_session *const *)a;
  const wd_session *y = *(wd_session *const *)b;
  if (x->created_ms != y->created_ms)
    return x->created_ms > y->created_ms ? -1 : 1;
  return strcmp(x->id, y->id);
}

static void store_push(wd_store *st, wd_session *s) {
  if (st->len == st->cap) {
    size_t cap = st->cap ? st->cap * 2 : 8;
    wd_session **items = realloc(st->items, cap * sizeof(*items));
    if (items == NULL) return;
    st->items = items;
    st->cap = cap;
  }
  st->items[st->len++] = s;
}

static bool valid_id(const char *name) {
  if (strlen(name) != WD_ID_LEN - 1) return false;
  for (const char *p = name; *p != '\0'; p++) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
  }
  return true;
}

int wd_store_open(wd_store *st, const char *root) {
  memset(st, 0, sizeof(*st));
  st->root = strdup(root);
  if (st->root == NULL) return -1;
  if (wd_mkdir_p(st->root, 0700) != 0) return -1;
  DIR *dir = opendir(st->root);
  if (dir == NULL) return -1;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    if (!valid_id(entry->d_name)) continue;
    char *path = wd_path_join(st->root, entry->d_name);
    if (path == NULL) continue;
    struct stat sb;
    bool is_dir = stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
    free(path);
    if (!is_dir) continue;
    wd_session *s = NULL;
    if (session_load(st->root, entry->d_name, &s) == 0) {
      store_push(st, s);
    } else {
      fprintf(stderr, "ward: skipping unreadable session %s\n",
              entry->d_name);
    }
  }
  closedir(dir);
  if (st->len > 1)
    qsort(st->items, st->len, sizeof(*st->items), compare_sessions);
  return 0;
}

void wd_store_close(wd_store *st) {
  for (size_t i = 0; i < st->len; i++) session_free(st->items[i]);
  free(st->items);
  free(st->root);
  memset(st, 0, sizeof(*st));
}

wd_session *wd_session_find(wd_store *st, const char *id) {
  if (id == NULL) return NULL;
  for (size_t i = 0; i < st->len; i++)
    if (strcmp(st->items[i]->id, id) == 0) return st->items[i];
  return NULL;
}

int wd_session_create(wd_store *st, const char *cwd, const char *model,
              wd_session **out) {
  wd_session *s = calloc(1, sizeof(*s));
  if (s == NULL) return -1;
  s->log.fd = -1;
  for (int attempt = 0; attempt < 16; attempt++) {
    if (wd_new_id(s->id) != 0) goto fail;
    if (wd_session_find(st, s->id) != NULL) continue;
    s->dir = wd_path_join(st->root, s->id);
    if (s->dir == NULL) goto fail;
    if (mkdir(s->dir, 0700) == 0) break;
    free(s->dir);
    s->dir = NULL;
    if (errno != EEXIST) goto fail;
  }
  if (s->dir == NULL) goto fail;
  s->log_path = wd_path_join(s->dir, "events.zst");
  if (s->log_path == NULL) goto fail;
  s->cwd = strdup(cwd);
  s->model = strdup(model != NULL ? model : "unset");
  if (s->cwd == NULL || s->model == NULL) goto fail;
  s->created_ms = (int64_t)wd_now_ms();
  if (hlog_create(&s->log, s->log_path) != 0) goto fail;
  cbor_item_t *header = wd_map(5);
  wd_put(header, "v", wd_uint(1));
  wd_put(header, "id", wd_text(s->id));
  wd_put(header, "cwd", wd_text(s->cwd));
  wd_put(header, "created", wd_uint((uint64_t)s->created_ms));
  wd_put(header, "model", wd_text(s->model));
  size_t len = 0;
  uint8_t *bytes = wd_encode(header, &len);
  if (bytes == NULL) goto fail;
  int rc = hlog_append(&s->log, bytes, len);
  free(bytes);
  if (rc != 0) goto fail;
  if (wd_fsync_dir(s->dir) != 0) goto fail;
  store_push(st, s);
  if (st->len > 1) qsort(st->items, st->len, sizeof(*st->items), compare_sessions);
  *out = s;
  return 0;
fail:
  session_free(s);
  return -1;
}

cbor_item_t *wd_session_to_cbor(const wd_session *s) {
  cbor_item_t *root = wd_map(6);
  wd_put(root, "id", wd_text(s->id));
  wd_put(root, "cwd", wd_text(s->cwd));
  wd_put(root, "model", wd_text(s->model));
  wd_put(root, "created", wd_uint((uint64_t)s->created_ms));
  wd_put(root, "last_seq", wd_uint(s->last_seq));
  cbor_item_t *pins = wd_array(s->pins_len);
  for (size_t i = 0; i < s->pins_len; i++) {
    cbor_item_t *entry = wd_map(2);
    wd_put(entry, "name", wd_text(s->pins[i].name));
    wd_put(entry, "version", wd_text(s->pins[i].version));
    bool pushed = cbor_array_push(pins, entry);
    cbor_decref(&entry);
    if (!pushed) break;
  }
  wd_put(root, "pins", pins);
  return root;
}

int wd_session_emit(wd_session *s, const char *kind, cbor_item_t *payload,
            uint64_t *seq_out) {
  uint64_t seq = s->last_seq + 1;
  cbor_item_t *record = wd_map(3);
  wd_put(record, "seq", wd_uint(seq));
  wd_put(record, "kind", wd_text(kind));
  wd_put(record, "payload", payload != NULL ? payload : wd_map(0));
  size_t len = 0;
  uint8_t *bytes = wd_encode(record, &len);
  if (bytes == NULL) return -1;
  int rc = hlog_append(&s->log, bytes, len);
  free(bytes);
  if (rc != 0) return -1;
  s->last_seq = seq;
  if (seq_out != NULL) *seq_out = seq;
  return 0;
}

int wd_session_attach(wd_session *s, const char *name, const uint8_t *data,
                      size_t len, char **path_out, char **err) {
  if (name == NULL || name[0] == '\0' || strlen(name) > 128 ||
      strchr(name, '/') != NULL || strstr(name, "..") != NULL) {
    if (err != NULL) *err = strdup("the attachment name is refused");
    return -1;
  }
  char *dir = wd_path_join(s->dir, "attachments");
  if (dir == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return -1;
  }
  (void)mkdir(dir, 0700);
  char *path = wd_path_join(dir, name);
  free(dir);
  if (path == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return -1;
  }
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    free(path);
    if (err != NULL) *err = strdup("cannot write the attachment");
    return -1;
  }
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    (void)unlink(path);
    free(path);
    if (err != NULL) *err = strdup("cannot write the attachment");
    return -1;
  }
  cbor_item_t *payload = wd_map(3);
  wd_put(payload, "name", wd_text(name));
  wd_put(payload, "size", wd_uint((uint64_t)len));
  wd_put(payload, "path", wd_text(path));
  if (wd_session_emit(s, "session.attachment", payload, NULL) != 0) {
    (void)unlink(path);
    free(path);
    if (err != NULL) *err = strdup("cannot record the attachment");
    return -1;
  }
  if (path_out != NULL) {
    *path_out = path;
  } else {
    free(path);
  }
  return 0;
}

int wd_session_set_model(wd_session *s, const char *model, uint64_t *seq_out) {
  char *copy = strdup(model);
  if (copy == NULL) return -1;
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "model", wd_text(model));
  int rc = wd_session_emit(s, "session.model", payload, seq_out);
  if (rc != 0) {
    free(copy);
    return -1;
  }
  free(s->model);
  s->model = copy;
  return 0;
}

int wd_session_set_head(wd_session *s, const char *text, uint64_t *seq_out) {
  char *copy = text != NULL ? strdup(text) : NULL;
  if (text != NULL && copy == NULL) return -1;
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "text", wd_text(text != NULL ? text : ""));
  int rc = wd_session_emit(s, "session.head", payload, seq_out);
  if (rc != 0) {
    free(copy);
    return -1;
  }
  free(s->head);
  s->head = copy;
  return 0;
}

int wd_session_set_system(wd_session *s, const char *text, uint64_t *seq_out) {
  char *copy = text != NULL ? strdup(text) : NULL;
  if (text != NULL && copy == NULL) return -1;
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "text", wd_text(text != NULL ? text : ""));
  int rc = wd_session_emit(s, "session.system", payload, seq_out);
  if (rc != 0) {
    free(copy);
    return -1;
  }
  free(s->system);
  s->system = copy;
  return 0;
}

int wd_session_checkpoint(wd_session *s, const char *label, uint64_t *seq_out) {
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "label", wd_text(label != NULL ? label : ""));
  return wd_session_emit(s, "session.checkpoint", payload, seq_out);
}

static int find_checkpoint(void *ctx, uint64_t seq, cbor_item_t *event) {
  uint64_t *last = ctx;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind != NULL && strcmp(kind, "session.checkpoint") == 0) *last = seq;
  free(kind);
  return 0;
}

int wd_session_last_checkpoint(wd_session *s, uint64_t *seq_out) {
  uint64_t last = 0;
  if (wd_session_replay(s, 0, find_checkpoint, &last) < 0) return -1;
  if (last == 0) return -1;
  *seq_out = last;
  return 0;
}

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

typedef struct {
  uint64_t upto;
  wd_session *child;
} fork_ctx;

static int fork_record(void *ctx, uint64_t seq, cbor_item_t *event) {
  fork_ctx *fc = ctx;
  if (fc->upto != 0 && seq > fc->upto) return 1;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  cbor_item_t *payload = wd_get(event, "payload");
  int stop = 0;
  if (kind != NULL && payload != NULL) {
    cbor_item_t *copy = dup_item(payload);
    if (copy == NULL)
      stop = 1;
    else if (wd_session_emit(fc->child, kind, copy, NULL) != 0)
      stop = 1;
  }
  free(kind);
  return stop;
}

int wd_session_fork(wd_store *st, wd_session *parent, uint64_t upto,
            wd_session **out) {
  if (parent == NULL) return -1;
  if (upto == 0 || upto > parent->last_seq) upto = parent->last_seq;
  wd_session *child = NULL;
  if (wd_session_create(st, parent->cwd, parent->model, &child) != 0) return -1;
  fork_ctx fc = {.upto = upto, .child = child};
  if (wd_session_replay(parent, 0, fork_record, &fc) < 0) return -1;
  cbor_item_t *link = wd_map(2);
  wd_put(link, "from", wd_text(parent->id));
  wd_put(link, "at", wd_uint(upto));
  if (wd_session_emit(child, "session.forked", link, NULL) != 0) return -1;
  *out = child;
  return 0;
}

static int measure_record(void *ctx, uint64_t seq, cbor_item_t *event) {
  wd_session_stats *stats = ctx;
  (void)seq;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  if (kind != NULL) {
    if (strcmp(kind, "turn.end") == 0) stats->turns++;
    if (strcmp(kind, "text.delta") == 0) {
      char *text = wd_text_copy(wd_get(wd_get(event, "payload"), "text"));
      if (text != NULL) stats->text_bytes += strlen(text);
      free(text);
    }
    if (strcmp(kind, "turn.timings") == 0) {
      cbor_item_t *payload = wd_get(event, "payload");
      uint64_t value = 0;
      if (wd_uint_of(wd_get(payload, "prompt_n"), &value))
        stats->prompt_tokens += value;
      if (wd_uint_of(wd_get(payload, "predicted_n"), &value))
        stats->predicted_tokens += value;
      if (wd_uint_of(wd_get(payload, "cache_n"), &value))
        stats->cache_tokens += value;
      if (wd_uint_of(wd_get(payload, "prompt_ms"), &value))
        stats->prompt_ms += value;
      if (wd_uint_of(wd_get(payload, "predicted_ms"), &value))
        stats->predicted_ms += value;
    }
    if (strcmp(kind, "session.prompted") == 0) {
      char *text = wd_text_copy(wd_get(wd_get(event, "payload"), "text"));
      if (text != NULL) stats->prompt_bytes += strlen(text);
      free(text);
    }
  }
  free(kind);
  return 0;
}

int wd_session_measure(wd_session *s, wd_session_stats *out) {
  memset(out, 0, sizeof(*out));
  if (wd_session_replay(s, 0, measure_record, out) < 0) return -1;
  out->events = s->last_seq;
  return 0;
}

typedef struct {
  char *role;
  char *text;
} hist_turn;

typedef struct {
  hist_turn *turns;
  size_t len;
  size_t cap;
  char *pending;
  size_t pending_len;
  char *goal;
} hist_ctx;

static int hist_push(hist_ctx *hc, const char *role, const char *text) {
  if (hc->len == hc->cap) {
    size_t grown = hc->cap == 0 ? 8 : hc->cap * 2;
    hist_turn *next = realloc(hc->turns, grown * sizeof(*next));
    if (next == NULL) return -1;
    hc->turns = next;
    hc->cap = grown;
  }
  hc->turns[hc->len].role = strdup(role);
  hc->turns[hc->len].text = strdup(text != NULL ? text : "");
  if (hc->turns[hc->len].role == NULL || hc->turns[hc->len].text == NULL)
    return -1;
  hc->len++;
  return 0;
}

static int history_record(void *ctx, uint64_t seq, cbor_item_t *event) {
  hist_ctx *hc = ctx;
  (void)seq;
  char *kind = wd_text_copy(wd_get(event, "kind"));
  cbor_item_t *payload = wd_get(event, "payload");
  if (kind == NULL) return 0;
  if (strcmp(kind, "session.prompted") == 0) {
    char *text = wd_text_copy(wd_get(payload, "text"));
    if (text != NULL) {
      if (hist_push(hc, "user", text) != 0) {
        free(text);
        free(kind);
        return 1;
      }
      free(text);
    }
  } else if (strcmp(kind, "session.time") == 0) {
    char *text = wd_text_copy(wd_get(payload, "text"));
    if (text != NULL) {
      if (hist_push(hc, "system", text) != 0) {
        free(text);
        free(kind);
        return 1;
      }
      free(text);
    }
  } else if (strcmp(kind, "text.delta") == 0) {
    char *piece = wd_text_copy(wd_get(payload, "text"));
    if (piece != NULL) {
      size_t add = strlen(piece);
      char *grown = realloc(hc->pending, hc->pending_len + add + 1);
      if (grown == NULL) {
        free(piece);
        free(kind);
        return 1;
      }
      hc->pending = grown;
      memcpy(hc->pending + hc->pending_len, piece, add);
      hc->pending_len += add;
      hc->pending[hc->pending_len] = '\0';
      free(piece);
    }
  } else if (strcmp(kind, "session.compacted") == 0) {
    char *summary = wd_text_copy(wd_get(payload, "summary"));
    if (summary != NULL) {
      for (size_t i = 0; i < hc->len; i++) {
        free(hc->turns[i].role);
        free(hc->turns[i].text);
      }
      hc->len = 0;
      if (hc->pending != NULL) hc->pending[0] = '\0';
      hc->pending_len = 0;
      int pushed = hist_push(hc, "system", summary);
      free(summary);
      if (pushed != 0) {
        free(kind);
        return 1;
      }
    }
  } else if (strcmp(kind, "goal.set") == 0) {
    char *text = wd_text_copy(wd_get(payload, "text"));
    if (text != NULL) {
      free(hc->goal);
      hc->goal = text;
    }
  } else if (strcmp(kind, "goal.cleared") == 0) {
    free(hc->goal);
    hc->goal = NULL;
  } else if (strcmp(kind, "turn.end") == 0) {
    if (hc->pending != NULL) {
      if (hist_push(hc, "assistant", hc->pending) != 0) {
        free(kind);
        return 1;
      }
      hc->pending[0] = '\0';
      hc->pending_len = 0;
    }
  }
  free(kind);
  return 0;
}

int wd_session_history(wd_session *s, size_t budget, cbor_item_t **out) {
  hist_ctx hc;
  memset(&hc, 0, sizeof(hc));
  int rc = wd_session_replay(s, 0, history_record, &hc);
  free(hc.pending);
  if (rc < 0) {
    for (size_t i = 0; i < hc.len; i++) {
      free(hc.turns[i].role);
      free(hc.turns[i].text);
    }
    free(hc.turns);
    return -1;
  }
  if (hc.goal != NULL) {
    int pushed = hist_push(&hc, "system", hc.goal);
    free(hc.goal);
    hc.goal = NULL;
    if (pushed != 0) {
      for (size_t i = 0; i < hc.len; i++) {
        free(hc.turns[i].role);
        free(hc.turns[i].text);
      }
      free(hc.turns);
      return -1;
    }
  }
  (void)budget;
  cbor_item_t *array = wd_array(hc.len);
  for (size_t i = 0; i < hc.len; i++) {
    cbor_item_t *entry = wd_map(2);
    wd_put(entry, "role", wd_text(hc.turns[i].role));
    wd_put(entry, "text", wd_text(hc.turns[i].text));
    bool pushed = cbor_array_push(array, entry);
    cbor_decref(&entry);
    if (!pushed) break;
  }
  for (size_t i = 0; i < hc.len; i++) {
    free(hc.turns[i].role);
    free(hc.turns[i].text);
  }
  free(hc.turns);
  *out = array;
  return 0;
}

typedef struct {
  uint64_t from;
  wd_session_replay_cb cb;
  void *ctx;
  int emitted;
} replay_ctx;

static int replay_record(void *ctx, const uint8_t *data, size_t len) {
  replay_ctx *rc = ctx;
  cbor_item_t *root = wd_decode(data, len);
  if (root == NULL) return 1;
  uint64_t seq = 0;
  int stop = 0;
  if (wd_uint_of(wd_get(root, "seq"), &seq) && seq > rc->from) {
    cbor_item_t *kind = wd_get(root, "kind");
    cbor_item_t *payload = wd_get(root, "payload");
    char *name = wd_text_copy(kind);
    if (name != NULL && payload != NULL) {
      (void)cbor_incref(payload);
      cbor_item_t *event = wd_event(name, payload);
      stop = rc->cb(rc->ctx, seq, event) != 0;
      wd_free(event);
      rc->emitted++;
    }
    free(name);
  }
  wd_free(root);
  return stop;
}

int wd_session_replay(wd_session *s, uint64_t from, wd_session_replay_cb cb, void *ctx) {
  replay_ctx rc = {.from = from, .cb = cb, .ctx = ctx, .emitted = 0};
  bool truncated = false;
  int n = hlog_read(s->log_path, replay_record, &rc, &truncated);
  if (n < 0) return -1;
  return rc.emitted;
}
