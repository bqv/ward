/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/presets.h"

int main(void) {
  assert(wd_preset_count() == 3);
  const wd_preset *def = wd_preset_find("default");
  assert(def != NULL && strcmp(def->name, "Ask writes") == 0);
  assert(strcmp(def->observe, "off") == 0);
  const wd_preset *reads = wd_preset_find("reads-only");
  assert(reads != NULL && strcmp(reads->write, "deny") == 0);
  const wd_preset *workspace = wd_preset_find("workspace");
  assert(workspace != NULL && strcmp(workspace->observe, "require-read") == 0);
  assert(wd_preset_find("nope") == NULL);

  assert(strcmp(wd_preset_clamp(reads, "write", "allow"), "deny") == 0);
  assert(strcmp(wd_preset_clamp(reads, "write", "ask"), "deny") == 0);
  assert(strcmp(wd_preset_clamp(def, "write", "allow"), "ask") == 0);
  assert(strcmp(wd_preset_clamp(def, "write", "deny"), "deny") == 0);
  assert(strcmp(wd_preset_clamp(def, "read", "allow"), "allow") == 0);
  assert(strcmp(wd_preset_clamp(def, "read", "deny"), "deny") == 0);
  assert(strcmp(wd_preset_clamp(def, "other", "allow"), "allow") == 0);

  char dir[] = "/tmp/ward-preset-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  assert(strcmp(wd_preset_current(dir)->id, "default") == 0);
  assert(wd_preset_select(dir, "reads-only", NULL) == 0);
  assert(strcmp(wd_preset_current(dir)->id, "reads-only") == 0);
  char *err = NULL;
  assert(wd_preset_select(dir, "bogus", &err) != 0);
  assert(err != NULL && strstr(err, "no such preset") != NULL);
  free(err);
  assert(strcmp(wd_preset_current(dir)->id, "reads-only") == 0);

  printf("presets tests passed\n");
  return 0;
}
