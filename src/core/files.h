/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_FILES_H
#define WARD_CORE_FILES_H

#include <stddef.h>

int wd_file_read(const char *root, const char *path, size_t max, char **out,
                 size_t *len, char **err);

#endif
