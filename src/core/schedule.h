/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_SCHEDULE_H
#define WARD_CORE_SCHEDULE_H

#include <stdint.h>

#include "wasm/host.h"

typedef struct wd_scheduler wd_scheduler;

wd_scheduler *wd_scheduler_new(wd_engine *engine, const char *module_dir,
                               char **err);
void wd_scheduler_free(wd_scheduler *scheduler);
int wd_scheduler_next(wd_scheduler *scheduler, const char *cron, uint64_t from_ms,
                      uint64_t *next_ms, char **err);

#endif
