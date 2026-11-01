/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdint.h>

#define HEAP_SIZE 65536u
#define RESULT_SIZE 8192u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t result[RESULT_SIZE];
static uint8_t state[64];
static uint32_t state_len;
static uint32_t counter;

__attribute__((import_module("ward"), import_name("cap")))
extern void ward_cap(uint32_t at, uint32_t len, uint32_t out);

static void copy(uint8_t *to, const uint8_t *from, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) to[i] = from[i];
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (heap_at + len > HEAP_SIZE) return 0;
  uint32_t at = (uint32_t)(uintptr_t)(heap + heap_at);
  heap_at += len;
  return at;
}

static void reply(uint8_t *out, const uint8_t *data, uint32_t len) {
  uint32_t at = (uint32_t)(uintptr_t)result;
  copy(result, data, len);
  put32(out, at);
  put32(out + 4, len);
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  const uint8_t *in = (const uint8_t *)(uintptr_t)at;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  uint32_t op = len > 0 ? in[0] : 0xffu;
  const uint8_t *payload = in + 1;
  uint32_t plen = len > 0 ? len - 1 : 0;

  if (op == 0) {
    reply(o, payload, plen);
    return;
  }
  if (op == 1) {
    counter++;
    put32(result, counter);
    reply(o, result, 4);
    return;
  }
  if (op == 2) {
    state_len = plen < sizeof(state) ? plen : (uint32_t)sizeof(state);
    copy(state, payload, state_len);
    reply(o, payload, plen);
    return;
  }
  if (op == 3) {
    volatile uint64_t spin = 0;
    for (;;) spin++;
  }
  if (op == 4) {
    uint8_t message[256];
    uint32_t total = 4 + plen;
    put32(message, plen);
    copy(message + 4, payload, plen);
    uint32_t desc[2];
    ward_cap((uint32_t)(uintptr_t)message, total, (uint32_t)(uintptr_t)desc);
    put32(o, desc[0]);
    put32(o + 4, desc[1]);
    return;
  }
  if (op == 5) {
    reply(o, state, state_len);
    return;
  }
  put32(o, 0);
  put32(o + 4, 0);
}

__attribute__((export_name("ward_state_export")))
void ward_state_export(uint32_t out) {
  reply((uint8_t *)(uintptr_t)out, state, state_len);
}

__attribute__((export_name("ward_state_migrate")))
void ward_state_migrate(uint32_t schema, uint32_t at, uint32_t len,
                        uint32_t out) {
  if (len >= sizeof(state)) len = sizeof(state) - 1;
  state[0] = (uint8_t)(schema + 1);
  copy(state + 1, (const uint8_t *)(uintptr_t)at, len);
  state_len = len + 1;
  put32((uint8_t *)(uintptr_t)out, (uint32_t)(uintptr_t)state);
  put32((uint8_t *)(uintptr_t)out + 4, state_len);
}

__attribute__((export_name("ward_state_import")))
uint32_t ward_state_import(uint32_t at, uint32_t len) {
  if (len > sizeof(state)) return 1;
  state_len = len;
  copy(state, (const uint8_t *)(uintptr_t)at, len);
  return 0;
}
