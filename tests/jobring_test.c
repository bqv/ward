/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/jobring.h"

static void put(wd_jobring *ring, const char *text) {
  wd_jobring_append(ring, text, strlen(text));
}

int main(void) {
  wd_jobring *ring = wd_jobring_new(8);
  assert(ring != NULL);
  put(ring, "abc");
  assert(wd_jobring_len(ring) == 3);
  assert(wd_jobring_start(ring) == 0);
  assert(wd_jobring_next(ring) == 3);
  assert(wd_jobring_dropped(ring) == 0);

  put(ring, "def");
  assert(wd_jobring_len(ring) == 6);
  assert(memcmp(wd_jobring_data(ring), "abcdef", 6) == 0);

  put(ring, "ghij");
  assert(wd_jobring_len(ring) == 8);
  assert(wd_jobring_start(ring) == 2);
  assert(wd_jobring_dropped(ring) == 2);
  assert(wd_jobring_next(ring) == 10);

  put(ring, "KLMNOPQRST");
  assert(wd_jobring_len(ring) == 8);
  assert(wd_jobring_next(ring) == 20);
  assert(wd_jobring_dropped(ring) == 12);

  wd_jobring_free(ring);
  printf("jobring tests passed\n");
  return 0;
}
