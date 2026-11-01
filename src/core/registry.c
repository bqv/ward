/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/registry.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/manifest.h"

uint64_t wd_registry_hash(const uint8_t *bytes, size_t len) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < len; i++) {
    h ^= bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

void wd_registry_init(wd_registry *registry) { memset(registry, 0, sizeof(*registry)); }

static uint8_t *read_bytes(const char *dir, const char *file, size_t *len) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, file);
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size <= 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  uint8_t *bytes = malloc((size_t)size);
  if (bytes == NULL) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(bytes, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) {
    free(bytes);
    return NULL;
  }
  *len = got;
  return bytes;
}

static bool is_wasm(const uint8_t *bytes, size_t len) {
  return len >= 8 && memcmp(bytes, "\0asm", 4) == 0 && bytes[4] == 1;
}

static void split_name(const char *stem, char *name, size_t name_cap,
                       char *version, size_t version_cap) {
  const char *at = strchr(stem, '@');
  if (at == NULL) {
    snprintf(name, name_cap, "%s", stem);
    snprintf(version, version_cap, "%s", "1");
    return;
  }
  size_t n = (size_t)(at - stem);
  if (n >= name_cap) n = name_cap - 1;
  memcpy(name, stem, n);
  name[n] = '\0';
  snprintf(version, version_cap, "%s", at + 1);
}

static wd_module_info *entry_for(wd_registry *registry, const char *name,
                                 const char *version) {
  for (size_t i = 0; i < registry->len; i++) {
    if (strcmp(registry->items[i].name, name) == 0 &&
        strcmp(registry->items[i].version, version) == 0)
      return &registry->items[i];
  }
  if (registry->len >= WD_REGISTRY_MAX) return NULL;
  wd_module_info *info = &registry->items[registry->len++];
  memset(info, 0, sizeof(*info));
  snprintf(info->name, sizeof(info->name), "%s", name);
  snprintf(info->version, sizeof(info->version), "%s", version);
  return info;
}

size_t wd_registry_scan(wd_registry *registry, const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) return 0;
  size_t changed = 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    size_t n = strlen(entry->d_name);
    if (n < 6 || strcmp(entry->d_name + n - 5, ".wasm") != 0) continue;
    char stem[64];
    if (n - 5 >= sizeof(stem)) continue;
    memcpy(stem, entry->d_name, n - 5);
    stem[n - 5] = '\0';
    size_t len = 0;
    uint8_t *bytes = read_bytes(dir, entry->d_name, &len);
    if (bytes == NULL) continue;
    char name[32];
    char version[16];
    split_name(stem, name, sizeof(name), version, sizeof(version));
    wd_module_info *info = entry_for(registry, name, version);
    if (info == NULL) {
      free(bytes);
      continue;
    }
    uint64_t hash = wd_registry_hash(bytes, len);
    bool wasm = is_wasm(bytes, len);
    free(bytes);
    bool fresh = info->size == 0 && info->hash == 0;
    if (fresh) {
      info->size = len;
      info->hash = hash;
    } else if (info->size != len || info->hash != hash) {
      info->size = len;
      info->hash = hash;
      info->changed = true;
      changed++;
    }
    info->valid = wasm;
    info->error[0] = '\0';
    if (!wasm) snprintf(info->error, sizeof(info->error), "not a wasm module");
  }
  closedir(d);
  return changed;
}

const wd_module_info *wd_registry_find(const wd_registry *registry,
                                       const char *name, const char *version) {
  for (size_t i = 0; i < registry->len; i++) {
    if (strcmp(registry->items[i].name, name) != 0) continue;
    if (version != NULL && strcmp(registry->items[i].version, version) != 0)
      continue;
    return &registry->items[i];
  }
  return NULL;
}

const wd_module_info *wd_registry_resolve(const wd_registry *registry,
                                          const char *name, const char *want) {
  const wd_module_info *best = NULL;
  for (size_t i = 0; i < registry->len; i++) {
    const wd_module_info *info = &registry->items[i];
    if (strcmp(info->name, name) != 0) continue;
    if (!info->valid) continue;
    if (want != NULL && strcmp(info->version, want) != 0) continue;
    if (best == NULL ||
        wd_manifest_compare_version(info->version, best->version) > 0)
      best = info;
  }
  return best;
}

int wd_registry_validate(const char *dir, const char *name, const char *version,
                         char **err) {
  char file[64];
  if (version != NULL && strcmp(version, "1") != 0)
    snprintf(file, sizeof(file), "%s@%s.wasm", name, version);
  else
    snprintf(file, sizeof(file), "%s.wasm", name);
  size_t len = 0;
  uint8_t *bytes = read_bytes(dir, file, &len);
  if (bytes == NULL) {
    if (err != NULL) *err = strdup("the module file cannot be read");
    return -1;
  }
  bool wasm = is_wasm(bytes, len);
  free(bytes);
  if (!wasm) {
    if (err != NULL) *err = strdup("the file is not a wasm module");
    return -1;
  }
  return 0;
}
