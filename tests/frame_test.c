/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "proto/frame.h"

static void test_header_layout(void) {
  uint8_t header[HX_FRAME_HEADER];
  hx_frame_header(header, HX_FRAME_REQUEST, 0x0102030405060708ull,
                  0x1112131415161718ull, 0x0a0b0c0d);
  assert(header[0] == 0x0d && header[1] == 0x0c && header[2] == 0x0b &&
         header[3] == 0x0a);
  assert(header[4] == HX_FRAME_REQUEST && header[5] == 0);
  assert(header[6] == 0x08 && header[13] == 0x01);
  assert(header[14] == 0x18 && header[21] == 0x11);
}

static void test_round_trip(void) {
  uint8_t packet[HX_FRAME_HEADER + 3];
  hx_frame_header(packet, HX_FRAME_EVENT, 7, 42, 3);
  packet[HX_FRAME_HEADER] = 0xa0;
  packet[HX_FRAME_HEADER + 1] = 0x01;
  packet[HX_FRAME_HEADER + 2] = 0x02;
  hx_frame frame;
  size_t used = 0;
  assert(hx_frame_parse(packet, sizeof(packet), &frame, &used) ==
         HX_FRAME_COMPLETE);
  assert(frame.type == HX_FRAME_EVENT);
  assert(frame.sub == 7);
  assert(frame.seq == 42);
  assert(frame.len == 3);
  assert(used == sizeof(packet));
  assert(frame.body[2] == 0x02);
}

static void test_partial(void) {
  uint8_t packet[HX_FRAME_HEADER];
  hx_frame_header(packet, HX_FRAME_HELLO, 0, 0, 5);
  hx_frame frame;
  size_t used = 0;
  assert(hx_frame_parse(packet, HX_FRAME_HEADER - 1, &frame, &used) ==
         HX_FRAME_PARTIAL);
  assert(hx_frame_parse(packet, HX_FRAME_HEADER, &frame, &used) ==
         HX_FRAME_PARTIAL);
  assert(hx_frame_parse(packet, HX_FRAME_HEADER + 4, &frame, &used) ==
         HX_FRAME_PARTIAL);
}

static void test_invalid(void) {
  uint8_t packet[HX_FRAME_HEADER];
  hx_frame frame;
  size_t used = 0;
  hx_frame_header(packet, HX_FRAME_REQUEST, 0, 0, 0);
  assert(hx_frame_parse(packet, sizeof(packet), &frame, &used) ==
         HX_FRAME_INVALID);
  hx_frame_header(packet, 0, 0, 0, 4);
  assert(hx_frame_parse(packet, sizeof(packet), &frame, &used) ==
         HX_FRAME_INVALID);
  hx_frame_header(packet, 7, 0, 0, 4);
  assert(hx_frame_parse(packet, sizeof(packet), &frame, &used) ==
         HX_FRAME_INVALID);
  hx_frame_header(packet, HX_FRAME_REQUEST, 0, 0, HX_MAX_BODY + 1);
  assert(hx_frame_parse(packet, sizeof(packet), &frame, &used) ==
         HX_FRAME_INVALID);
}

int main(void) {
  test_header_layout();
  test_round_trip();
  test_partial();
  test_invalid();
  printf("frame tests passed\n");
  return 0;
}
