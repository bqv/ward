/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/ids.h"

#include "core/util.h"

static const char hex[] = "0123456789abcdef";

int wd_new_id(char out[WD_ID_LEN]) {
  uint8_t raw[8];
  if (wd_random_bytes(raw, sizeof(raw)) != 0) return -1;
  for (size_t i = 0; i < sizeof(raw); i++) {
    out[2 * i] = hex[raw[i] >> 4];
    out[2 * i + 1] = hex[raw[i] & 0x0f];
  }
  out[WD_ID_LEN - 1] = '\0';
  return 0;
}
