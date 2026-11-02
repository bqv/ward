/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_JOBRING_H
#define WARD_CORE_JOBRING_H

#include <stddef.h>
#include <stdint.h>

typedef struct wd_jobring wd_jobring;

wd_jobring *wd_jobring_new(size_t cap);
void wd_jobring_free(wd_jobring *ring);

void wd_jobring_append(wd_jobring *ring, const char *data, size_t len);
const char *wd_jobring_data(const wd_jobring *ring);
size_t wd_jobring_len(const wd_jobring *ring);
uint64_t wd_jobring_start(const wd_jobring *ring);
uint64_t wd_jobring_next(const wd_jobring *ring);
uint64_t wd_jobring_dropped(const wd_jobring *ring);

#endif
