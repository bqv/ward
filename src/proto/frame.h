/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_PROTO_FRAME_H
#define WARD_PROTO_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define WD_FRAME_HEADER 22u
#define WD_MAX_BODY 65536u

enum wd_frame_type {
  WD_FRAME_HELLO = 1,
  WD_FRAME_WELCOME = 2,
  WD_FRAME_REQUEST = 3,
  WD_FRAME_RESPONSE = 4,
  WD_FRAME_EVENT = 5,
  WD_FRAME_CONTROL = 6,
  WD_FRAME_TYPE_MAX = 6,
};

typedef struct {
  uint16_t type;
  uint64_t sub;
  uint64_t seq;
  uint32_t len;
  const uint8_t *body;
} wd_frame;

enum wd_frame_status {
  WD_FRAME_INVALID = -1,
  WD_FRAME_COMPLETE = 0,
  WD_FRAME_PARTIAL = 1,
};

void wd_frame_header(uint8_t out[WD_FRAME_HEADER], uint16_t type, uint64_t sub,
                     uint64_t seq, uint32_t len);
enum wd_frame_status wd_frame_parse(const uint8_t *buf, size_t avail,
                                    wd_frame *out, size_t *used);

#endif
