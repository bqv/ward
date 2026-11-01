/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_MODSTATE_H
#define WARD_CORE_MODSTATE_H

#include <stddef.h>
#include <stdint.h>

char *wd_module_state_path(const char *state_dir, const char *session_id,
                           const char *name, const char *version);
int wd_module_state_write(const char *path, const uint8_t *data, size_t len);
int wd_module_state_read(const char *path, uint8_t **data, size_t *len);
int wd_module_state_find(const char *state_dir, const char *session_id,
                         const char *name, char *version_out, size_t cap);
int wd_module_state_forget(const char *state_dir, const char *session_id,
                           const char *name, const char *keep_version);

#endif
