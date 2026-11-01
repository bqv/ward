/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/judge.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  uint64_t key;
  wd_verdict verdict;
  bool used;
} judge_entry;

struct wd_judge {
  judge_entry *entries;
  size_t capacity;
  size_t len;
};

wd_verdict wd_verdict_max(wd_verdict rule, wd_verdict judge) {
  return rule >= judge ? rule : judge;
}

const char *wd_verdict_name(wd_verdict verdict) {
  switch (verdict) {
  case WD_VERDICT_ALLOW:
    return "allow";
  case WD_VERDICT_ASK:
    return "ask";
  default:
    return "deny";
  }
}

static uint64_t hash_bytes(uint64_t seed, const void *data, size_t len) {
  const uint8_t *bytes = data;
  uint64_t h = seed;
  for (size_t i = 0; i < len; i++) {
    h ^= bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t cache_key(const char *tool, const uint8_t *args,
                          size_t args_len) {
  uint64_t h = 1469598103934665603ull;
  h = hash_bytes(h, tool != NULL ? tool : "", tool != NULL ? strlen(tool) : 0);
  h = hash_bytes(h, "\0", 1);
  return hash_bytes(h, args, args_len);
}

wd_judge *wd_judge_new(size_t capacity) {
  wd_judge *judge = calloc(1, sizeof(*judge));
  if (judge == NULL) return NULL;
  judge->capacity = capacity == 0 ? 64 : capacity;
  judge->entries = calloc(judge->capacity, sizeof(*judge->entries));
  if (judge->entries == NULL) {
    free(judge);
    return NULL;
  }
  return judge;
}

void wd_judge_free(wd_judge *judge) {
  if (judge == NULL) return;
  free(judge->entries);
  free(judge);
}

size_t wd_judge_cached(const wd_judge *judge) {
  return judge != NULL ? judge->len : 0;
}

static judge_entry *cache_find(wd_judge *judge, uint64_t key) {
  if (judge->capacity == 0) return NULL;
  size_t slot = (size_t)(key % judge->capacity);
  for (size_t i = 0; i < judge->capacity; i++) {
    judge_entry *entry = &judge->entries[(slot + i) % judge->capacity];
    if (!entry->used) return entry;
    if (entry->key == key) return entry;
  }
  return NULL;
}

wd_verdict wd_judge_decide(wd_judge *judge, wd_verdict rule, const char *tool,
                           const uint8_t *args, size_t args_len,
                           wd_judge_ask ask, void *ctx, bool *consulted) {
  if (consulted != NULL) *consulted = false;
  if (judge == NULL || ask == NULL || rule != WD_VERDICT_ALLOW) return rule;
  uint64_t key = cache_key(tool, args, args_len);
  judge_entry *entry = cache_find(judge, key);
  if (entry != NULL && entry->used && entry->key == key)
    return wd_verdict_max(rule, entry->verdict);
  wd_verdict verdict = rule;
  if (ask(ctx, tool, args, args_len, &verdict) != 0) return rule;
  if (verdict < WD_VERDICT_ALLOW || verdict > WD_VERDICT_DENY) return rule;
  if (consulted != NULL) *consulted = true;
  if (entry == NULL) return wd_verdict_max(rule, verdict);
  if (!entry->used) judge->len++;
  entry->key = key;
  entry->verdict = verdict;
  entry->used = true;
  return wd_verdict_max(rule, verdict);
}
