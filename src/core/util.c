/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/util.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int wu_write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

int wu_mkdir_p(const char *path, mode_t mode) {
  if (path == NULL || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }
  char *copy = strdup(path);
  if (copy == NULL) return -1;
  size_t len = strlen(copy);
  while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
  for (char *p = copy + 1; *p != '\0'; p++) {
    if (*p != '/') continue;
    *p = '\0';
    if (mkdir(copy, mode) != 0 && errno != EEXIST) {
      free(copy);
      return -1;
    }
    *p = '/';
  }
  int rc = mkdir(copy, mode);
  if (rc != 0 && errno == EEXIST) rc = 0;
  free(copy);
  return rc;
}

int wu_fsync_dir(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return -1;
  int rc = fsync(fd);
  close(fd);
  return rc;
}

char *wu_path_join(const char *a, const char *b) {
  if (a == NULL || b == NULL) return NULL;
  size_t alen = strlen(a);
  size_t blen = strlen(b);
  bool slash = alen > 0 && a[alen - 1] == '/';
  char *out = malloc(alen + (slash ? 0 : 1) + blen + 1);
  if (out == NULL) return NULL;
  memcpy(out, a, alen);
  size_t at = alen;
  if (!slash) out[at++] = '/';
  memcpy(out + at, b, blen);
  out[at + blen] = '\0';
  return out;
}

char *wu_default_state_dir(void) {
  const char *xdg = getenv("XDG_STATE_HOME");
  if (xdg != NULL && xdg[0] != '\0') return wu_path_join(xdg, "ward");
  const char *home = getenv("HOME");
  if (home != NULL && home[0] != '\0') {
    char *base = wu_path_join(home, ".local/state");
    if (base == NULL) return NULL;
    char *full = wu_path_join(base, "ward");
    free(base);
    return full;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "/tmp/ward-state-%lu",
           (unsigned long)geteuid());
  return strdup(buf);
}

char *wu_default_socket_path(void) {
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime != NULL && runtime[0] != '\0') return wu_path_join(runtime, "ward.sock");
  char buf[64];
  snprintf(buf, sizeof(buf), "/tmp/ward-%lu.sock", (unsigned long)geteuid());
  return strdup(buf);
}

char *wu_boot_id(void) {
  int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return NULL;
  char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return NULL;
  size_t len = (size_t)n;
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
  buf[len] = '\0';
  return strdup(buf);
}

uint64_t wu_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

bool wu_pid_alive(long pid) {
  if (pid <= 0) return false;
  if (kill((pid_t)pid, 0) == 0) return true;
  return errno == EPERM;
}

int wu_random_bytes(void *buf, size_t len) {
  uint8_t *p = buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = getrandom(p + off, len - off, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}
