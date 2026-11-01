/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/models.h"

static void write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(text, file) >= 0);
  fclose(file);
}

int main(void) {
  char dir[] = "/tmp/ward-models-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char *path = wd_models_path(dir);
  assert(path != NULL);
  assert(strstr(path, "models.toml") != NULL);

  char *err = NULL;
  wd_models *empty = wd_models_load(path, &err);
  assert(empty != NULL);
  assert(wd_models_count(empty) == 0);
  assert(wd_models_find(empty, "anything") == NULL);
  wd_models_free(empty);

  write_file(path,
             "# the local rung\n"
             "[[model]]\n"
             "id = \"local:qwen\"\n"
             "provider = \"stadi\"\n"
             "model = \"qwen3\"\n"
             "context = \"32768\"\n"
             "tags = \"local, fast\"\n"
             "price = \"0.04,2.0\"\n"
             "\n"
             "[[model]]\n"
             "id = \"cloud:flash\"\n"
             "provider = \"deepseek\"\n"
             "model = \"deepseek-v4-flash\"\n"
             "context = \"128000\"\n");
  wd_models *models = wd_models_load(path, &err);
  assert(models != NULL);
  assert(wd_models_count(models) == 2);
  const wd_model *first = wd_models_find(models, "local:qwen");
  assert(first != NULL);
  assert(strcmp(first->provider, "stadi") == 0);
  assert(strcmp(first->model, "qwen3") == 0);
  assert(strcmp(first->context, "32768") == 0);
  assert(strcmp(first->tags, "local, fast") == 0);
  assert(first->price != NULL && strcmp(first->price, "0.04,2.0") == 0);
  const wd_model *second = wd_models_at(models, 1);
  assert(second != NULL && strcmp(second->id, "cloud:flash") == 0);
  assert(second->tags == NULL);
  assert(wd_models_at(models, 2) == NULL);
  wd_models_free(models);

  write_file(path,
             "[[model]]\n"
             "id = \"a\"\n"
             "[[model]]\n"
             "id = \"a\"\n");
  assert(wd_models_load(path, &err) == NULL);
  assert(err != NULL);
  free(err);

  write_file(path, "[[model]]\nprovider = \"x\"\n");
  assert(wd_models_load(path, NULL) == NULL);

  write_file(path, "[[model]]\nid = \"a\"\n");
  models = wd_models_load(path, &err);
  assert(models != NULL);
  assert(wd_models_count(models) == 1);
  wd_models_free(models);

  free(path);
  char file[512];
  snprintf(file, sizeof(file), "%s/models.toml", dir);
  unlink(file);
  rmdir(dir);
  printf("models tests passed\n");
  return 0;
}
