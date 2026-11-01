/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE (64u * 1024u)
#define REPLY_SIZE 2048u
#define TEXT_SIZE 1024u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (heap_at + len > HEAP_SIZE) return 0;
  uint32_t at = (uint32_t)(uintptr_t)(heap + heap_at);
  heap_at += len;
  return at;
}

static void pair(uint32_t out, uint32_t n) {
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, (uint32_t)(uintptr_t)reply);
  wd_put32(o + 4, n);
}

static void answer(int handled, const char *text) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 2);
  wd_w_textz(&w, "handled");
  wd_w_textz(&w, handled ? "yes" : "no");
  wd_w_textz(&w, "text");
  wd_w_textz(&w, text);
  if (!w.failed) pair(0, 0);
}

static const char help_text[] =
    "skills: /help, /rules, /skills. Everything else is a prompt.";

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char command[TEXT_SIZE];
  command[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "command")) {
      if (wd_r_text(&r, command, sizeof(command), 0) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  int handled = 0;
  const char *text = "";
  if (wd_key_eq((const uint8_t *)command, wd_len(command), "help")) {
    handled = 1;
    text = help_text;
  } else if (wd_key_eq((const uint8_t *)command, wd_len(command), "skills")) {
    handled = 1;
    text = "skills: help, rules";
  } else if (wd_key_eq((const uint8_t *)command, wd_len(command), "rules")) {
    handled = 1;
    text = "project rules are read from AGENTS.md in the session's cwd";
  }

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 2);
  wd_w_textz(&w, "handled");
  wd_w_textz(&w, handled ? "yes" : "no");
  wd_w_textz(&w, "text");
  wd_w_textz(&w, text);
  if (w.failed) return;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, (uint32_t)(uintptr_t)reply);
  wd_put32(o + 4, w.at);
}
