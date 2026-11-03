/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../modules/abi/wd_abi.h"

int main(void) {
  uint8_t hostile[] = {0x7a, 0xff, 0xff, 0xff, 0xff};
  wd_r reader = {hostile, sizeof(hostile), 0, 0};
  char out[16];
  uint32_t len = 0;
  assert(wd_r_text(&reader, out, sizeof(out), &len) != 0);
  assert(reader.failed == 1);

  wd_r pointer_reader = {hostile, sizeof(hostile), 0, 0};
  const uint8_t *p = NULL;
  uint32_t n = 0;
  assert(wd_r_text_ptr(&pointer_reader, &p, &n) != 0);
  assert(pointer_reader.failed == 1);

  uint8_t room[64];
  wd_w writer;
  wd_w_init(&writer, room, sizeof(room));
  wd_w_text(&writer, "abcdefgh", 8);
  assert(writer.failed == 0);

  char exact[8];
  wd_r narrow = {room, 9, 0, 0};
  assert(wd_r_text(&narrow, exact, sizeof(exact), &len) != 0);
  assert(narrow.failed == 1);

  char fits[9];
  wd_r roomy = {room, 9, 0, 0};
  assert(wd_r_text(&roomy, fits, sizeof(fits), &len) == 0);
  assert(len == 8 && strcmp(fits, "abcdefgh") == 0);

  printf("abi tests passed\n");
  return 0;
}
