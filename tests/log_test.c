/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/log.h"
#include "core/util.h"

typedef struct {
  int count;
  char last[64];
} collector;

static int collect(void *ctx, const uint8_t *data, size_t len) {
  collector *c = ctx;
  c->count++;
  snprintf(c->last, sizeof(c->last), "%.*s", (int)(len > 63 ? 63 : len), data);
  return 0;
}

int main(void) {
  char dir[] = "/tmp/harness-log-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char *path = hu_path_join(dir, "events.zst");
  assert(path != NULL);

  hlog log;
  assert(hlog_create(&log, path) == 0);
  assert(hlog_append(&log, (const uint8_t *)"alpha", 5) == 0);
  assert(hlog_append(&log, (const uint8_t *)"bravo", 5) == 0);
  assert(hlog_append(&log, (const uint8_t *)"charlie", 7) == 0);
  hlog_close(&log);

  collector c = {0};
  bool truncated = false;
  assert(hlog_read(path, collect, &c, &truncated) == 3);
  assert(c.count == 3);
  assert(strcmp(c.last, "charlie") == 0);
  assert(!truncated);

  assert(hlog_open_append(&log, path) == 0);
  assert(hlog_append(&log, (const uint8_t *)"delta", 5) == 0);
  hlog_close(&log);
  c = (collector){0};
  assert(hlog_read(path, collect, &c, &truncated) == 4);
  assert(strcmp(c.last, "delta") == 0);

  struct stat sb;
  assert(stat(path, &sb) == 0);
  assert(ftruncate(open(path, O_WRONLY), sb.st_size - 2) == 0);
  c = (collector){0};
  truncated = false;
  assert(hlog_read(path, collect, &c, &truncated) == 3);
  assert(truncated);

  char missing[] = "/tmp/harness-log-missing/events.zst";
  assert(hlog_read(missing, collect, &c, &truncated) == -1);

  unlink(path);
  free(path);
  printf("log tests passed\n");
  return 0;
}
