/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 4096u
#define REPLY_SIZE 2048u
#define TEXT_SIZE 512u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static char prompt[TEXT_SIZE];
static char answer[TEXT_SIZE];

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

static void copy_text(char *to, uint32_t cap, const char *from, uint32_t n) {
  uint32_t limit = n < cap - 1 ? n : cap - 1;
  for (uint32_t i = 0; i < limit; i++) to[i] = from[i];
  to[limit] = '\0';
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  if (wd_r_map(&r, &n) != 0) return;
  int start = 0;
  int result = 0;
  uint32_t plen = 0;
  char text[TEXT_SIZE];
  text[0] = '\0';
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "start")) {
      if (wd_r_text(&r, text, sizeof(text), &plen) != 0) return;
      start = 1;
    } else if (wd_key_eq(kp, kn, "result")) {
      if (wd_r_text(&r, text, sizeof(text), &plen) != 0) return;
      result = 1;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }
  if (start) {
    copy_text(prompt, sizeof(prompt), text, plen);
    wd_w w;
    wd_w_init(&w, reply, REPLY_SIZE);
    wd_w_map(&w, 2);
    wd_w_textz(&w, "action");
    wd_w_textz(&w, "model");
    wd_w_textz(&w, "prompt");
    wd_w_text(&w, prompt, wd_len(prompt));
    if (!w.failed) pair(out, reply, w.at);
    return;
  }
  if (result) {
    copy_text(answer, sizeof(answer), text, plen);
    wd_w w;
    wd_w_init(&w, reply, REPLY_SIZE);
    wd_w_map(&w, 2);
    wd_w_textz(&w, "action");
    wd_w_textz(&w, "done");
    wd_w_textz(&w, "text");
    wd_w_text(&w, answer, wd_len(answer));
    if (!w.failed) pair(out, reply, w.at);
    return;
  }
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 2);
  wd_w_textz(&w, "action");
  wd_w_textz(&w, "idle");
  wd_w_textz(&w, "text");
  wd_w_textz(&w, "");
  if (!w.failed) pair(out, reply, w.at);
}
