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

typedef struct {
  char id[WD_ID_LEN];
  char *dir;
  char *log_path;
  char *cwd;
  char *model;
  int64_t created_ms;
  uint64_t last_seq;
  hlog log;
} hs_session;

typedef struct {
  char *root;
  hs_session **items;
  size_t len;
  size_t cap;
} hs_store;

int hs_open(hs_store *st, const char *root);
void hs_close(hs_store *st);
hs_session *hs_find(hs_store *st, const char *id);
int hs_create(hs_store *st, const char *cwd, const char *model,
              hs_session **out);
cbor_item_t *hs_to_cbor(const hs_session *s);
int hs_emit(hs_session *s, const char *kind, cbor_item_t *payload,
            uint64_t *seq_out);
int hs_set_model(hs_session *s, const char *model, uint64_t *seq_out);
int hs_fork(hs_store *st, hs_session *parent, uint64_t upto, hs_session **out);
int hs_checkpoint(hs_session *s, const char *label, uint64_t *seq_out);
int hs_last_checkpoint(hs_session *s, uint64_t *seq_out);

typedef int (*hs_replay_cb)(void *ctx, uint64_t seq, cbor_item_t *event);

int hs_replay(hs_session *s, uint64_t from, hs_replay_cb cb, void *ctx);

typedef struct {
  uint64_t events;
  uint64_t turns;
  uint64_t text_bytes;
  uint64_t prompt_bytes;
} hs_stats;

int hs_measure(hs_session *s, hs_stats *out);
int hs_history(hs_session *s, size_t budget, cbor_item_t **out);

#endif
