/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "proto/frame.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

static void put_u64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

static uint16_t get_u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
  return v;
}

static uint64_t get_u64(const uint8_t *p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

void wd_frame_header(uint8_t out[WD_FRAME_HEADER], uint16_t type, uint64_t sub,
                     uint64_t seq, uint32_t len) {
  memset(out, 0, WD_FRAME_HEADER);
  put_u32(out, len);
  put_u16(out + 4, type);
  put_u64(out + 6, sub);
  put_u64(out + 14, seq);
}

enum wd_frame_status wd_frame_parse(const uint8_t *buf, size_t avail,
                                    wd_frame *out, size_t *used) {
  if (avail < WD_FRAME_HEADER) return WD_FRAME_PARTIAL;
  uint32_t len = get_u32(buf);
  uint16_t type = get_u16(buf + 4);
  if (len == 0 || len > WD_MAX_BODY) return WD_FRAME_INVALID;
  if (type == 0 || type > WD_FRAME_TYPE_MAX) return WD_FRAME_INVALID;
  if (avail - WD_FRAME_HEADER < len) return WD_FRAME_PARTIAL;
  out->type = type;
  out->sub = get_u64(buf + 6);
  out->seq = get_u64(buf + 14);
  out->len = len;
  out->body = buf + WD_FRAME_HEADER;
  if (used) *used = WD_FRAME_HEADER + len;
  return WD_FRAME_COMPLETE;
}
