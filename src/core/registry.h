/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_REGISTRY_H
#define WARD_CORE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm/host.h"

#define WD_REGISTRY_MAX 32

typedef struct {
  char name[32];
  char version[16];
  uint64_t hash;
  size_t size;
  bool changed;
  bool valid;
  char error[128];
} wd_module_info;

typedef struct {
  wd_module_info items[WD_REGISTRY_MAX];
  size_t len;
} wd_registry;

void wd_registry_init(wd_registry *registry);
size_t wd_registry_scan(wd_registry *registry, const char *dir);
const wd_module_info *wd_registry_resolve(const wd_registry *registry,
                                          const char *name, const char *want);
const wd_module_info *wd_registry_find(const wd_registry *registry,
                                       const char *name, const char *version);
int wd_registry_validate(const char *dir, const char *name, const char *version,
                         char **err);
uint64_t wd_registry_hash(const uint8_t *bytes, size_t len);

#endif
