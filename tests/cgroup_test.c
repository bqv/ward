/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/cgroup.h"

static int read_text(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return -1;
  size_t got = fread(out, 1, cap - 1, f);
  fclose(f);
  out[got] = '\0';
  return 0;
}

int main(void) {
  char base[] = "/sys/fs/cgroup/ward/cgroup-test-XXXXXX";
  if (mkdtemp(base) == NULL) {
    printf("cgroup tests skipped: no writable ward cgroup\n");
    return 0;
  }

  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    wd_cgroup_config config = {base, "harness", 4ll << 30, 4ll << 30, 128};
    char *err = NULL;
    int rc = wd_cgroup_enter(&config, &err);
    if (rc != 0) {
      fprintf(stderr, "child: %s\n", err != NULL ? err : "failed");
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  char path[512];
  char text[128];
  snprintf(path, sizeof(path), "%s/harness/memory.max", base);
  assert(read_text(path, text, sizeof(text)) == 0);
  assert(atoll(text) == 4ll << 30);
  snprintf(path, sizeof(path), "%s/harness/memory.high", base);
  assert(read_text(path, text, sizeof(text)) == 0);
  assert(atoll(text) == 4ll << 30);
  snprintf(path, sizeof(path), "%s/harness/pids.max", base);
  assert(read_text(path, text, sizeof(text)) == 0);
  assert(atoll(text) == 128);

  wd_cgroup_config off = {"", "harness", 0, 0, 0};
  char *err = NULL;
  assert(wd_cgroup_enter(&off, &err) == 0);

  printf("cgroup tests passed\n");
  return 0;
}
