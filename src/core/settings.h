/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_SETTINGS_H
#define WARD_CORE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define WD_SETTINGS_DISABLED_MAX 16
#define WD_SETTINGS_NAME_MAX 32

typedef struct {
  long max_runs;
  bool debug;
  char disabled[WD_SETTINGS_DISABLED_MAX][WD_SETTINGS_NAME_MAX];
  size_t disabled_len;
} wd_settings;

void wd_settings_defaults(wd_settings *settings);
int wd_settings_load(const char *state_dir, wd_settings *settings);
int wd_settings_apply(wd_settings *settings, const char *key, const char *value,
                      char **err);
int wd_settings_save(const char *state_dir, const wd_settings *settings);
bool wd_settings_module_disabled(const wd_settings *settings, const char *name);
int wd_settings_set_module(wd_settings *settings, const char *name, bool enabled,
                           char **err);

#endif
