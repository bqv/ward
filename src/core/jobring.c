/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/jobring.h"

#include <stdlib.h>
#include <string.h>

struct wd_jobring {
  char *buf;
  size_t cap;
  size_t len;
  size_t head;
  uint64_t start;
  uint64_t dropped;
};

wd_jobring *wd_jobring_new(size_t cap) {
  if (cap == 0) return NULL;
  wd_jobring *ring = calloc(1, sizeof(*ring));
  if (ring == NULL) return NULL;
  ring->buf = malloc(cap);
  if (ring->buf == NULL) {
    free(ring);
    return NULL;
  }
  ring->cap = cap;
  return ring;
}

void wd_jobring_free(wd_jobring *ring) {
  if (ring == NULL) return;
  free(ring->buf);
  free(ring);
}

static void trim(wd_jobring *ring, size_t want) {
  if (ring->len + want <= ring->cap) return;
  size_t drop = ring->len + want - ring->cap;
  if (drop > ring->len) drop = ring->len;
  ring->head = (ring->head + drop) % ring->cap;
  ring->len -= drop;
  ring->start += drop;
  ring->dropped += drop;
}

void wd_jobring_append(wd_jobring *ring, const char *data, size_t len) {
  if (ring == NULL || data == NULL || len == 0) return;
  if (len >= ring->cap) {
    ring->dropped += len - ring->cap + ring->len;
    ring->start += ring->len + (len - ring->cap);
    ring->head = 0;
    ring->len = ring->cap;
    memcpy(ring->buf, data + (len - ring->cap), ring->cap);
    return;
  }
  trim(ring, len);
  size_t tail = (ring->head + ring->len) % ring->cap;
  size_t first = ring->cap - tail;
  if (first > len) first = len;
  memcpy(ring->buf + tail, data, first);
  if (len > first) memcpy(ring->buf, data + first, len - first);
  ring->len += len;
}

const char *wd_jobring_data(const wd_jobring *ring) {
  return ring != NULL ? ring->buf : NULL;
}

size_t wd_jobring_len(const wd_jobring *ring) {
  return ring != NULL ? ring->len : 0;
}

uint64_t wd_jobring_start(const wd_jobring *ring) {
  return ring != NULL ? ring->start : 0;
}

uint64_t wd_jobring_next(const wd_jobring *ring) {
  return ring != NULL ? ring->start + ring->len : 0;
}

uint64_t wd_jobring_dropped(const wd_jobring *ring) {
  return ring != NULL ? ring->dropped : 0;
}
