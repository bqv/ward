/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_PROC_H
#define WARD_CORE_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int status;
  bool timed_out;
  bool truncated;
  uint8_t *out;
  size_t len;
} wd_proc_result;

int wd_proc_run(const char *const argv[], const char *cwd, uint64_t timeout_ms,
                size_t max_bytes, wd_proc_result *out, char **err);
void wd_proc_result_free(wd_proc_result *result);

#endif
