/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_SESSION_H
#define WARD_CORE_SESSION_H

#include <cbor.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/ids.h"
#include "core/log.h"

#define WD_PIN_MAX 12

typedef struct {
  char name[32];
  char version[16];
} wd_pin;

typedef struct {
  char id[WD_ID_LEN];
  char *dir;
  char *log_path;
  char *cwd;
  char *model;
  char *system;
  int64_t created_ms;
  uint64_t last_seq;
  hlog log;
  wd_pin pins[WD_PIN_MAX];
  size_t pins_len;
} wd_session;

typedef struct {
  char *root;
  wd_session **items;
  size_t len;
  size_t cap;
} wd_store;

int wd_store_open(wd_store *st, const char *root);
void wd_store_close(wd_store *st);
wd_session *wd_session_find(wd_store *st, const char *id);
int wd_session_create(wd_store *st, const char *cwd, const char *model,
              wd_session **out);
cbor_item_t *wd_session_to_cbor(const wd_session *s);
int wd_session_emit(wd_session *s, const char *kind, cbor_item_t *payload,
            uint64_t *seq_out);
int wd_session_attach(wd_session *s, const char *name, const uint8_t *data,
                      size_t len, char **path_out, char **err);
int wd_session_upgrade(wd_session *s, const char *name, const char *version,
                      uint64_t *seq_out);
int wd_session_note_migration(wd_session *s, const char *name,
                              const char *from, const char *to,
                              const char *reason, uint64_t *seq_out);
int wd_session_set_pins(wd_session *s, const wd_pin *pins, size_t n,
                       uint64_t *seq_out);
int wd_session_set_model(wd_session *s, const char *model, uint64_t *seq_out);
int wd_session_set_system(wd_session *s, const char *text, uint64_t *seq_out);
int wd_session_fork(wd_store *st, wd_session *parent, uint64_t upto, wd_session **out);
int wd_session_checkpoint(wd_session *s, const char *label, uint64_t *seq_out);
int wd_session_last_checkpoint(wd_session *s, uint64_t *seq_out);

typedef int (*wd_session_replay_cb)(void *ctx, uint64_t seq, cbor_item_t *event);

int wd_session_replay(wd_session *s, uint64_t from, wd_session_replay_cb cb, void *ctx);

typedef struct {
  uint64_t events;
  uint64_t turns;
  uint64_t text_bytes;
  uint64_t prompt_bytes;
} wd_session_stats;

int wd_session_measure(wd_session *s, wd_session_stats *out);
int wd_session_history(wd_session *s, size_t budget, cbor_item_t **out);

#endif
