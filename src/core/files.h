/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_FILES_H
#define WARD_CORE_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum wd_file_kind { WD_FILE_REGULAR = 0, WD_FILE_DIR = 1, WD_FILE_OTHER = 2 };

typedef struct {
  char *name;
  int kind;
  uint64_t size;
} wd_file_entry;

int wd_file_list(const char *root, const char *path, size_t max,
                 wd_file_entry **out, size_t *len, bool *truncated, char **err);
void wd_file_entries_free(wd_file_entry *entries, size_t len);

int wd_file_read(const char *root, const char *path, size_t max, char **out,
                 size_t *len, char **err);
int wd_file_read_range(const char *root, const char *path, size_t offset,
                       size_t want, size_t max_total, char **out, size_t *len,
                       char **err);

#endif
