/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_PRESETS_H
#define WARD_CORE_PRESETS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *id;
  const char *name;
  const char *description;
  const char *read;
  const char *write;
  const char *edit;
  const char *exec;
  const char *grep;
  const char *observe;
  bool dangerous;
  const char *warning;
} wd_preset;

size_t wd_preset_count(void);
const wd_preset *wd_preset_at(size_t index);
const wd_preset *wd_preset_find(const char *id);
const wd_preset *wd_preset_current(const char *state_dir);
int wd_preset_select(const char *state_dir, const char *id, bool confirmed,
                     char **err);
const char *wd_preset_clamp(const wd_preset *preset, const char *tool,
                            const char *decision);

#endif
