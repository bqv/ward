/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_JUDGE_H
#define WARD_CORE_JUDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  WD_VERDICT_ALLOW = 0,
  WD_VERDICT_ASK = 1,
  WD_VERDICT_DENY = 2
} wd_verdict;

typedef struct wd_judge wd_judge;

typedef int (*wd_judge_ask)(void *ctx, const char *tool, const uint8_t *args,
                            size_t args_len, wd_verdict *out);

wd_verdict wd_verdict_max(wd_verdict rule, wd_verdict judge);
const char *wd_verdict_name(wd_verdict verdict);

wd_judge *wd_judge_new(size_t capacity);
void wd_judge_free(wd_judge *judge);
size_t wd_judge_cached(const wd_judge *judge);

wd_verdict wd_judge_decide(wd_judge *judge, wd_verdict rule, const char *tool,
                           const uint8_t *args, size_t args_len,
                           wd_judge_ask ask, void *ctx, bool *consulted);

#endif
