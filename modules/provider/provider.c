/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdio.h>

#include "wd_abi.h"

#define HEAP_SIZE (1024u * 1024u)
#define REPLY_SIZE 24576u
#define TEXT_SIZE 2048u
#define JSON_SIZE 65536u
#define CALL_SIZE 8192u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static uint8_t callbuf[CALL_SIZE];
static char answer[TEXT_SIZE];

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

static uint32_t json_escape(char *out, uint32_t cap, const uint8_t *in,
                            uint32_t len) {
  static const char hex[] = "0123456789abcdef";
  uint32_t at = 0;
  for (uint32_t i = 0; i < len && at + 7 < cap; i++) {
    uint8_t c = in[i];
    if (c == '"' || c == '\\') {
      out[at++] = '\\';
      out[at++] = (char)c;
    } else if (c == '\n') {
      out[at++] = '\\';
      out[at++] = 'n';
    } else if (c == '\r') {
      out[at++] = '\\';
      out[at++] = 'r';
    } else if (c == '\t') {
      out[at++] = '\\';
      out[at++] = 't';
    } else if (c < 0x20) {
      out[at++] = '\\';
      out[at++] = 'u';
      out[at++] = '0';
      out[at++] = '0';
      out[at++] = hex[(c >> 4) & 0xf];
      out[at++] = hex[c & 0xf];
    } else {
      out[at++] = (char)c;
    }
  }
  return at;
}

static void append_text(uint32_t *at, const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n && *at + 1 < TEXT_SIZE; i++) answer[(*at)++] = (char)p[i];
  answer[*at] = '\0';
}

