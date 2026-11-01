/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/manifest.h"

static void write_file(const char *dir, const char *name, const char *body) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  assert(f != NULL);
  assert(fputs(body, f) >= 0);
  fclose(f);
}

int main(void) {
  char dir[] = "/tmp/ward-manifest-XXXXXX";
  assert(mkdtemp(dir) != NULL);

  write_file(dir, "skills.toml",
             "# a guest\nname = \"skills\"\nversion = \"2\"\nstate_schema = 2\n"
             "capabilities = [\"web.search\", \"mcp.call\"]\n");

  wd_manifest manifest;
  char *err = NULL;
  assert(wd_manifest_load(dir, "skills", &manifest, &err) == 0);
  assert(strcmp(manifest.name, "skills") == 0);
  assert(strcmp(manifest.version, "2") == 0);
  assert(manifest.state_schema == 2);
  assert(manifest.capabilities_len == 2);
  assert(strcmp(manifest.capabilities[0], "web.search") == 0);
  assert(strcmp(manifest.capabilities[1], "mcp.call") == 0);
  assert(!manifest.lazy);

  write_file(dir, "driver.toml", "name = \"driver\"\n");
  assert(wd_manifest_load(dir, "driver", &manifest, &err) == 0);
  assert(strcmp(manifest.version, "1") == 0);
  assert(manifest.state_schema == 1);
  assert(manifest.capabilities_len == 0);

  write_file(dir, "wrong.toml", "name = \"other\"\nversion = \"1\"\n");
  err = NULL;
  assert(wd_manifest_load(dir, "wrong", &manifest, &err) != 0);
  assert(err != NULL && strstr(err, "does not match") != NULL);
  free(err);

  err = NULL;
  assert(wd_manifest_load(dir, "missing", &manifest, &err) != 0);
  assert(err != NULL && strstr(err, "missing") != NULL);
  free(err);

  write_file(dir, "loud.toml", "name = \"loud\"\nversion = \"1\"\n"
                               "capabilities = [\"a\", \"b\", \"c\", \"d\", "
                               "\"e\", \"f\", \"g\", \"h\", \"i\", \"j\", "
                               "\"k\", \"l\", \"m\", \"n\", \"o\", \"p\", "
                               "\"q\"]\n");
  err = NULL;
  assert(wd_manifest_load(dir, "loud", &manifest, &err) != 0);
  assert(err != NULL && strstr(err, "too many") != NULL);
  free(err);

  assert(wd_manifest_compare_version("2", "10") < 0);
  assert(wd_manifest_compare_version("10", "2") > 0);
  assert(wd_manifest_compare_version("3", "3") == 0);

  printf("manifest tests passed\n");
  return 0;
}
