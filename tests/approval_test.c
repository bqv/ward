/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "core/approval.h"

int main(void) {
  wd_approvals *table = wd_approvals_new();
  assert(table != NULL);

  assert(wd_approval_request(table, "s1", "write", "write a file", 7, 1, 1000, 500) == WD_APPROVAL_PENDING);
  assert(wd_approval_lookup(table, "s1", 7, 1, 1000) == WD_APPROVAL_PENDING);
  assert(wd_approval_pending(table, 1000) == 1);
  assert(wd_approval_answer(table, "s1", 7, 1, true, 1100) == WD_APPROVAL_GRANTED);
  assert(wd_approval_answer(table, "s1", 7, 1, false, 1200) ==
         WD_APPROVAL_ALREADY_RESOLVED);
  assert(wd_approval_lookup(table, "s1", 7, 1, 1200) == WD_APPROVAL_GRANTED);
  assert(wd_approval_pending(table, 1200) == 0);

  assert(wd_approval_request(table, "s1", "exec", "run a command", 8, 1, 1000, 500) == WD_APPROVAL_PENDING);
  assert(wd_approval_answer(table, "s1", 8, 1, false, 1000) == WD_APPROVAL_DENIED);
  assert(wd_approval_lookup(table, "s1", 8, 1, 1000) == WD_APPROVAL_DENIED);

  assert(wd_approval_request(table, "s1", "read", "look at a file", 9, 2, 1000, 500) == WD_APPROVAL_PENDING);
  assert(wd_approval_lookup(table, "s1", 9, 1, 1000) == WD_APPROVAL_UNKNOWN);
  assert(wd_approval_answer(table, "s2", 9, 2, true, 1000) == WD_APPROVAL_UNKNOWN);

  assert(wd_approval_request(table, "s3", "grep", "search", 1, 1, 1000, 10) == WD_APPROVAL_PENDING);
  assert(wd_approval_lookup(table, "s3", 1, 1, 1010) == WD_APPROVAL_UNKNOWN);
  assert(wd_approval_answer(table, "s3", 1, 1, true, 1010) == WD_APPROVAL_UNKNOWN);
  assert(wd_approval_pending(table, 1010) == 1);

  assert(wd_approval_request(table, "s4", "write", "write", 2, 1, 2000, 500) == WD_APPROVAL_PENDING);
  assert(wd_approval_request(table, "s4", "write", "write", 2, 1, 2000, 500) == WD_APPROVAL_PENDING);
  assert(wd_approval_pending(table, 2000) == 1);

  wd_approvals_clear(table);
  assert(wd_approval_lookup(table, "s1", 7, 1, 1200) == WD_APPROVAL_UNKNOWN);
  wd_approvals_clear(table);
  assert(wd_approval_request(table, "s2", "edit", "change a file", 42, 1, 5000, 1000) ==
         WD_APPROVAL_PENDING);
  wd_approval_info listed[WD_MAX_APPROVALS];
  size_t listed_count = wd_approval_list(table, 5100, listed, WD_MAX_APPROVALS);
  assert(listed_count == 1);
  assert(strcmp(listed[0].session, "s2") == 0);
  assert(strcmp(listed[0].tool, "edit") == 0);
  assert(strcmp(listed[0].reason, "change a file") == 0);
  assert(listed[0].call == 42);
  assert(listed[0].expires_ms == 6000);
  assert(wd_approval_list(table, 6000, listed, WD_MAX_APPROVALS) == 0);
  wd_approvals_free(table);

  printf("approval tests passed\n");
  return 0;
}
