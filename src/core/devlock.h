/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_DEVLOCK_H
#define WARD_CORE_DEVLOCK_H

typedef struct {
  int fd;
} wd_device_lock;

int wd_device_acquire(const char *state_dir, const char *device, int slots,
                      wd_device_lock *out);
void wd_device_release(wd_device_lock *lock);

#endif
