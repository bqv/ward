/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

__attribute__((import_module("ward"), import_name("cap")))
extern void ward_cap(uint32_t at, uint32_t len, uint32_t out);

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
  char args[TEXT_SIZE];
  command[0] = '\0';
  args[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "command")) {
      if (wd_r_text(&r, command, sizeof(command), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "args")) {
      if (wd_r_text(&r, args, sizeof(args), 0) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  static char search_text[REPLY_SIZE];
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
  if (wd_key_eq((const uint8_t *)command, wd_len(command), "recall")) {
    handled = 1;
    if (args[0] == '\0') {
      text = "usage: /recall <words>";
    } else {
      static uint8_t rbuf[1024];
      static const char rname[] = "recall.search";
      wd_put32(rbuf, 13);
      for (uint32_t i = 0; i < 13; i++) rbuf[4 + i] = (uint8_t)rname[i];
      wd_w rw;
      wd_w_init(&rw, rbuf + 17, sizeof(rbuf) - 17);
      wd_w_map(&rw, 2);
      wd_w_textz(&rw, "query");
      wd_w_textz(&rw, args);
      wd_w_textz(&rw, "k");
      wd_w_textz(&rw, "5");
      uint32_t rdesc[2];
      search_text[0] = '\0';
      if (!rw.failed) {
        ward_cap((uint32_t)(uintptr_t)rbuf, 17 + rw.at, (uint32_t)(uintptr_t)rdesc);
        const uint8_t *rres = (const uint8_t *)(uintptr_t)rdesc[0];
        wd_r rr3 = {rres, rdesc[1], 0, 0};
        uint32_t rn3 = 0;
        if (wd_r_map(&rr3, &rn3) == 0) {
          for (uint32_t i = 0; i < rn3; i++) {
            const uint8_t *kp4 = 0;
            uint32_t kn4 = 0;
            if (wd_r_text_ptr(&rr3, &kp4, &kn4) != 0) break;
            if (wd_key_eq(kp4, kn4, "text")) {
              wd_r_text(&rr3, search_text, sizeof(search_text), 0);
            } else if (wd_r_skip(&rr3) != 0) {
              break;
            }
          }
        }
      }
      if (search_text[0] == '\0') {
        static const char rnone[] = "nothing found";
        for (uint32_t i = 0; i < sizeof(rnone); i++) search_text[i] = rnone[i];
      }
      text = search_text;
    }
  }
  if (wd_key_eq((const uint8_t *)command, wd_len(command), "mcp")) {
    handled = 1;
    if (args[0] == '\0') {
      text = "usage: /mcp <json request>";
    } else {
      static uint8_t callbuf2[1024];
      static const char cap_name2[] = "mcp.call";
      wd_put32(callbuf2, 8);
      for (uint32_t i = 0; i < 8; i++) callbuf2[4 + i] = (uint8_t)cap_name2[i];
      wd_w cw2;
      wd_w_init(&cw2, callbuf2 + 12, sizeof(callbuf2) - 12);
      wd_w_map(&cw2, 1);
      wd_w_textz(&cw2, "request");
      wd_w_textz(&cw2, args);
      uint32_t desc2[2];
      search_text[0] = '\0';
      if (!cw2.failed) {
        ward_cap((uint32_t)(uintptr_t)callbuf2, 12 + cw2.at, (uint32_t)(uintptr_t)desc2);
        const uint8_t *res2 = (const uint8_t *)(uintptr_t)desc2[0];
        wd_r rr2 = {res2, desc2[1], 0, 0};
        uint32_t rn2 = 0;
        if (wd_r_map(&rr2, &rn2) == 0) {
          for (uint32_t i = 0; i < rn2; i++) {
            const uint8_t *kp3 = 0;
            uint32_t kn3 = 0;
            if (wd_r_text_ptr(&rr2, &kp3, &kn3) != 0) break;
            if (wd_key_eq(kp3, kn3, "text")) {
              wd_r_text(&rr2, search_text, sizeof(search_text), 0);
            } else if (wd_r_skip(&rr2) != 0) {
              break;
            }
          }
        }
      }
      if (search_text[0] == '\0') {
        static const char none2[] = "the server said nothing";
        for (uint32_t i = 0; i < sizeof(none2); i++) search_text[i] = none2[i];
      }
      text = search_text;
    }
  }
  if (wd_key_eq((const uint8_t *)command, wd_len(command), "search")) {
    handled = 1;
    if (args[0] == '\0') {
      text = "usage: /search <terms>";
    } else {
      static uint8_t callbuf[1024];
      wd_put32(callbuf, 10);
      static const char cap_name[] = "web.search";
      for (uint32_t i = 0; i < 10; i++) callbuf[4 + i] = (uint8_t)cap_name[i];
      wd_w cw;
      wd_w_init(&cw, callbuf + 14, sizeof(callbuf) - 14);
      wd_w_map(&cw, 2);
      wd_w_textz(&cw, "query");
      wd_w_textz(&cw, args);
      wd_w_textz(&cw, "k");
      wd_w_textz(&cw, "5");
      uint32_t desc[2];
      search_text[0] = '\0';
      if (!cw.failed) {
        ward_cap((uint32_t)(uintptr_t)callbuf, 14 + cw.at, (uint32_t)(uintptr_t)desc);
        const uint8_t *res = (const uint8_t *)(uintptr_t)desc[0];
        wd_r rr = {res, desc[1], 0, 0};
        uint32_t rn = 0;
        if (wd_r_map(&rr, &rn) == 0) {
          for (uint32_t i = 0; i < rn; i++) {
            const uint8_t *kp2 = 0;
            uint32_t kn2 = 0;
            if (wd_r_text_ptr(&rr, &kp2, &kn2) != 0) break;
            if (wd_key_eq(kp2, kn2, "text")) {
              wd_r_text(&rr, search_text, sizeof(search_text), 0);
            } else if (wd_r_skip(&rr) != 0) {
              break;
            }
          }
        }
      }
      if (search_text[0] == '\0') {
        static const char none[] = "search returned nothing";
        for (uint32_t i = 0; i < sizeof(none); i++) search_text[i] = none[i];
      }
      text = search_text;
    }
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
