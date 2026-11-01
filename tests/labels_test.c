/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/labels.h"

static void purge(const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/labels.db", dir);
  unlink(path);
  rmdir(dir);
}

int main(void) {
  char dir[] = "/tmp/ward-labels-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char *err = NULL;
  wd_labels *labels = wd_labels_open(dir, &err);
  assert(labels != NULL);

  const char *one = "0123456789abcdef";
  const char *two = "fedcba9876543210";
  assert(wd_labels_touch(labels, one) == 0);
  assert(wd_labels_touch(labels, one) == 0);
  wd_label label;
  assert(wd_labels_get(labels, one, &label) == 0);
  assert(label.name[0] == '\0');
  assert(!label.archived && !label.pinned);

  assert(wd_labels_set_name(labels, one, "first session") == 0);
  assert(wd_labels_get(labels, one, &label) == 0);
  assert(strcmp(label.name, "first session") == 0);
  assert(label.updated_ms > 0);

  assert(wd_labels_set_pinned(labels, one, true) == 0);
  assert(wd_labels_get(labels, one, &label) == 0);
  assert(label.pinned);
  assert(wd_labels_set_archived(labels, one, true) == 0);
  assert(wd_labels_get(labels, one, &label) == 0);
  assert(label.archived);

  assert(wd_labels_touch(labels, two) == 0);
  assert(wd_labels_set_name(labels, two, "second session") == 0);

  wd_label *rows = NULL;
  size_t count = 0;
  assert(wd_labels_all(labels, &rows, &count) == 0);
  assert(count == 2);
  free(rows);

  rows = NULL;
  count = 0;
  assert(wd_labels_search(labels, "first", 10, &rows, &count) == 0);
  assert(count == 1 && strcmp(rows[0].id, one) == 0);
  free(rows);

  rows = NULL;
  count = 0;
  assert(wd_labels_search(labels, "fedc", 10, &rows, &count) == 0);
  assert(count == 1 && strcmp(rows[0].id, two) == 0);
  free(rows);

  rows = NULL;
  count = 0;
  assert(wd_labels_search(labels, "nothing", 10, &rows, &count) == 0);
  assert(count == 0);
  free(rows);

  assert(wd_labels_get(labels, "not-a-session", &label) != 0);
  wd_labels_free(labels);

  labels = wd_labels_open(dir, &err);
  assert(labels != NULL);
  assert(wd_labels_get(labels, one, &label) == 0);
  assert(strcmp(label.name, "first session") == 0);
  assert(label.archived && label.pinned);
  wd_labels_free(labels);

  purge(dir);
  printf("labels tests passed\n");
  return 0;
}
