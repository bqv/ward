/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_SETTINGS_H
#define WARD_CORE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  long max_runs;
  bool debug;
} wd_settings;

void wd_settings_defaults(wd_settings *settings);
int wd_settings_load(const char *state_dir, wd_settings *settings);
int wd_settings_apply(wd_settings *settings, const char *key, const char *value,
                      char **err);
int wd_settings_save(const char *state_dir, const wd_settings *settings);

#endif
