/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "core/judge.h"

typedef struct {
  int calls;
  int rc;
  wd_verdict verdict;
  char last_tool[64];
} stub;

static int ask_stub(void *ctx, const char *tool, const uint8_t *args,
                    size_t args_len, wd_verdict *out) {
  (void)args;
  (void)args_len;
  stub *s = ctx;
  s->calls++;
  snprintf(s->last_tool, sizeof(s->last_tool), "%s", tool);
  if (s->rc != 0) return s->rc;
  *out = s->verdict;
  return 0;
}

int main(void) {
  assert(wd_verdict_max(WD_VERDICT_ALLOW, WD_VERDICT_ASK) == WD_VERDICT_ASK);
  assert(wd_verdict_max(WD_VERDICT_ASK, WD_VERDICT_ALLOW) == WD_VERDICT_ASK);
  assert(wd_verdict_max(WD_VERDICT_DENY, WD_VERDICT_ASK) == WD_VERDICT_DENY);
  assert(wd_verdict_max(WD_VERDICT_ASK, WD_VERDICT_DENY) == WD_VERDICT_DENY);
  assert(strcmp(wd_verdict_name(WD_VERDICT_ASK), "ask") == 0);

  stub s = {0, 0, WD_VERDICT_ASK, {0}};
  wd_judge *judge = wd_judge_new(16);
  assert(judge != NULL);
  bool consulted = false;
  wd_verdict v = wd_judge_decide(judge, WD_VERDICT_ALLOW, "read",
                                 (const uint8_t *)"a.txt", 5, ask_stub, &s,
                                 &consulted);
  assert(v == WD_VERDICT_ASK && consulted && s.calls == 1);
  v = wd_judge_decide(judge, WD_VERDICT_ALLOW, "read", (const uint8_t *)"a.txt",
                      5, ask_stub, &s, &consulted);
  assert(v == WD_VERDICT_ASK && !consulted && s.calls == 1);
  assert(wd_judge_cached(judge) == 1);

  v = wd_judge_decide(judge, WD_VERDICT_ALLOW, "read",
                      (const uint8_t *)"b.txt", 5, ask_stub, &s, &consulted);
  assert(v == WD_VERDICT_ASK && consulted && s.calls == 2);

  v = wd_judge_decide(judge, WD_VERDICT_DENY, "read",
                      (const uint8_t *)"b.txt", 5, ask_stub, &s, &consulted);
  assert(v == WD_VERDICT_DENY && !consulted && s.calls == 2);

  s.rc = -1;
  v = wd_judge_decide(judge, WD_VERDICT_ALLOW, "write",
                      (const uint8_t *)"c.txt", 5, ask_stub, &s, &consulted);
  assert(v == WD_VERDICT_ALLOW && !consulted && s.calls == 3);

  s.rc = 0;
  s.verdict = WD_VERDICT_DENY;
  v = wd_judge_decide(judge, WD_VERDICT_ALLOW, "write",
                      (const uint8_t *)"c.txt", 5, ask_stub, &s, &consulted);
  assert(v == WD_VERDICT_DENY && consulted && s.calls == 4);

  wd_judge_free(judge);
  printf("judge tests passed\n");
  return 0;
}
