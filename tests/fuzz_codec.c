/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "proto/codec.h"
#include "proto/frame.h"

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void) {
  uint64_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  rng_state = x;
  return x;
}

static void chew(const uint8_t *buf, size_t len) {
  hx_frame frame;
  size_t used = 0;
  enum hx_frame_status status = hx_frame_parse(buf, len, &frame, &used);
  if (status != HX_FRAME_COMPLETE) return;
  cbor_item_t *root = hx_decode(frame.body, frame.len);
  if (root == NULL) return;
  uint64_t id = 0;
  char method[HX_MAX_METHOD];
  cbor_item_t *params = NULL;
  if (hx_read_request(root, &id, method, &params)) {
    uint64_t limit = 0;
    hx_uint_of(hx_get(params, "limit"), &limit);
  }
  bool ok = false;
  cbor_item_t *value = NULL;
  if (hx_read_response(root, &id, &ok, &value)) {
    char *name = hx_text_copy(hx_get(value, "message"));
    free(name);
  }
  unsigned version = 0;
  char *client = NULL;
  if (hx_read_hello(root, &version, &client)) free(client);
  char *kind = NULL;
  cbor_item_t *payload = NULL;
  if (hx_read_event(root, &kind, &payload)) free(kind);
  char text[128];
  hx_json(root, text, sizeof(text));
  hx_free(root);
}

int main(void) {
  cbor_item_t *seed = hx_request(1, "session.list", hx_map(0));
  size_t seed_len = 0;
  uint8_t *seed_body = hx_encode(seed, &seed_len);

  uint8_t packet[HX_FRAME_HEADER + HX_MAX_BODY];
  for (int iteration = 0; iteration < 20000; iteration++) {

    size_t len = 1 + (size_t)(rnd() % 2048);
    uint8_t *junk = malloc(len);
    assert(junk != NULL);
    for (size_t i = 0; i < len; i++) junk[i] = (uint8_t)rnd();
    chew(junk, len);
    free(junk);

    hx_frame_header(packet, (uint16_t)(1 + rnd() % 6), rnd(), rnd(),
                    (uint32_t)seed_len);
    memcpy(packet + HX_FRAME_HEADER, seed_body, seed_len);
    size_t mutations = 1 + (size_t)(rnd() % 4);
    for (size_t i = 0; i < mutations; i++) {
      size_t at = (size_t)(rnd() % (HX_FRAME_HEADER + seed_len));
      packet[at] ^= (uint8_t)(1u << (rnd() % 8));
    }
    chew(packet, HX_FRAME_HEADER + seed_len);
  }

  free(seed_body);
  printf("fuzz survived\n");
  return 0;
}
