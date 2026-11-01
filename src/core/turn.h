/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_TURN_H
#define WARD_CORE_TURN_H

#include <cbor.h>
#include <stddef.h>

#include "wasm/host.h"

typedef struct wd_turn wd_turn;

typedef int (*wd_turn_emit)(void *ctx, const char *event, cbor_item_t *payload,
                            char **err);

wd_turn *wd_turn_new(wd_engine *engine, const char *module_dir, char **err);
void wd_turn_free(wd_turn *turn);
int wd_turn_prompt(wd_turn *turn, const char *text, wd_turn_emit emit,
                   void *ctx, char **err);

#endif
