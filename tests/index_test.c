/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/index.h"
#include "wasm/host.h"

static const wd_limits LIMITS = {.fuel = 50000000, .max_memory = 16 << 20,
                                 .max_instances = 2};

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f != NULL);
  fputs(text, f);
  fclose(f);
}

int main(void) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  char path[1024];
  snprintf(path, sizeof(path), "%s/code.wasm", dir);
  FILE *f = fopen(path, "rb");
  assert(f != NULL);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *bytes = malloc((size_t)size);
  assert(bytes != NULL);
  assert(fread(bytes, 1, (size_t)size, f) == (size_t)size);
  fclose(f);

  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  static const char *const no_caps[] = {NULL};
  wd_module_spec spec = {"code", no_caps, 0, true};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, size, &err);
  assert(module != NULL);
  wd_instance *instance =
      wd_instance_new(module, no_caps, 0, &LIMITS, NULL, NULL, &err);
  assert(instance != NULL);

  char root[] = "/tmp/ward-index-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char cache[512];
  snprintf(cache, sizeof(cache), "%s/cache", root);
  assert(mkdir(cache, 0700) == 0);
  char a[512], b[512];
  snprintf(a, sizeof(a), "%s/one.c", root);
  snprintf(b, sizeof(b), "%s/two.h", root);
  write_file(a, "int main(void) {\n  return 0;\n}\n");
  write_file(b, "static void helper(int x) {\n}\n");

  uint64_t generation = 0;
  assert(wd_index_build(root, cache, instance, &generation, &err) == 0);
  assert(generation != 0);

  char out[512];
  assert(wd_index_find(cache, "helper", out, sizeof(out)) == 1);
  assert(strstr(out, "two.h:1 helper") != NULL);
  assert(wd_index_find(cache, "main", out, sizeof(out)) == 1);
  assert(strstr(out, "one.c:1") != NULL);
  assert(wd_index_find(cache, "nothing", out, sizeof(out)) == 0);

  uint64_t again = 0;
  assert(wd_index_build(root, cache, instance, &again, &err) == 0);
  assert(again == generation);

  wd_instance_free(instance);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("index tests passed\n");
  return 0;
}
