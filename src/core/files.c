/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/files.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int inside_root(const char *root, const char *path) {
  char real_root[PATH_MAX];
  char real_path[PATH_MAX];
  if (realpath(root, real_root) == NULL) return 0;
  if (realpath(path, real_path) == NULL) return 0;
  size_t len = strlen(real_root);
  if (strncmp(real_path, real_root, len) != 0) return 0;
  return real_path[len] == '\0' || real_path[len] == '/';
}

static int resolve(const char *root, const char *path, char *full, char **err) {
  if (path == NULL || path[0] == '\0') {
    if (err != NULL) *err = strdup("file.read: the path is empty");
    return -1;
  }
  if (path[0] == '/') {
    snprintf(full, PATH_MAX, "%s", path);
  } else {
    snprintf(full, PATH_MAX, "%s/%s", root != NULL ? root : ".", path);
  }
  if (!inside_root(root != NULL ? root : ".", full)) {
    if (err != NULL)
      *err = strdup("file.read: the path is outside the session directory");
    return -1;
  }
  struct stat st;
  if (stat(full, &st) != 0) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.read: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    if (err != NULL) *err = strdup("file.read: not a regular file");
    return -1;
  }
  return 0;
}

int wd_file_read_range(const char *root, const char *path, size_t offset,
                       size_t want, size_t max_total, char **out, size_t *len,
                       char **err) {
  if (out == NULL || len == NULL) return -1;
  *out = NULL;
  *len = 0;
  if (want == 0) {
    if (err != NULL) *err = strdup("file.read: len is required");
    return -1;
  }
  if (offset + want > max_total) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "file.read: offset %zu plus len %zu exceeds the %zu byte total",
               offset, want, max_total);
      *err = strdup(buf);
    }
    return -1;
  }
  char full[PATH_MAX];
  if (resolve(root, path, full, err) != 0) return -1;
  int fd = open(full, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.read: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    close(fd);
    if (err != NULL) *err = strdup("file.read: cannot seek");
    return -1;
  }
  char *buffer = malloc(want);
  if (buffer == NULL) {
    close(fd);
    if (err != NULL) *err = strdup("file.read: out of memory");
    return -1;
  }
  size_t used = 0;
  while (used < want) {
    ssize_t got = read(fd, buffer + used, want - used);
    if (got <= 0) break;
    used += (size_t)got;
  }
  close(fd);
  *out = buffer;
  *len = used;
  return 0;
}

int wd_file_read(const char *root, const char *path, size_t max, char **out,
                 size_t *len, char **err) {
  if (out == NULL || len == NULL) return -1;
  *out = NULL;
  *len = 0;
  if (path == NULL || path[0] == '\0') {
    if (err != NULL) *err = strdup("file.read: the path is empty");
    return -1;
  }
  char full[PATH_MAX];
  if (path[0] == '/') {
    snprintf(full, sizeof(full), "%s", path);
  } else {
    snprintf(full, sizeof(full), "%s/%s", root != NULL ? root : ".", path);
  }
  if (!inside_root(root != NULL ? root : ".", full)) {
    if (err != NULL)
      *err = strdup("file.read: the path is outside the session directory");
    return -1;
  }
  struct stat st;
  if (stat(full, &st) != 0) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.read: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    if (err != NULL) *err = strdup("file.read: not a regular file");
    return -1;
  }
  if ((size_t)st.st_size > max) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "file.read: %lld bytes exceeds the %zu byte limit",
               (long long)st.st_size, max);
      *err = strdup(buf);
    }
    return -1;
  }
  int fd = open(full, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.read: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  size_t size = (size_t)st.st_size;
  char *buffer = malloc(size + 1);
  if (buffer == NULL) {
    close(fd);
    if (err != NULL) *err = strdup("file.read: out of memory");
    return -1;
  }
  size_t used = 0;
  while (used < size) {
    ssize_t got = read(fd, buffer + used, size - used);
    if (got <= 0) break;
    used += (size_t)got;
  }
  close(fd);
  buffer[used] = '\0';
  *out = buffer;
  *len = used;
  return 0;
}

void wd_file_entries_free(wd_file_entry *entries, size_t len) {
  if (entries == NULL) return;
  for (size_t i = 0; i < len; i++) free(entries[i].name);
  free(entries);
}

int wd_file_list(const char *root, const char *path, size_t max,
                 wd_file_entry **out, size_t *len, bool *truncated, char **err) {
  if (out == NULL || len == NULL) return -1;
  *out = NULL;
  *len = 0;
  if (truncated != NULL) *truncated = false;
  if (max == 0) return 0;
  char full[PATH_MAX];
  if (path == NULL || path[0] == '\0') {
    snprintf(full, sizeof(full), "%s", root != NULL ? root : ".");
  } else if (path[0] == '/') {
    snprintf(full, sizeof(full), "%s", path);
  } else {
    snprintf(full, sizeof(full), "%s/%s", root != NULL ? root : ".", path);
  }
  if (!inside_root(root != NULL ? root : ".", full)) {
    if (err != NULL) *err = strdup("file.list: the path is outside the session directory");
    return -1;
  }
  DIR *dir = opendir(full);
  if (dir == NULL) {
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.list: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  size_t cap = max < 64 ? max : 64;
  wd_file_entry *entries = calloc(cap, sizeof(*entries));
  if (entries == NULL) {
    closedir(dir);
    if (err != NULL) *err = strdup("file.list: out of memory");
    return -1;
  }
  size_t used = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) break;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (used == max) {
      if (truncated != NULL) *truncated = true;
      break;
    }
    if (used == cap) {
      size_t grown = cap * 2 < max ? cap * 2 : max;
      wd_file_entry *next = realloc(entries, grown * sizeof(*entries));
      if (next == NULL) {
        wd_file_entries_free(entries, used);
        closedir(dir);
        if (err != NULL) *err = strdup("file.list: out of memory");
        return -1;
      }
      memset(next + cap, 0, (grown - cap) * sizeof(*next));
      entries = next;
      cap = grown;
    }
    char child[PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", full, entry->d_name);
    if (n <= 0 || (size_t)n >= sizeof(child)) continue;
    struct stat st;
    if (lstat(child, &st) != 0) continue;
    char *name = strdup(entry->d_name);
    if (name == NULL) {
      wd_file_entries_free(entries, used);
      closedir(dir);
      if (err != NULL) *err = strdup("file.list: out of memory");
      return -1;
    }
    entries[used].name = name;
    entries[used].kind = S_ISDIR(st.st_mode) ? WD_FILE_DIR
                        : S_ISREG(st.st_mode) ? WD_FILE_REGULAR
                                              : WD_FILE_OTHER;
    entries[used].size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    used++;
  }
  closedir(dir);
  if (errno != 0 && used == 0) {
    wd_file_entries_free(entries, used);
    if (err != NULL) {
      char buf[256];
      snprintf(buf, sizeof(buf), "file.list: %s", strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  *out = entries;
  *len = used;
  return 0;
}
