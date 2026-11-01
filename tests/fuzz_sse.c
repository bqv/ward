/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/sse.h"

static uint64_t rng_state = 0x2545f4914f6cdd1dull;

static uint64_t rnd(void) {
  uint64_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state = x;
  return x;
}

typedef struct {
  uint64_t events;
  uint64_t bytes;
  int abort_after;
} stats;

static int emit(void *ctx, const wd_sse_event *event) {
  stats *s = ctx;
  assert(event->type != NULL);
  assert(event->type_len > 0);
  assert(event->type_len <= WD_SSE_MAX_TYPE);
  assert(event->data != NULL);
  assert(event->data_len <= WD_SSE_MAX_DATA);
  assert(event->id_len <= WD_SSE_MAX_ID);
  if (event->id != NULL) assert(event->id_len > 0);
  s->events++;
  s->bytes += event->data_len;
  return s->abort_after >= 0 && (int64_t)s->events > s->abort_after ? 1 : 0;
}

static void chew(const uint8_t *buf, size_t len) {
  stats s = {0, 0, -1};
  wd_sse *sse = wd_sse_create(emit, &s);
  assert(sse != NULL);
  size_t i = 0;
  while (i < len) {
    size_t n = (size_t)(rnd() % 512) + 1;
    if (n > len - i) n = len - i;
    int rc = wd_sse_feed(sse, (const char *)buf + i, n);
    i += n;
    if (rc != 0) {
      assert(wd_sse_failed(sse));
      assert(wd_sse_feed(sse, "data: x\n\n", 9) == -1);
      break;
    }
  }
  wd_sse_finish(sse);
  wd_sse_destroy(sse);

  s.abort_after = 1;
  sse = wd_sse_create(emit, &s);
  assert(sse != NULL);
  for (size_t k = 0; k < len && !wd_sse_failed(sse); k++)
    wd_sse_feed(sse, (const char *)(buf + k), 1);
  wd_sse_destroy(sse);
}

static void structured(void) {
  static const char alphabet[] = "data:event\n\r :id test\0";
  char buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++)
    buf[i] = rnd() % 4 == 0 ? alphabet[rnd() % (sizeof(alphabet) - 1)] : (char)(rnd() & 0x7f);
  chew((const uint8_t *)buf, sizeof(buf));
}

static void random_bytes(void) {
  uint8_t buf[2048];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)rnd();
  chew(buf, sizeof(buf));
}

static void boundary_sizes(void) {
  static const size_t sizes[] = {0, 1, 2, 3, 4, 7, 22, 63, 64, 65, 255, 1023, 4096};
  uint8_t *buf = malloc(WD_SSE_MAX_LINE + 16);
  assert(buf != NULL);
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t n = sizes[i];
    for (size_t k = 0; k < n; k++) buf[k] = (uint8_t)("data:"[k % 5]);
    chew(buf, n);
  }
  memset(buf, 'd', WD_SSE_MAX_LINE + 16);
  chew(buf, WD_SSE_MAX_LINE + 16);
  free(buf);
}

int main(void) {
  for (int round = 0; round < 64; round++) {
    structured();
    random_bytes();
    boundary_sizes();
  }
  printf("sse fuzz passed\n");
  return 0;
}
