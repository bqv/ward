/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_TURN_H
#define WARD_CORE_TURN_H

#include <cbor.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm/host.h"

typedef struct wd_turn wd_turn;

#define WD_TURN_OK 0
#define WD_TURN_PENDING 1

typedef int (*wd_turn_emit)(void *ctx, const char *event, cbor_item_t *payload,
                            char **err);

wd_turn *wd_turn_new(wd_engine *engine, const char *module_dir, const char *root,
                     char **err);
void wd_turn_free(wd_turn *turn);
void wd_turn_set_root(wd_turn *turn, const char *root);
void wd_turn_set_history(wd_turn *turn, cbor_item_t *history);
int wd_turn_prompt(wd_turn *turn, const char *text, wd_turn_emit emit,
                   void *ctx, char **err);
int wd_turn_resume(wd_turn *turn, bool allow, wd_turn_emit emit, void *ctx,
                   char **err);
int wd_turn_pending(wd_turn *turn, uint64_t *call, uint64_t *epoch,
                    char **reason);
int wd_turn_pending_kind(wd_turn *turn);
int wd_turn_worker_fd(wd_turn *turn);
int wd_turn_resume_tool(wd_turn *turn, wd_turn_emit emit, void *ctx,
                        char **err);
int wd_turn_cancel_tool(wd_turn *turn);
int wd_turn_resume_http(wd_turn *turn, wd_turn_emit emit, void *ctx,
                        char **err);
int wd_turn_http_fd(wd_turn *turn);
int wd_turn_compact(wd_turn *turn, cbor_item_t *history, char **summary,
                    char **err);

#endif
