/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HARNESS_PROTO_FRAME_H
#define HARNESS_PROTO_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define HX_FRAME_HEADER 22u
#define HX_MAX_BODY 65536u

enum hx_frame_type {
  HX_FRAME_HELLO = 1,
  HX_FRAME_WELCOME = 2,
  HX_FRAME_REQUEST = 3,
  HX_FRAME_RESPONSE = 4,
  HX_FRAME_EVENT = 5,
  HX_FRAME_CONTROL = 6,
  HX_FRAME_TYPE_MAX = 6,
};

typedef struct {
  uint16_t type;
  uint64_t sub;
  uint64_t seq;
  uint32_t len;
  const uint8_t *body;
} hx_frame;

enum hx_frame_status {
  HX_FRAME_INVALID = -1,
  HX_FRAME_COMPLETE = 0,
  HX_FRAME_PARTIAL = 1,
};

void hx_frame_header(uint8_t out[HX_FRAME_HEADER], uint16_t type, uint64_t sub,
                     uint64_t seq, uint32_t len);
enum hx_frame_status hx_frame_parse(const uint8_t *buf, size_t avail,
                                    hx_frame *out, size_t *used);

#endif
