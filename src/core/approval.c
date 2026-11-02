/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/approval.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  bool used;
  char session[WD_APPROVAL_SESSION_MAX];
  uint64_t call;
  uint64_t epoch;
  uint64_t expires_ms;
  wd_approval_state state;
} entry;

struct wd_approvals {
  entry entries[WD_MAX_APPROVALS];
  size_t next;
};

wd_approvals *wd_approvals_new(void) {
  return calloc(1, sizeof(wd_approvals));
}

void wd_approvals_free(wd_approvals *table) { free(table); }

void wd_approvals_clear(wd_approvals *table) {
  memset(table->entries, 0, sizeof(table->entries));
  table->next = 0;
}

static bool expired(const entry *e, uint64_t now_ms) {
  return !e->used || e->state != WD_APPROVAL_PENDING || now_ms >= e->expires_ms;
}

static entry *find(wd_approvals *table, const char *session, uint64_t call,
                   uint64_t epoch, uint64_t now_ms) {
  for (size_t i = 0; i < WD_MAX_APPROVALS; i++) {
    entry *e = &table->entries[i];
    if (!e->used) continue;
    if (e->call != call || e->epoch != epoch) continue;
    if (strcmp(e->session, session) != 0) continue;
    if (expired(e, now_ms)) continue;
    return e;
  }
  return NULL;
}

wd_approval_state wd_approval_request(wd_approvals *table, const char *session,
                                      uint64_t call, uint64_t epoch,
                                      uint64_t now_ms, uint64_t ttl_ms) {
  if (session == NULL || strlen(session) >= WD_APPROVAL_SESSION_MAX)
    return WD_APPROVAL_UNKNOWN;
  if (find(table, session, call, epoch, now_ms) != NULL)
    return WD_APPROVAL_PENDING;
  entry *slot = NULL;
  for (size_t i = 0; i < WD_MAX_APPROVALS && slot == NULL; i++) {
    entry *e = &table->entries[(table->next + i) % WD_MAX_APPROVALS];
    if (e->call == call && e->epoch == epoch &&
        strcmp(e->session, session) == 0 &&
        e->state != WD_APPROVAL_PENDING) {
      slot = e;
      break;
    }
  }
  for (size_t i = 0; i < WD_MAX_APPROVALS && slot == NULL; i++) {
    entry *e = &table->entries[(table->next + i) % WD_MAX_APPROVALS];
    if (!e->used || expired(e, now_ms)) slot = e;
  }
  if (slot == NULL) return WD_APPROVAL_UNKNOWN;
  memset(slot, 0, sizeof(*slot));
  slot->used = true;
  snprintf(slot->session, sizeof(slot->session), "%s", session);
  slot->call = call;
  slot->epoch = epoch;
  slot->expires_ms = now_ms + ttl_ms;
  slot->state = WD_APPROVAL_PENDING;
  table->next = (size_t)(slot - table->entries) + 1;
  if (table->next >= WD_MAX_APPROVALS) table->next = 0;
  return WD_APPROVAL_PENDING;
}

wd_approval_state wd_approval_answer(wd_approvals *table, const char *session,
                                     uint64_t call, uint64_t epoch, bool allow,
                                     uint64_t now_ms) {
  if (session == NULL) return WD_APPROVAL_UNKNOWN;
  entry *e = find(table, session, call, epoch, now_ms);
  if (e == NULL) {
    for (size_t i = 0; i < WD_MAX_APPROVALS; i++) {
      entry *old = &table->entries[i];
      if (old->used && old->call == call && old->epoch == epoch &&
          strcmp(old->session, session) == 0 &&
          old->state != WD_APPROVAL_PENDING)
        return WD_APPROVAL_ALREADY_RESOLVED;
    }
    return WD_APPROVAL_UNKNOWN;
  }
  e->state = allow ? WD_APPROVAL_GRANTED : WD_APPROVAL_DENIED;
  return e->state;
}

wd_approval_state wd_approval_lookup(const wd_approvals *table,
                                     const char *session, uint64_t call,
                                     uint64_t epoch, uint64_t now_ms) {
  if (session == NULL) return WD_APPROVAL_UNKNOWN;
  for (size_t i = 0; i < WD_MAX_APPROVALS; i++) {
    const entry *e = &table->entries[i];
    if (!e->used) continue;
    if (e->call != call || e->epoch != epoch) continue;
    if (strcmp(e->session, session) != 0) continue;
    if (e->state == WD_APPROVAL_PENDING && now_ms >= e->expires_ms)
      return WD_APPROVAL_UNKNOWN;
    return e->state;
  }
  return WD_APPROVAL_UNKNOWN;
}

size_t wd_approval_list(const wd_approvals *table, uint64_t now_ms,
                        wd_approval_info *out, size_t cap) {
  if (table == NULL || out == NULL) return 0;
  size_t count = 0;
  for (size_t i = 0; i < WD_MAX_APPROVALS && count < cap; i++) {
    const entry *e = &table->entries[i];
    if (expired(e, now_ms)) continue;
    snprintf(out[count].session, sizeof(out[count].session), "%s", e->session);
    out[count].call = e->call;
    out[count].epoch = e->epoch;
    out[count].expires_ms = e->expires_ms;
    count++;
  }
  return count;
}

size_t wd_approval_pending(const wd_approvals *table, uint64_t now_ms) {
  size_t n = 0;
  for (size_t i = 0; i < WD_MAX_APPROVALS; i++) {
    const entry *e = &table->entries[i];
    if (e->used && e->state == WD_APPROVAL_PENDING && now_ms < e->expires_ms) n++;
  }
  return n;
}
