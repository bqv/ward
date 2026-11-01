/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/probe.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int wd_context_from_text(const char *text) {
  if (text == NULL || text[0] == '\0') return 0;
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (value <= 0 || end == text) return 0;
  if (end != NULL) {
    if (*end == 'k' || *end == 'K') value *= 1024;
    else if (*end == 'm' || *end == 'M') value *= 1024 * 1024;
  }
  if (value <= 0 || value > 100000000) return 0;
  return (int)value;
}

static int number_after(const char *json, size_t len, const char *key,
                        int *out) {
  size_t key_len = strlen(key);
  for (size_t i = 0; i + key_len < len; i++) {
    if (strncmp(json + i, key, key_len) != 0) continue;
    size_t j = i + key_len;
    while (j < len && (json[j] == ' ' || json[j] == ':')) j++;
    if (j >= len || !isdigit((unsigned char)json[j])) continue;
    long value = 0;
    while (j < len && isdigit((unsigned char)json[j])) {
      value = value * 10 + (json[j] - '0');
      if (value > 100000000) return 0;
      j++;
    }
    *out = (int)value;
    return 1;
  }
  return 0;
}

static int arg_number(const char *json, size_t len, size_t from,
                      const char *flag, int *out) {
  size_t flag_len = strlen(flag);
  for (size_t i = from; i + flag_len + 3 < len; i++) {
    if (strncmp(json + i, flag, flag_len) != 0) continue;
    size_t j = i + flag_len;
    while (j < len && json[j] != '"') j++;
    if (j >= len) return 0;
    j++;
    char digits[16];
    size_t k = 0;
    while (j < len && json[j] != '"' && k + 1 < sizeof(digits)) {
      digits[k++] = json[j++];
    }
    digits[k] = '\0';
    int value = atoi(digits);
    if (value <= 0) return 0;
    *out = value;
    return 1;
  }
  return 0;
}

int wd_probe_router_models(const char *json, const char *want, int *n_ctx,
                           int *slots) {
  if (json == NULL || want == NULL) return -1;
  size_t len = strlen(json);
  size_t at = 0;
  size_t want_len = strlen(want);
  int found = 0;
  for (size_t i = 0; i + want_len + 8 < len; i++) {
    if (strncmp(json + i, "\"id\":\"", 6) != 0) continue;
    if (strncmp(json + i + 6, want, want_len) != 0) continue;
    if (json[i + 6 + want_len] != '"') continue;
    at = i + 6 + want_len;
    found = 1;
    break;
  }
  if (!found) return -1;
  int value = 0;
  int any = 0;
  if (n_ctx != NULL && arg_number(json, len, at, "\"--ctx-size\"", &value)) {
    *n_ctx = value;
    any = 1;
  }
  value = 0;
  if (slots != NULL && arg_number(json, len, at, "\"--parallel\"", &value)) {
    *slots = value;
    any = 1;
  }
  return any ? 0 : -1;
}

int wd_probe_scan(const char *json, int *n_ctx, int *slots) {
  if (json == NULL) return -1;
  size_t len = strlen(json);
  int found = 0;
  int value = 0;
  if (n_ctx != NULL && number_after(json, len, "\"n_ctx\"", &value)) {
    *n_ctx = value;
    found = 1;
  }
  value = 0;
  if (slots != NULL && number_after(json, len, "\"total_slots\"", &value)) {
    *slots = value;
    found = 1;
  }
  return found ? 0 : -1;
}
