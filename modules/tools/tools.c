/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 8192u
#define REPLY_SIZE 4096u
#define TEXT_SIZE 1024u
#define CALLBUF_SIZE 4096u
#define PAYLOAD_SIZE 2048u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static uint8_t callbuf[CALLBUF_SIZE];
static uint8_t payload[PAYLOAD_SIZE];

__attribute__((import_module("ward"), import_name("cap")))
extern void ward_cap(uint32_t at, uint32_t len, uint32_t out);

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (heap_at + len > HEAP_SIZE) return 0;
  uint32_t at = (uint32_t)(uintptr_t)(heap + heap_at);
  heap_at += len;
  return at;
}

static void pair(uint32_t out, const uint8_t *data, uint32_t n) {
  uint32_t at = (uint32_t)(uintptr_t)reply;
  for (uint32_t i = 0; i < n && i < REPLY_SIZE; i++) reply[i] = data[i];
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, at);
  wd_put32(o + 4, n);
}

static void forward(uint32_t out, const char *capability, const uint8_t *body,
                    uint32_t len) {
  uint32_t name_len = wd_len(capability);
  if (4 + name_len + len > CALLBUF_SIZE) return;
  wd_put32(callbuf, name_len);
  for (uint32_t i = 0; i < name_len; i++) callbuf[4 + i] = (uint8_t)capability[i];
  for (uint32_t i = 0; i < len; i++) callbuf[4 + name_len + i] = body[i];
  uint32_t desc[2];
  ward_cap((uint32_t)(uintptr_t)callbuf, 4 + name_len + len,
           (uint32_t)(uintptr_t)desc);
  if (desc[1] > REPLY_SIZE) return;
  const uint8_t *p = (const uint8_t *)(uintptr_t)desc[0];
  for (uint32_t i = 0; i < desc[1]; i++) reply[i] = p[i];
  pair(out, reply, desc[1]);
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char tool[TEXT_SIZE];
  char path[TEXT_SIZE];
  char text[TEXT_SIZE];
  tool[0] = '\0';
  path[0] = '\0';
  text[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "tool")) {
      if (wd_r_text(&r, tool, sizeof(tool), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "args")) {
      uint32_t an = 0;
      if (wd_r_map(&r, &an) != 0) return;
      for (uint32_t k = 0; k < an; k++) {
        const uint8_t *ak = 0;
        uint32_t al = 0;
        if (wd_r_text_ptr(&r, &ak, &al) != 0) return;
        if (wd_key_eq(ak, al, "path")) {
          if (wd_r_text(&r, path, sizeof(path), 0) != 0) return;
        } else if (wd_key_eq(ak, al, "text")) {
          if (wd_r_text(&r, text, sizeof(text), 0) != 0) return;
        } else if (wd_r_skip(&r) != 0) {
          return;
        }
      }
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }
  if (tool[0] == '\0' || path[0] == '\0') return;

  wd_w w;
  wd_w_init(&w, payload, PAYLOAD_SIZE);
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "read")) {
    wd_w_map(&w, 1);
    wd_w_textz(&w, "path");
    wd_w_textz(&w, path);
    if (w.failed) return;
    forward(out, "fs.read", payload, w.at);
    return;
  }
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "write")) {
    wd_w_map(&w, 2);
    wd_w_textz(&w, "path");
    wd_w_textz(&w, path);
    wd_w_textz(&w, "text");
    wd_w_textz(&w, text);
    if (w.failed) return;
    forward(out, "fs.write", payload, w.at);
  }
}
