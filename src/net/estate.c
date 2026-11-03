/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "net/estate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/http.h"

static const char *after(const char *at, const char *key) {
  const char *found = strstr(at, key);
  return found != NULL ? found + strlen(key) : NULL;
}

static const char *skip_ws(const char *at) {
  while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
  return at;
}

static const char *value_after(const char *at, const char *key) {
  const char *found = after(at, key);
  if (found == NULL) return NULL;
  found = skip_ws(found);
  if (*found != ':') return NULL;
  return skip_ws(found + 1);
}

static const char *array_after(const char *at, const char *key) {
  const char *value = value_after(at, key);
  if (value == NULL || *value != '[') return NULL;
  return value + 1;
}

static int read_string(const char *at, char *out, size_t cap,
                       const char **next) {
  at = skip_ws(at);
  if (*at != '"') return -1;
  const char *close = strchr(at + 1, '"');
  if (close == NULL) return -1;
  size_t len = (size_t)(close - at - 1);
  if (len + 1 > cap) return -1;
  memcpy(out, at + 1, len);
  out[len] = '\0';
  if (next != NULL) *next = close + 1;
  return 0;
}

static void url_escape(const char *in, char *out, size_t cap) {
  static const char hex[] = "0123456789ABCDEF";
  size_t at = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
    bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                 *p == '.' || *p == '~';
    if (plain) {
      if (at + 1 >= cap) break;
      out[at++] = (char)*p;
    } else {
      if (at + 3 >= cap) break;
      out[at++] = '%';
      out[at++] = hex[*p >> 4];
      out[at++] = hex[*p & 0x0f];
    }
  }
  out[at] = '\0';
}

int wd_estate_parse(const char *json, wd_estate_card *out) {
  if (json == NULL || out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  const char *group = value_after(json, "\"group\"");
  if (group == NULL ||
      read_string(group, out->group, sizeof(out->group), NULL) != 0)
    return -1;
  const char *since = value_after(json, "\"since_ms\"");
  if (since != NULL) {
    char *end = NULL;
    unsigned long long value = strtoull(since, &end, 10);
    if (end != since) out->since_ms = (uint64_t)value;
  }
  const char *holders = array_after(json, "\"holders\"");
  if (holders != NULL) {
    const char *at = holders;
    while (out->holders_len < WD_ESTATE_HOLDERS) {
      at = skip_ws(at);
      if (*at == ']') break;
      if (read_string(at, out->holders[out->holders_len],
                      sizeof(out->holders[0]), &at) != 0)
        break;
      out->holders_len++;
      at = skip_ws(at);
      if (*at != ',') break;
      at++;
    }
  }
  const char *slots = array_after(json, "\"slots\"");
  if (slots != NULL) {
    const char *at = slots;
    while (out->slots_len < WD_ESTATE_SLOTS) {
      const char *end = strchr(at, ']');
      const char *name = value_after(at, "\"name\"");
      if (end == NULL || name == NULL || name > end) break;
      const char *object = strchr(name, '}');
      if (object == NULL || object > end) break;
      wd_estate_slot *slot = &out->slots[out->slots_len];
      if (read_string(name, slot->name, sizeof(slot->name), NULL) != 0) break;
      const char *state = value_after(name, "\"state\"");
      if (state == NULL || state > object ||
          read_string(state, slot->state, sizeof(slot->state), NULL) != 0)
        break;
      out->slots_len++;
      at = object + 1;
    }
  }
  return 0;
}

int wd_estate_card_read(const char *socket_path, const char *group,
                        wd_estate_card *out, char **err) {
  if (socket_path == NULL || out == NULL) {
    if (err != NULL) *err = strdup("estate: no socket configured");
    return -1;
  }
  char url[256];
  if (group != NULL && group[0] != '\0') {
    char escaped[128];
    url_escape(group, escaped, sizeof(escaped));
    snprintf(url, sizeof(url), "http://127.0.0.1/estate/card?group=%s",
             escaped);
  } else {
    snprintf(url, sizeof(url), "http://127.0.0.1/estate/card");
  }
  wd_http_set_unix_socket(socket_path);
  wd_http_response response;
  int rc = wd_http_request(url, "GET", NULL, NULL, 5000, 64u << 10, &response,
                           err);
  wd_http_set_unix_socket(NULL);
  if (rc != 0) return -1;
  if (response.status != 200) {
    wd_http_response_free(&response);
    if (err != NULL) {
      char detail[64];
      snprintf(detail, sizeof(detail), "estate: the socket answered %ld",
               response.status);
      *err = strdup(detail);
    }
    return -1;
  }
  int parsed = wd_estate_parse((const char *)response.body, out);
  wd_http_response_free(&response);
  if (parsed != 0) {
    if (err != NULL) *err = strdup("estate: the document could not be read");
    return -1;
  }
  return 0;
}
