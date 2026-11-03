/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

__attribute__((import_module("ward"), import_name("cap")))
extern void ward_cap(uint32_t at, uint32_t len, uint32_t out);

static uint8_t callbuf[4096];

static int cap_call(const char *capability, const uint8_t *payload, uint32_t len,
                    uint8_t *out, uint32_t *out_len) {
  uint32_t name_len = wd_len(capability);
  if (4 + name_len + len > sizeof(callbuf)) return -1;
  wd_put32(callbuf, name_len);
  for (uint32_t i = 0; i < name_len; i++)
    callbuf[4 + i] = (uint8_t)capability[i];
  for (uint32_t i = 0; i < len; i++) callbuf[4 + name_len + i] = payload[i];
  uint32_t desc[2];
  ward_cap((uint32_t)(uintptr_t)callbuf, 4 + name_len + len,
           (uint32_t)(uintptr_t)desc);
  if (desc[1] > 4096) return -1;
  *out_len = desc[1];
  const uint8_t *p = (const uint8_t *)(uintptr_t)desc[0];
  for (uint32_t i = 0; i < desc[1]; i++) out[i] = p[i];
  return 0;
}

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

  if (wd_key_eq((const uint8_t *)verdict, wd_len(verdict), "allow") &&
      (wd_key_eq((const uint8_t *)tool, wd_len(tool), "write") ||
       wd_key_eq((const uint8_t *)tool, wd_len(tool), "edit") ||
       wd_key_eq((const uint8_t *)tool, wd_len(tool), "exec"))) {
    uint8_t asked[2048];
    wd_w q;
    wd_w_init(&q, asked, sizeof(asked));
    wd_w_map(&q, 1);
    wd_w_textz(&q, "text");
    wd_w_textz(&q, payload);
    uint8_t judged[512];
    uint32_t judged_len = 0;
    if (!q.failed &&
        cap_call("judge.score", asked, q.at, judged, &judged_len) == 0) {
      wd_r jr = {judged, judged_len, 0, 0};
      uint32_t jn = 0;
      uint32_t score_bp = 0;
      if (wd_r_map(&jr, &jn) == 0) {
        for (uint32_t k = 0; k < jn; k++) {
          const uint8_t *jk = 0;
          uint32_t jkn = 0;
          if (wd_r_text_ptr(&jr, &jk, &jkn) != 0) break;
          if (wd_key_eq(jk, jkn, "score_bp")) {
            if (wd_r_uint(&jr, &score_bp) != 0) score_bp = 0;
          } else if (wd_r_skip(&jr) != 0) {
            break;
          }
        }
      }
      if (score_bp >= 500) {
        verdict = "ask";
        reason = "the decision model scored this action as risky";
      }
    }
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
