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

static int read_string(const char *at, char *out, size_t cap) {
  const char *open = strchr(at, '"');
  if (open == NULL) return -1;
  const char *close = strchr(open + 1, '"');
  if (close == NULL) return -1;
  size_t len = (size_t)(close - open - 1);
  if (len >= cap) len = cap - 1;
  memcpy(out, open + 1, len);
  out[len] = '\0';
  return 0;
}

int wd_estate_parse(const char *json, wd_estate_card *out) {
  if (json == NULL || out == NULL) return -1;
  memset(out, 0, sizeof(*out));
  const char *group = after(json, "\"group\":");
  if (group == NULL || read_string(group, out->group, sizeof(out->group)) != 0)
    return -1;
  const char *since = after(json, "\"since_ms\":");
  if (since != NULL) out->since_ms = (uint64_t)strtoull(since, NULL, 10);
  const char *holders = after(json, "\"holders\":[");
  if (holders != NULL) {
    const char *at = holders;
    while (out->holders_len < WD_ESTATE_HOLDERS) {
      const char *comma = strchr(at, ',');
      const char *end = strchr(at, ']');
      if (end == NULL) break;
      if (at >= end) break;
      if (*at == '"') {
        if (read_string(at, out->holders[out->holders_len],
                        sizeof(out->holders[0])) != 0)
          break;
        out->holders_len++;
      }
      if (comma == NULL || comma > end) break;
      at = comma + 1;
    }
  }
  const char *slots = after(json, "\"slots\":[");
  if (slots != NULL) {
    const char *at = slots;
    while (out->slots_len < WD_ESTATE_SLOTS) {
      const char *end = strchr(at, ']');
      const char *name = after(at, "\"name\":");
      if (end == NULL || name == NULL || name > end) break;
      wd_estate_slot *slot = &out->slots[out->slots_len];
      if (read_string(name, slot->name, sizeof(slot->name)) != 0) break;
      const char *state = after(name, "\"state\":");
      if (state == NULL || read_string(state, slot->state, sizeof(slot->state)) != 0)
        break;
      out->slots_len++;
      const char *next = strchr(state, '}');
      if (next == NULL) break;
      at = next + 1;
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
  snprintf(url, sizeof(url), "http://demesne/estate/card%s%s",
           group != NULL && group[0] != '\0' ? "?group=" : "",
           group != NULL && group[0] != '\0' ? group : "");
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
