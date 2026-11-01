/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_MANIFEST_H
#define WARD_CORE_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_MANIFEST_NAME 32
#define WD_MANIFEST_VERSION 16
#define WD_MANIFEST_CAPS 16

typedef struct {
  char name[WD_MANIFEST_NAME];
  char version[WD_MANIFEST_VERSION];
  uint32_t state_schema;
  char capabilities[WD_MANIFEST_CAPS][WD_MANIFEST_NAME];
  size_t capabilities_len;
  bool lazy;
} wd_manifest;

int wd_manifest_load(const char *dir, const char *name, wd_manifest *out,
                     char **err);
int wd_manifest_compare_version(const char *a, const char *b);

#endif
