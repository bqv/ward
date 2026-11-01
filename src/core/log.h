/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_LOG_H
#define WARD_CORE_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLOG_MAX_RECORD (1u << 20)

typedef struct {
  int fd;
  char *path;
} hlog;

int hlog_create(hlog *log, const char *path);
int hlog_open_append(hlog *log, const char *path);
int hlog_append(hlog *log, const uint8_t *data, size_t len);
void hlog_close(hlog *log);

typedef int (*hlog_cb)(void *ctx, const uint8_t *data, size_t len);

int hlog_read(const char *path, hlog_cb cb, void *ctx, bool *truncated);

#endif
