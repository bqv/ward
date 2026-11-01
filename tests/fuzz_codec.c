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
  wd_frame frame;
  size_t used = 0;
  enum wd_frame_status status = wd_frame_parse(buf, len, &frame, &used);
  if (status != WD_FRAME_COMPLETE) return;
  cbor_item_t *root = wd_decode(frame.body, frame.len);
  if (root == NULL) return;
  uint64_t id = 0;
  char method[WD_MAX_METHOD];
  cbor_item_t *params = NULL;
  if (wd_read_request(root, &id, method, &params)) {
    uint64_t limit = 0;
    wd_uint_of(wd_get(params, "limit"), &limit);
  }
  bool ok = false;
  cbor_item_t *value = NULL;
  if (wd_read_response(root, &id, &ok, &value)) {
    char *name = wd_text_copy(wd_get(value, "message"));
    free(name);
  }
  unsigned version = 0;
  char *client = NULL;
  if (wd_read_hello(root, &version, &client)) free(client);
  char *kind = NULL;
  cbor_item_t *payload = NULL;
  if (wd_read_event(root, &kind, &payload)) free(kind);
  char text[128];
  wd_json(root, text, sizeof(text));
  wd_free(root);
}

int main(void) {
  cbor_item_t *seed = wd_request(1, "session.list", wd_map(0));
  size_t seed_len = 0;
  uint8_t *seed_body = wd_encode(seed, &seed_len);

  uint8_t packet[WD_FRAME_HEADER + WD_MAX_BODY];
  for (int iteration = 0; iteration < 20000; iteration++) {

    size_t len = 1 + (size_t)(rnd() % 2048);
    uint8_t *junk = malloc(len);
    assert(junk != NULL);
    for (size_t i = 0; i < len; i++) junk[i] = (uint8_t)rnd();
    chew(junk, len);
    free(junk);

    wd_frame_header(packet, (uint16_t)(1 + rnd() % 6), rnd(), rnd(),
                    (uint32_t)seed_len);
    memcpy(packet + WD_FRAME_HEADER, seed_body, seed_len);
    size_t mutations = 1 + (size_t)(rnd() % 4);
    for (size_t i = 0; i < mutations; i++) {
      size_t at = (size_t)(rnd() % (WD_FRAME_HEADER + seed_len));
      packet[at] ^= (uint8_t)(1u << (rnd() % 8));
    }
    chew(packet, WD_FRAME_HEADER + seed_len);
  }

  free(seed_body);
  printf("fuzz survived\n");
  return 0;
}
