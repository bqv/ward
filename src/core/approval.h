/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_APPROVAL_H
#define WARD_CORE_APPROVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_MAX_APPROVALS 64
#define WD_APPROVAL_SESSION_MAX 40

typedef enum {
  WD_APPROVAL_UNKNOWN = 0,
  WD_APPROVAL_PENDING,
  WD_APPROVAL_GRANTED,
  WD_APPROVAL_DENIED,
  WD_APPROVAL_ALREADY_RESOLVED,
} wd_approval_state;

typedef struct wd_approvals wd_approvals;

wd_approvals *wd_approvals_new(void);
void wd_approvals_free(wd_approvals *table);
void wd_approvals_clear(wd_approvals *table);

wd_approval_state wd_approval_request(wd_approvals *table, const char *session,
                                      uint64_t call, uint64_t epoch,
                                      uint64_t now_ms, uint64_t ttl_ms);
wd_approval_state wd_approval_answer(wd_approvals *table, const char *session,
                                     uint64_t call, uint64_t epoch, bool allow,
                                     uint64_t now_ms);
wd_approval_state wd_approval_lookup(const wd_approvals *table,
                                     const char *session, uint64_t call,
                                     uint64_t epoch, uint64_t now_ms);
size_t wd_approval_pending(const wd_approvals *table, uint64_t now_ms);

#endif
