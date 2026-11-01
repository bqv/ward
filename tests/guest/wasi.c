/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdint.h>

static uint8_t heap[1024];

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write")))
extern int fd_write(int fd, int iovs, int iovs_len, int nwritten);

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (len > sizeof(heap)) return 0;
  return (uint32_t)(uintptr_t)heap;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  (void)at;
  (void)len;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  fd_write(1, 0, 0, 0);
  o[0] = 0;
  o[1] = 0;
  o[2] = 0;
  o[3] = 0;
  o[4] = 0;
  o[5] = 0;
  o[6] = 0;
  o[7] = 0;
}
