/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/registry.h"
#include "core/turn.h"

static int emit(void *ctx, const char *event, cbor_item_t *payload, char **err) {
  (void)ctx;
  (void)event;
  (void)payload;
  (void)err;
  return 0;
}

static uint8_t *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  size_t cap = 1 << 16, used = 0;
  uint8_t *bytes = malloc(cap);
  size_t n;
  while (bytes != NULL && (n = fread(bytes + used, 1, cap - used, f)) > 0) {
    used += n;
    if (used == cap) {
      uint8_t *grown = realloc(bytes, cap * 2);
      if (grown == NULL) {
        free(bytes);
        return NULL;
      }
      bytes = grown;
      cap *= 2;
    }
  }
  fclose(f);
  *len = used;
  return bytes;
}

static void write_all(const char *path, const uint8_t *bytes, size_t len) {
  FILE *f = fopen(path, "wb");
  assert(f != NULL);
  assert(fwrite(bytes, 1, len, f) == len);
  fclose(f);
}

int main(void) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  char trap_path[1024];
  snprintf(trap_path, sizeof(trap_path), "%s/trap.wasm", dir);
  size_t trap_len = 0;
  uint8_t *trap = read_all(trap_path, &trap_len);
  assert(trap != NULL);

  char copy[] = "/tmp/ward-trap-XXXXXX";
  assert(mkdtemp(copy) != NULL);
  DIR *source = opendir(dir);
  assert(source != NULL);
  struct dirent *entry;
  while ((entry = readdir(source)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    if (strstr(entry->d_name, "trap") != NULL) continue;
    char from[1024], to[1024];
    snprintf(from, sizeof(from), "%s/%s", dir, entry->d_name);
    snprintf(to, sizeof(to), "%s/%s", copy, entry->d_name);
    size_t len = 0;
    uint8_t *bytes = read_all(from, &len);
    if (bytes == NULL) continue;
    write_all(to, bytes, len);
    free(bytes);
  }
  closedir(source);

  char driver_path[1024];
  snprintf(driver_path, sizeof(driver_path), "%s/driver.wasm", copy);
  write_all(driver_path, trap, trap_len);

  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  char root[] = "/tmp/ward-trap-root-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char *err = NULL;
  wd_turn *turn = wd_turn_new(engine, copy, root, &err);
  assert(turn != NULL);
  int rc = wd_turn_prompt(turn, "hello", emit, NULL, &err);
  assert(rc != WD_TURN_OK);
  char trapped[64];
  assert(wd_turn_trapped(turn, trapped, sizeof(trapped)));
  assert(strcmp(trapped, "driver") == 0);

  wd_registry registry;
  wd_registry_init(&registry);
  (void)wd_registry_scan(&registry, copy);
  const wd_module_info *driver = wd_registry_resolve(&registry, "driver", NULL);
  assert(driver != NULL);
  wd_registry_trap(&registry, "driver", driver->version);
  assert(wd_registry_resolve(&registry, "driver", NULL) != NULL);
  wd_registry_trap(&registry, "driver", driver->version);
  assert(wd_registry_resolve(&registry, "driver", NULL) == NULL);

  wd_turn_free(turn);
  wd_engine_free(engine);
  free(err);
  free(trap);
  printf("trap tests passed\n");
  return 0;
}
