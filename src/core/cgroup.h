/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_CGROUP_H
#define WARD_CORE_CGROUP_H

typedef struct {
  const char *base;
  const char *leaf;
  long long high_bytes;
  long long max_bytes;
  long long pids_max;
} wd_cgroup_config;

int wd_cgroup_enter(const wd_cgroup_config *config, char **err);
void wd_cgroup_leave(void);

#endif
