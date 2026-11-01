/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_PROTO_SSE_H
#define WARD_PROTO_SSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_SSE_MAX_LINE 65536u
#define WD_SSE_MAX_DATA (1u << 20)
#define WD_SSE_MAX_TYPE 64u
#define WD_SSE_MAX_ID 1024u

typedef struct {
  const char *type;
  size_t type_len;
  const char *data;
  size_t data_len;
  const char *id;
  size_t id_len;
} wd_sse_event;

typedef int (*wd_sse_emit)(void *ctx, const wd_sse_event *event);

typedef struct wd_sse wd_sse;

wd_sse *wd_sse_create(wd_sse_emit emit, void *ctx);
void wd_sse_destroy(wd_sse *sse);
int wd_sse_feed(wd_sse *sse, const char *buf, size_t len);
int wd_sse_finish(wd_sse *sse);
bool wd_sse_failed(const wd_sse *sse);

#endif
