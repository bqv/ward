/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/devlock.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

int wd_device_acquire(const char *state_dir, const char *device, int slots,
                      wd_device_lock *out) {
  if (out == NULL) return -1;
  out->fd = -1;
  if (state_dir == NULL || device == NULL || device[0] == '\0') return 0;
  if (slots <= 0) slots = 1;
  for (int slot = 0; slot < slots; slot++) {
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/device-%s-%d.lock",
                           state_dir, device, slot);
    if (written <= 0 || (size_t)written >= sizeof(path)) return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      out->fd = fd;
      return 0;
    }
    close(fd);
  }
  return 1;
}

void wd_device_release(wd_device_lock *lock) {
  if (lock == NULL || lock->fd < 0) return;
  flock(lock->fd, LOCK_UN);
  close(lock->fd);
  lock->fd = -1;
}
