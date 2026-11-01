/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_CREDENTIALS_H
#define WARD_CORE_CREDENTIALS_H

#include <stddef.h>

typedef struct wd_credentials wd_credentials;

wd_credentials *wd_credentials_load(const char *path, char **err);
void wd_credentials_free(wd_credentials *credentials);
const char *wd_credentials_find(const wd_credentials *credentials,
                                const char *name);
char *wd_credentials_path(const char *state_dir);

#endif