static int hex_value(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static uint32_t scan_content(const uint8_t *body, uint32_t len) {
  static const char key[] = "\"content\":\"";
  uint32_t key_len = sizeof(key) - 1;
  uint32_t at = 0;
  answer[0] = '\0';
  for (uint32_t i = 0; i + key_len <= len; i++) {
    uint32_t k = 0;
    while (k < key_len && body[i + k] == (uint8_t)key[k]) k++;
    if (k != key_len) continue;
    uint32_t j = i + key_len;
    while (j < len && body[j] != '"') {
      if (body[j] == '\\' && j + 1 < len) {
        uint8_t e = body[j + 1];
        if (e == 'n') {
          append_text(&at, (const uint8_t *)"\n", 1);
        } else if (e == 't') {
          append_text(&at, (const uint8_t *)"\t", 1);
        } else if (e == 'u' && j + 5 < len) {
          int h1 = hex_value(body[j + 2]);
          int h2 = hex_value(body[j + 3]);
          int h3 = hex_value(body[j + 4]);
          int h4 = hex_value(body[j + 5]);
          if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
            uint32_t cp = (uint32_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
            uint8_t byte = cp < 0x80 ? (uint8_t)cp : (uint8_t)'?';
            append_text(&at, &byte, 1);
          }
          j += 6;
          continue;
        } else {
          append_text(&at, &e, 1);
        }
        j += 2;
        continue;
      }
      append_text(&at, body + j, 1);
      j++;
    }
    i = j;
  }
  return at;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  static uint8_t prompt[JSON_SIZE];
  uint32_t prompt_len = 0;
  static char body[JSON_SIZE];
  uint32_t body_len = 0;
  uint32_t status = 0;
  int have_prompt = 0;
  int have_body = 0;
  char model[128];
  model[0] = '\0';
  char max_tokens[16];
  {
    static const char fallback[] = "1024";
    uint32_t i = 0;
    while (fallback[i] != '\0' && i + 1 < sizeof(max_tokens)) {
      max_tokens[i] = fallback[i];
      i++;
    }
    max_tokens[i] = '\0';
  }
  const uint8_t *hist_text[32];
  uint32_t hist_text_len[32];
  const uint8_t *hist_role[32];
  uint32_t hist_role_len[32];
  uint32_t hist_count = 0;
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "prompt")) {
      if (wd_r_text(&r, (char *)prompt, sizeof(prompt), &prompt_len) != 0) return;
      have_prompt = 1;
    } else if (wd_key_eq(kp, kn, "model")) {
      if (wd_r_text(&r, model, sizeof(model), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "max_tokens")) {
      if (wd_r_text(&r, max_tokens, sizeof(max_tokens), 0) != 0) return;
    } else if (wd_key_eq(kp, kn, "body")) {
      if (wd_r_text(&r, body, sizeof(body), &body_len) != 0) return;
      have_body = 1;
    } else if (wd_key_eq(kp, kn, "history")) {
      uint32_t hn = 0;
      if (wd_r_array(&r, &hn) != 0) return;
      for (uint32_t e = 0; e < hn && hist_count < 32; e++) {
        uint32_t mn = 0;
        if (wd_r_map(&r, &mn) != 0) return;
        const uint8_t *tp = 0;
        uint32_t tn = 0;
        const uint8_t *rp = 0;
        uint32_t rn = 0;
        for (uint32_t k = 0; k < mn; k++) {
          const uint8_t *hk = 0;
          uint32_t hl = 0;
          if (wd_r_text_ptr(&r, &hk, &hl) != 0) return;
          if (wd_key_eq(hk, hl, "role")) {
            if (wd_r_text_ptr(&r, &rp, &rn) != 0) return;
          } else if (wd_key_eq(hk, hl, "text")) {
            if (wd_r_text_ptr(&r, &tp, &tn) != 0) return;
          } else if (wd_r_skip(&r) != 0) {
            return;
          }
        }
        if (tp != 0 && rp != 0) {
          hist_text[hist_count] = tp;
          hist_text_len[hist_count] = tn;
          hist_role[hist_count] = rp;
          hist_role_len[hist_count] = rn;
          hist_count++;
        }
      }
    } else if (wd_key_eq(kp, kn, "status")) {
      uint32_t value = 0;
      if (wd_r_uint(&r, &value) != 0) return;
      status = value;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  wd_w w;
  if (have_body) {
    uint32_t text_at = 0;
    answer[0] = '\0';
    if (status == 200)
      text_at = scan_content((const uint8_t *)body, body_len);
    wd_w_init(&w, reply, REPLY_SIZE);
    wd_w_map(&w, 2);
    wd_w_textz(&w, "text");
    wd_w_text(&w, answer, text_at);
    wd_w_textz(&w, "status");
    wd_w_uint(&w, status);
    if (!w.failed) pair(out, reply, w.at);
    return;
  }
  if (!have_prompt) return;

  static const char prefix[] =
      "{\"model\":\"";
  static char json[JSON_SIZE];
  uint32_t jl = 0;
  for (uint32_t i = 0; prefix[i] != '\0' && jl < sizeof(json); i++)
    json[jl++] = prefix[i];
  {
    const char *chosen = model[0] != '\0' ? model : "local";
    for (uint32_t i = 0; chosen[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = chosen[i];
    static const char rest[] = "\",\"stream\":true,\"max_tokens\":";
    for (uint32_t i = 0; rest[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = rest[i];
    for (uint32_t i = 0; max_tokens[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = max_tokens[i];
    static const char messages[] = ",\"messages\":[";
    for (uint32_t i = 0; messages[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = messages[i];
  }
  for (uint32_t e = 0; e < hist_count; e++) {
    static const char role_open[] = "{\"role\":\"";
    static const char content_open[] = "\",\"content\":\"";
    for (uint32_t i = 0; role_open[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = role_open[i];
    for (uint32_t i = 0; i < hist_role_len[e] && jl < sizeof(json); i++)
      json[jl++] = (char)hist_role[e][i];
    for (uint32_t i = 0; content_open[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = content_open[i];
    jl += json_escape(json + jl, sizeof(json) - jl, hist_text[e], hist_text_len[e]);
    if (jl + 2 < sizeof(json)) {
      json[jl++] = '"';
      json[jl++] = '}';
      json[jl++] = ',';
    }
  }
  {
    static const char user_open[] = "{\"role\":\"user\",\"content\":\"";
    static const char suffix[] = "\"}]}";
    for (uint32_t i = 0; user_open[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = user_open[i];
    jl += json_escape(json + jl, sizeof(json) - jl, prompt, prompt_len);
    for (uint32_t i = 0; suffix[i] != '\0' && jl < sizeof(json); i++)
      json[jl++] = suffix[i];
  }

  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "request");
  wd_w_map(&w, 2);
  wd_w_textz(&w, "path");
  wd_w_textz(&w, "/v1/chat/completions");
  wd_w_textz(&w, "body");
  wd_w_text(&w, json, jl);
  if (!w.failed) pair(out, reply, w.at);
}
