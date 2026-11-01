/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "core/util.h"

static int read_exact(int fd, uint8_t *buf, size_t len, bool *at_eof) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) {
      if (off == 0) {
        if (at_eof != NULL) *at_eof = true;
        return 0;
      }
      return -1;
    }
    off += (size_t)n;
  }
  return 1;
}

int hlog_create(hlog *log, const char *path) {
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  log->fd = fd;
  log->path = strdup(path);
  if (log->path == NULL) {
    close(fd);
    return -1;
  }
  return 0;
}

int hlog_open_append(hlog *log, const char *path) {
  int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
  if (fd < 0) return -1;
  log->fd = fd;
  log->path = strdup(path);
  if (log->path == NULL) {
    close(fd);
    return -1;
  }
  return 0;
}

int hlog_append(hlog *log, const uint8_t *data, size_t len) {
  if (len == 0 || len > HLOG_MAX_RECORD) {
    errno = EMSGSIZE;
    return -1;
  }
  size_t cap = ZSTD_compressBound(len);
  uint8_t *frame = malloc(cap);
  if (frame == NULL) return -1;
  size_t n = ZSTD_compress(frame, cap, data, len, 3);
  if (ZSTD_isError(n)) {
    free(frame);
    errno = EIO;
    return -1;
  }
  uint8_t prefix[4];
  prefix[0] = (uint8_t)(n & 0xff);
  prefix[1] = (uint8_t)((n >> 8) & 0xff);
  prefix[2] = (uint8_t)((n >> 16) & 0xff);
  prefix[3] = (uint8_t)((n >> 24) & 0xff);
  int rc = 0;
  if (wd_write_all(log->fd, prefix, sizeof(prefix)) != 0 ||
      wd_write_all(log->fd, frame, n) != 0 || fsync(log->fd) != 0)
    rc = -1;
  free(frame);
  return rc;
}

void hlog_close(hlog *log) {
  if (log->fd >= 0) close(log->fd);
  log->fd = -1;
  free(log->path);
  log->path = NULL;
}

int hlog_read(const char *path, hlog_cb cb, void *ctx, bool *truncated) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  if (truncated != NULL) *truncated = false;
  int records = 0;
  for (;;) {
    uint8_t prefix[4];
    bool eof = false;
    int rc = read_exact(fd, prefix, sizeof(prefix), &eof);
    if (rc == 0) break;
    if (rc < 0) {
      if (records == 0) {
        close(fd);
        return -1;
      }
      if (truncated != NULL) *truncated = true;
      break;
    }
    uint32_t len = (uint32_t)prefix[0] | ((uint32_t)prefix[1] << 8) |
                   ((uint32_t)prefix[2] << 16) | ((uint32_t)prefix[3] << 24);
    if (len == 0 || len > 4u * HLOG_MAX_RECORD) {
      if (truncated != NULL) *truncated = true;
      break;
    }
    uint8_t *frame = malloc(len);
    if (frame == NULL) break;
    eof = false;
    rc = read_exact(fd, frame, len, &eof);
    if (rc <= 0) {
      free(frame);
      if (truncated != NULL) *truncated = true;
      break;
    }
    unsigned long long size = ZSTD_getFrameContentSize(frame, len);
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN ||
        size > HLOG_MAX_RECORD) {
      free(frame);
      if (truncated != NULL) *truncated = true;
      break;
    }
    uint8_t *plain = malloc((size_t)size + 1);
    if (plain == NULL) {
      free(frame);
      if (truncated != NULL) *truncated = true;
      break;
    }
    size_t got = ZSTD_decompress(plain, (size_t)size, frame, len);
    free(frame);
    if (ZSTD_isError(got) || got != size) {
      free(plain);
      if (truncated != NULL) *truncated = true;
      break;
    }
    int stop = cb(ctx, plain, got);
    free(plain);
    if (stop) break;
    records++;
  }
  close(fd);
  return records;
}
