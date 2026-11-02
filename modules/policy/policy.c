#include <stdbool.h>
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 1024u
#define REPLY_SIZE 512u
#define TEXT_SIZE 64u

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

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char tool[TEXT_SIZE];
  tool[0] = '\0';
  char rule_read[8] = "allow";
  char rule_write[8] = "ask";
  char rule_edit[8] = "ask";
  char rule_exec[8] = "ask";
  char rule_grep[8] = "allow";
  bool has_rules = false;
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "tool")) {
      if (wd_r_text(&r, tool, sizeof(tool), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "rules")) {
      uint32_t pairs = 0;
      if (wd_r_map(&r, &pairs) != 0) return;
      has_rules = true;
      for (uint32_t k = 0; k < pairs; k++) {
        const uint8_t *rk = 0;
        uint32_t rkn = 0;
        if (wd_r_text_ptr(&r, &rk, &rkn) != 0) return;
        if (wd_key_eq(rk, rkn, "read"))
          { if (wd_r_text(&r, rule_read, sizeof(rule_read), 0) != 0) return; }
        else if (wd_key_eq(rk, rkn, "write"))
          { if (wd_r_text(&r, rule_write, sizeof(rule_write), 0) != 0) return; }
        else if (wd_key_eq(rk, rkn, "edit"))
          { if (wd_r_text(&r, rule_edit, sizeof(rule_edit), 0) != 0) return; }
        else if (wd_key_eq(rk, rkn, "exec"))
          { if (wd_r_text(&r, rule_exec, sizeof(rule_exec), 0) != 0) return; }
        else if (wd_key_eq(rk, rkn, "grep"))
          { if (wd_r_text(&r, rule_grep, sizeof(rule_grep), 0) != 0) return; }
        else if (wd_r_skip(&r) != 0) return;
      }
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }
  const char *decision = has_rules ? rule_write : "deny";
  if (has_rules) {
    if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "read")) decision = rule_read;
    if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "write")) decision = rule_write;
    if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "exec")) decision = rule_exec;
    if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "edit")) decision = rule_edit;
    if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "grep")) decision = rule_grep;
  }
  if (0) decision = "deny";
  if (!has_rules && wd_key_eq((const uint8_t *)tool, wd_len(tool), "read")) decision = "allow";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "write")) decision = "ask";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "exec")) decision = "ask";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "edit")) decision = "ask";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "grep")) decision = "allow";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "python")) decision = "ask";
  if (wd_key_eq((const uint8_t *)tool, wd_len(tool), "plan")) decision = "ask";

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "decision");
  wd_w_textz(&w, decision);
  if (w.failed) return;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, (uint32_t)(uintptr_t)reply);
  wd_put32(o + 4, w.at);
}
