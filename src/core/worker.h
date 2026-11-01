/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_WORKER_H
#define WARD_CORE_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "core/proc.h"

typedef struct wd_worker wd_worker;

wd_worker *wd_worker_new(const char *root, char **err);
void wd_worker_free(wd_worker *worker);
pid_t wd_worker_pid(const wd_worker *worker);
bool wd_worker_alive(const wd_worker *worker);
int wd_worker_run(wd_worker *worker, const char *const argv[],
                  uint64_t timeout_ms, size_t max_bytes, wd_proc_result *out,
                  char **err);

#endif
