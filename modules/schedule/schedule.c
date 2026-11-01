/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE (64u * 1024u)
#define REPLY_SIZE 256u

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

static int plain_number(const char *text, uint8_t *values, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) values[i] = 0;
  uint32_t at = 0;
  while (text[at] != '\0') {
    while (text[at] == ' ') at++;
    if (text[at] == '\0') break;
    if (text[at] == '*') {
      at++;
      uint32_t step = 1;
      if (text[at] == '/') {
        at++;
        step = 0;
        while (text[at] >= '0' && text[at] <= '9')
          step = step * 10 + (uint32_t)(text[at++] - '0');
        if (step == 0) step = 1;
      }
      for (uint32_t k = 0; k < n; k += step) values[k] = 1;
    } else {
      uint32_t v = 0;
      while (text[at] >= '0' && text[at] <= '9')
        v = v * 10 + (uint32_t)(text[at++] - '0');
      if (v >= n) return -1;
      values[v] = 1;
    }
    while (text[at] == ' ') at++;
    if (text[at] == ',') at++;
  }
  return 0;
}

static void civil(uint64_t days, uint32_t *month, uint32_t *day) {
  int64_t z = (int64_t)days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  char cron[128];
  uint32_t cron_len = 0;
  uint64_t from = 0;
  int have_cron = 0;
  if (wd_r_map(&r, &n) != 0) return;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "cron")) {
      if (wd_r_text(&r, cron, sizeof(cron), &cron_len) != 0) return;
      have_cron = 1;
    } else if (wd_key_eq(kp, kn, "from_ms")) {
      char text[32];
      uint32_t text_len = 0;
      if (wd_r_text(&r, text, sizeof(text), &text_len) != 0) return;
      for (uint32_t k = 0; k < text_len && text[k] >= '0' && text[k] <= '9'; k++)
        from = from * 10 + (uint64_t)(text[k] - '0');
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }

  uint64_t found = 0;
  if (have_cron && cron[0] == '@') {
    static const char every[] = "@every ";
    uint32_t k = 0;
    while (every[k] != '\0' && cron[k] == every[k]) k++;
    if (every[k] == '\0') {
      uint64_t step = 0;
      for (uint32_t i = k; cron[i] >= '0' && cron[i] <= '9'; i++)
        step = step * 10 + (uint64_t)(cron[i] - '0');
      if (step > 0) found = from + step;
    }
  } else if (have_cron) {
    char fields[5][32];
    uint32_t fi = 0;
    uint32_t ci = 0;
    for (uint32_t i = 0; i < 5; i++) fields[i][0] = '\0';
    while (cron[ci] != '\0' && fi < 5) {
      while (cron[ci] == ' ') ci++;
      uint32_t k = 0;
      while (cron[ci] != '\0' && cron[ci] != ' ' && k + 1 < sizeof(fields[0]))
        fields[fi][k++] = cron[ci++];
      fields[fi][k] = '\0';
      if (k > 0) fi++;
    }
    if (fi == 5) {
      uint8_t mins[60], hours[24], doms[32], months[13], dows[7];
      int valid = plain_number(fields[0], mins, 60) == 0 &&
                  plain_number(fields[1], hours, 24) == 0 &&
                  plain_number(fields[2], doms, 32) == 0 &&
                  plain_number(fields[3], months, 13) == 0 &&
                  plain_number(fields[4], dows, 7) == 0;
      uint64_t minute = from / 60000u + 1;
      if (!valid) minute = 0;
      for (uint64_t step = 0; minute != 0 && step < 4u * 366u * 24u * 60u; step++) {
        uint64_t m = minute + step;
        uint64_t days = m / 1440u;
        uint32_t hod = (uint32_t)(m % 1440u);
        uint32_t mon = 0, day = 0;
        civil(days, &mon, &day);
        if (!months[mon] || !doms[day] || !dows[(days + 4) % 7]) continue;
        if (!hours[hod / 60] || !mins[hod % 60]) continue;
        found = m * 60000u;
        break;
      }
    }
  }

  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "next_ms");
  char text[24];
  uint32_t tl = 0;
  uint64_t value = found;
  if (value == 0) {
    text[tl++] = '0';
  } else {
    char digits[24];
    uint32_t dn = 0;
    while (value > 0 && dn < sizeof(digits)) {
      digits[dn++] = (char)('0' + (value % 10));
      value /= 10;
    }
    while (dn > 0) text[tl++] = digits[--dn];
  }
  text[tl] = '\0';
  wd_w_textz(&w, text);
  if (!w.failed) pair(out, w.at);
}
