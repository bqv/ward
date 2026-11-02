/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/files.h"

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
