/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_INDEX_H
#define WARD_CORE_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "wasm/host.h"

int wd_index_build(const char *root, const char *cache_dir, wd_instance *code,
                   uint64_t *generation, char **err);
int wd_index_find(const char *cache_dir, const char *name, char *out,
                  size_t cap);

#endif
