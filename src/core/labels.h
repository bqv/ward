/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_LABELS_H
#define WARD_CORE_LABELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ids.h"

#define WD_LABEL_NAME 128

typedef struct {
  char id[WD_ID_LEN];
  char name[WD_LABEL_NAME];
  bool archived;
  bool pinned;
  uint64_t updated_ms;
} wd_label;

typedef struct wd_labels wd_labels;

wd_labels *wd_labels_open(const char *dir, char **err);
void wd_labels_free(wd_labels *labels);

int wd_labels_touch(wd_labels *labels, const char *id);
int wd_labels_set_name(wd_labels *labels, const char *id, const char *name);
int wd_labels_set_archived(wd_labels *labels, const char *id, bool archived);
int wd_labels_set_pinned(wd_labels *labels, const char *id, bool pinned);
int wd_labels_get(wd_labels *labels, const char *id, wd_label *out);
int wd_labels_all(wd_labels *labels, wd_label **out, size_t *count);
int wd_labels_search(wd_labels *labels, const char *query, size_t limit,
                     wd_label **out, size_t *count);

#endif
