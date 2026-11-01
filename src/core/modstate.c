/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/modstate.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int safe_part(const char *part) {
  if (part == NULL || part[0] == '\0') return 0;
  if (strstr(part, "..") != NULL) return 0;
  return strchr(part, '/') == NULL;
}

static int state_dir_for(const char *state_dir, const char *session_id,
                         char *out, size_t cap) {
  if (state_dir == NULL || !safe_part(session_id)) return -1;
  int n = snprintf(out, cap, "%s/%s/modules", state_dir, session_id);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

char *wd_module_state_path(const char *state_dir, const char *session_id,
                           const char *name, const char *version) {
  if (!safe_part(name) || !safe_part(version)) return NULL;
  char dir[1024];
  if (state_dir_for(state_dir, session_id, dir, sizeof(dir)) != 0) return NULL;
  size_t len = strlen(dir) + strlen(name) + strlen(version) + 8;
  char *path = malloc(len);
  if (path == NULL) return NULL;
  snprintf(path, len, "%s/%s@%s.bin", dir, name, version);
  return path;
}

int wd_module_state_write(const char *path, const uint8_t *data, size_t len) {
  if (path == NULL || (data == NULL && len > 0)) return -1;
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash == NULL) return -1;
  *slash = '\0';
  if (mkdir(dir, 0700) != 0) {
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *up = strrchr(parent, '/');
    if (up == NULL) return -1;
    *up = '\0';
    if (mkdir(parent, 0700) != 0 && access(parent, F_OK) != 0) return -1;
    if (mkdir(dir, 0700) != 0 && access(dir, F_OK) != 0) return -1;
  }
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  size_t wrote = len > 0 ? fwrite(data, 1, len, f) : 0;
  int bad = len > 0 && wrote != len;
  if (fclose(f) != 0) bad = 1;
  if (bad) unlink(path);
  return bad ? -1 : 0;
}

int wd_module_state_read(const char *path, uint8_t **data, size_t *len) {
  if (path == NULL || data == NULL || len == NULL) return -1;
  *data = NULL;
  *len = 0;
  FILE *f = fopen(path, "rb");
  if (f == NULL) return 1;
  size_t cap = 4096, used = 0;
  uint8_t *bytes = malloc(cap);
  if (bytes == NULL) {
    fclose(f);
    return -1;
  }
  size_t n;
  while ((n = fread(bytes + used, 1, cap - used, f)) > 0) {
    used += n;
    if (used == cap) {
      uint8_t *grown = realloc(bytes, cap * 2);
      if (grown == NULL) {
        free(bytes);
        fclose(f);
        return -1;
      }
      bytes = grown;
      cap *= 2;
    }
  }
  fclose(f);
  *data = bytes;
  *len = used;
  return 0;
}

int wd_module_state_find(const char *state_dir, const char *session_id,
                         const char *name, char *version_out, size_t cap) {
  if (!safe_part(name) || version_out == NULL) return -1;
  char dir[1024];
  if (state_dir_for(state_dir, session_id, dir, sizeof(dir)) != 0) return -1;
  DIR *d = opendir(dir);
  if (d == NULL) return 1;
  size_t name_len = strlen(name);
  int found = 1;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    const char *file = entry->d_name;
    if (strncmp(file, name, name_len) != 0 || file[name_len] != '@') continue;
    const char *rest = file + name_len + 1;
    const char *dot = strstr(rest, ".bin");
    if (dot == NULL || dot == rest) continue;
    size_t vlen = (size_t)(dot - rest);
    if (vlen + 1 > cap) continue;
    memcpy(version_out, rest, vlen);
    version_out[vlen] = '\0';
    found = 0;
    break;
  }
  closedir(d);
  return found;
}

int wd_module_state_forget(const char *state_dir, const char *session_id,
                           const char *name, const char *keep_version) {
  if (!safe_part(name)) return -1;
  char dir[1024];
  if (state_dir_for(state_dir, session_id, dir, sizeof(dir)) != 0) return -1;
  DIR *d = opendir(dir);
  if (d == NULL) return 0;
  size_t name_len = strlen(name);
  int removed = 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    const char *file = entry->d_name;
    if (strncmp(file, name, name_len) != 0 || file[name_len] != '@') continue;
    const char *rest = file + name_len + 1;
    if (keep_version != NULL) {
      size_t vlen = strlen(keep_version);
      if (strncmp(rest, keep_version, vlen) == 0 && strcmp(rest + vlen, ".bin") == 0)
        continue;
    }
    char path[1500];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    if (unlink(path) == 0) removed++;
  }
  closedir(d);
  return removed;
}
