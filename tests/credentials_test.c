/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/credentials.h"

int main(void) {
  char dir[] = "/tmp/ward-credentials-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char path[256];
  snprintf(path, sizeof(path), "%s/credentials", dir);
  FILE *f = fopen(path, "w");
  assert(f != NULL);
  fputs("# name=value pairs\n", f);
  fputs("deepseek=sk-deep\n", f);
  fputs("other = \"sk-other\"\n", f);
  fputs("\n", f);
  fputs("broken line without equals\n", f);
  fclose(f);

  char *err = NULL;
  wd_credentials *credentials = wd_credentials_load(path, &err);
  assert(credentials != NULL);
  assert(strcmp(wd_credentials_find(credentials, "deepseek"), "sk-deep") == 0);
  assert(strcmp(wd_credentials_find(credentials, "other"), "sk-other") == 0);
  assert(wd_credentials_find(credentials, "missing") == NULL);
  assert(wd_credentials_find(credentials, NULL) == NULL);
  wd_credentials_free(credentials);

  wd_credentials *empty = wd_credentials_load(NULL, &err);
  assert(empty != NULL);
  assert(wd_credentials_find(empty, "deepseek") == NULL);
  wd_credentials_free(empty);

  char *missing = wd_credentials_path(dir);
  assert(missing != NULL);
  assert(strstr(missing, "credentials") != NULL);
  free(missing);

  printf("credentials tests passed\n");
  return 0;
}
