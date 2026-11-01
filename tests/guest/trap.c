/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdint.h>

static uint8_t heap[4096];

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (len > sizeof(heap)) return 0;
  return (uint32_t)(uintptr_t)heap;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  (void)at;
  (void)len;
  (void)out;
  __builtin_trap();
}
