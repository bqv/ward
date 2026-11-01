/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE (256u * 1024u)
#define REPLY_SIZE 4096u
#define TEXT_SIZE 2048u
#define JSON_SIZE 4096u
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
  uint8_t prompt[JSON_SIZE];
  uint32_t prompt_len = 0;
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "prompt")) {
      if (wd_r_text(&r, (char *)prompt, sizeof(prompt), &prompt_len) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  char escaped[JSON_SIZE];
  uint32_t escaped_len = json_escape(escaped, sizeof(escaped), prompt, prompt_len);

  static const char prefix[] =
      "{\"model\":\"local\",\"stream\":true,\"messages\":[{\"role\":\"user\","
      "\"content\":\"";
  static const char suffix[] = "\"}]}";
  char json[JSON_SIZE];
  uint32_t jl = 0;
  for (uint32_t i = 0; prefix[i] != '\0' && jl < sizeof(json); i++) json[jl++] = prefix[i];
  for (uint32_t i = 0; i < escaped_len && jl < sizeof(json); i++) json[jl++] = escaped[i];
  for (uint32_t i = 0; suffix[i] != '\0' && jl < sizeof(json); i++) json[jl++] = suffix[i];

  wd_w w;
  wd_w_init(&w, callbuf, CALL_SIZE);
  static const char cap_name[] = "net.request";
  wd_put32(callbuf, sizeof(cap_name) - 1);
  uint32_t name_len = sizeof(cap_name) - 1;
  for (uint32_t i = 0; i < name_len; i++) callbuf[4 + i] = (uint8_t)cap_name[i];
  wd_w_init(&w, callbuf + 4 + name_len, CALL_SIZE - 4 - name_len);
  wd_w_map(&w, 2);
  wd_w_textz(&w, "path");
  wd_w_textz(&w, "/v1/chat/completions");
  wd_w_textz(&w, "body");
  wd_w_text(&w, json, jl);
  if (w.failed) {
    wd_w_init(&w, reply, REPLY_SIZE);
    wd_w_map(&w, 1);
    wd_w_textz(&w, "text");
    wd_w_textz(&w, "");
    pair(out, reply, w.at);
    return;
  }

  uint32_t desc[2];
  ward_cap((uint32_t)(uintptr_t)callbuf, 4 + name_len + w.at,
           (uint32_t)(uintptr_t)desc);
  const uint8_t *res = (const uint8_t *)(uintptr_t)desc[0];
  uint32_t res_len = desc[1];

  uint32_t text_at = 0;
  answer[0] = '\0';
  if (res_len > 4 && wd_get32(res) == 200)
    text_at = scan_content(res + 4, res_len - 4);

  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "text");
  wd_w_text(&w, answer, text_at);
  if (!w.failed) pair(out, reply, w.at);
}
