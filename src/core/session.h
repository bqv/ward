/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HARNESS_CORE_SESSION_H
#define HARNESS_CORE_SESSION_H

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

typedef int (*hs_replay_cb)(void *ctx, uint64_t seq, cbor_item_t *event);

int hs_replay(hs_session *s, uint64_t from, hs_replay_cb cb, void *ctx);

#endif
