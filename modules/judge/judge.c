/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 4096u
#define REPLY_SIZE 1024u
#define TEXT_SIZE 256u

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

static int contains(const char *haystack, const char *needle) {
  size_t n = wd_len(needle);
  if (n == 0) return 0;
  for (const char *p = haystack; *p != '\0'; p++) {
    size_t i = 0;
    while (i < n && p[i] == needle[i]) i++;
    if (i == n) return 1;
  }
  return 0;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char tool[TEXT_SIZE];
  char payload[1024];
  tool[0] = '\0';
  payload[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "tool")) {
      if (wd_r_text(&r, tool, sizeof(tool), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "payload")) {
      if (wd_r_text(&r, payload, sizeof(payload), 0) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  const char *verdict = "allow";
  const char *reason = "nothing in the payload looks destructive";
  if (contains(payload, "rm -rf") || contains(payload, "mkfs") ||
      contains(payload, "dd if=") || contains(payload, "> /dev/sd")) {
    verdict = "ask";
    reason = "the payload can destroy data";
  } else if (contains(payload, "curl ") || contains(payload, "wget ")) {
    verdict = "ask";
    reason = "the payload fetches from the network";
  } else if (contains(payload, "ssh ") || contains(payload, "git push")) {
    verdict = "ask";
    reason = "the payload leaves this machine";
  } else if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "write") ||
             wd_key_eq((const uint8_t *)tool, wd_len(tool), "exec")) {
    verdict = "allow";
    reason = "the payload is a plain command";
  }

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 2);
  wd_w_textz(&w, "verdict");
  wd_w_textz(&w, verdict);
  wd_w_textz(&w, "reason");
  wd_w_textz(&w, reason);
  if (w.failed) return;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, (uint32_t)(uintptr_t)reply);
  wd_put32(o + 4, w.at);
}
