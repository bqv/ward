/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/devlock.h"

int main(void) {
  char dir[] = "/tmp/ward-devlock-XXXXXX";
  assert(mkdtemp(dir) != NULL);

  wd_device_lock first;
  assert(wd_device_acquire(dir, "gpu", 1, &first) == 0);

  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    wd_device_lock other;
    int rc = wd_device_acquire(dir, "gpu", 1, &other);
    if (rc == 0) wd_device_release(&other);
    _exit(rc == 1 ? 0 : 1);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  wd_device_lock second;
  assert(wd_device_acquire(dir, "gpu", 2, &second) == 0);
  wd_device_release(&second);
  wd_device_release(&first);

  wd_device_lock third;
  assert(wd_device_acquire(dir, "gpu", 1, &third) == 0);
  wd_device_release(&third);

  wd_device_lock none;
  assert(wd_device_acquire(dir, "", 1, &none) == 0);
  wd_device_release(&none);

  printf("devlock tests passed\n");
  return 0;
}
