/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/registry.h"

static void copy_module(const char *src_dir, const char *src, const char *dir,
                        const char *dst) {
  char from[1024];
  snprintf(from, sizeof(from), "%s/%s", src_dir, src);
  FILE *in = fopen(from, "rb");
  assert(in != NULL);
  char to[1024];
  snprintf(to, sizeof(to), "%s/%s", dir, dst);
  FILE *out = fopen(to, "wb");
  assert(out != NULL);
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    assert(fwrite(buf, 1, n, out) == n);
  fclose(in);
  fclose(out);
}

static void write_text(const char *dir, const char *name, const char *body) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "wb");
  assert(f != NULL);
  assert(fputs(body, f) >= 0);
  fclose(f);
}

int main(void) {
  const char *module_dir = getenv("WARD_MODULE_DIR");
  assert(module_dir != NULL);
  char dir[] = "/tmp/ward-registry-XXXXXX";
  assert(mkdtemp(dir) != NULL);

  copy_module(module_dir, "echo.wasm", dir, "echo.wasm");
  copy_module(module_dir, "echo.wasm", dir, "echo@2.wasm");

  wd_registry registry;
  wd_registry_init(&registry);
  assert(wd_registry_scan(&registry, dir) == 0);
  assert(registry.len == 2);

  const wd_module_info *latest = wd_registry_resolve(&registry, "echo", NULL);
  assert(latest != NULL && strcmp(latest->version, "2") == 0);
  const wd_module_info *first = wd_registry_resolve(&registry, "echo", "1");
  assert(first != NULL && strcmp(first->version, "1") == 0);
  assert(wd_registry_resolve(&registry, "echo", "9") == NULL);
  assert(wd_registry_resolve(&registry, "absent", NULL) == NULL);

  wd_registry again;
  wd_registry_init(&again);
  assert(wd_registry_scan(&again, dir) == 0);
  assert(!again.items[0].changed);
  assert(!again.items[1].changed);

  write_text(dir, "echo@2.wasm", "not a wasm module");
  assert(wd_registry_scan(&registry, dir) == 1);
  const wd_module_info *broken = wd_registry_find(&registry, "echo", "2");
  assert(broken != NULL && !broken->valid);
  assert(strstr(broken->error, "not a wasm") != NULL);
  latest = wd_registry_resolve(&registry, "echo", NULL);
  assert(latest != NULL && strcmp(latest->version, "1") == 0);

  write_text(dir, "echo@2.wasm", "");
  {
    char from[1024];
    snprintf(from, sizeof(from), "%s/echo.wasm", module_dir);
    char to[1024];
    snprintf(to, sizeof(to), "%s/echo@2.wasm", dir);
    FILE *in = fopen(from, "rb");
    assert(in != NULL);
    FILE *out = fopen(to, "wb");
    assert(out != NULL);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      assert(fwrite(buf, 1, n, out) == n);
    fclose(in);
    fclose(out);
  }
  wd_registry fresh;
  wd_registry_init(&fresh);
  (void)wd_registry_scan(&fresh, dir);
  wd_registry_trap(&fresh, "echo", "2");
  latest = wd_registry_resolve(&fresh, "echo", NULL);
  assert(latest != NULL && strcmp(latest->version, "2") == 0);
  wd_registry_trap(&fresh, "echo", "2");
  latest = wd_registry_resolve(&fresh, "echo", NULL);
  assert(latest != NULL && strcmp(latest->version, "1") == 0);
  wd_registry_clear(&fresh, "echo", "2");
  latest = wd_registry_resolve(&fresh, "echo", NULL);
  assert(latest != NULL && strcmp(latest->version, "2") == 0);
  write_text(dir, "echo@2.wasm", "not a wasm module");
  (void)wd_registry_scan(&registry, dir);

  char *err = NULL;
  assert(wd_registry_validate(dir, "echo", "2", &err) != 0);
  assert(err != NULL && strstr(err, "not a wasm") != NULL);
  free(err);
  err = NULL;
  assert(wd_registry_validate(dir, "echo", "1", &err) == 0);
  assert(err == NULL);

  printf("registry tests passed\n");
  return 0;
}
