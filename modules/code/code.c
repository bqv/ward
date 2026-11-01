/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 65536u
#define REPLY_SIZE 4096u
#define TEXT_SIZE 16384u
#define SYMBOL_MAX 16u
#define NAME_MAX 48u

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

static int is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_space(char c) {
  return c == ' ' || c == '\t';
}

static int keyword(const char *word, uint32_t len) {
  static const char *const words[] = {"if",   "for",   "while", "switch",
                                      "return", "else", "do",   "sizeof"};
  for (uint32_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    size_t n = wd_len(words[i]);
    if (n != len) continue;
    uint32_t k = 0;
    while (k < len && words[i][k] == word[k]) k++;
    if (k == len) return 1;
  }
  return 0;
}

static uint32_t scan(const char *text, uint32_t cap,
                     char names[SYMBOL_MAX][NAME_MAX], uint32_t *lines) {
  uint32_t found = 0;
  uint32_t line = 1;
  uint32_t i = 0;
  while (i < cap && text[i] != '\0') {
    char c = text[i];
    if (c == '\n') {
      line++;
      i++;
      continue;
    }
    if (!is_alpha(c)) {
      i++;
      continue;
    }
    uint32_t start = i;
    uint32_t len = 0;
    while (i < cap && (is_alpha(text[i]) || (text[i] >= '0' && text[i] <= '9'))) {
      i++;
      len++;
    }
    uint32_t j = i;
    while (j < cap && is_space(text[j])) j++;
    if (j < cap && text[j] == '(' && len < NAME_MAX &&
        !keyword(text + start, len) && found < SYMBOL_MAX) {
      uint32_t k = j;
      while (k < cap && text[k] != ')' && text[k] != ';' && text[k] != '}' &&
             k - j < 400)
        k++;
      if (k < cap && text[k] == ')') {
        uint32_t m = k + 1;
        while (m < cap && is_space(text[m])) m++;
        if (m < cap && text[m] == '{') {
          uint32_t n = 0;
          while (n < len) {
            names[found][n] = text[start + n];
            n++;
          }
          names[found][n] = '\0';
          lines[found] = line;
          found++;
        }
      }
    }
  }
  return found;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char text[TEXT_SIZE];
  text[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "text")) {
      if (wd_r_text(&r, text, sizeof(text), 0) != 0) return;
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  static char names[SYMBOL_MAX][NAME_MAX];
  static uint32_t lines[SYMBOL_MAX];
  uint32_t found = scan(text, (uint32_t)wd_len(text), names, lines);

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "symbols");
  wd_w_array(&w, found);
  for (uint32_t i = 0; i < found; i++) {
    wd_w_map(&w, 3);
    wd_w_textz(&w, "name");
    wd_w_textz(&w, names[i]);
    wd_w_textz(&w, "kind");
    wd_w_textz(&w, "function");
    wd_w_textz(&w, "line");
    static char digits[16];
    uint32_t v = lines[i], k = 0;
    char tmp[16];
    if (v == 0) tmp[k++] = '0';
    while (v > 0 && k < sizeof(tmp)) {
      tmp[k++] = (char)('0' + (v % 10));
      v /= 10;
    }
    uint32_t d = 0;
    while (k > 0) digits[d++] = tmp[--k];
    digits[d] = '\0';
    wd_w_textz(&w, digits);
  }
  if (w.failed) return;
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, (uint32_t)(uintptr_t)reply);
  wd_put32(o + 4, w.at);
}
