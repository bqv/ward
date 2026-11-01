/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "proto/sse.h"

#include <stdlib.h>
#include <string.h>

struct wd_sse {
  wd_sse_emit emit;
  void *ctx;
  char *line;
  size_t line_len;
  size_t line_cap;
  char *data;
  size_t data_len;
  size_t data_cap;
  char *type;
  size_t type_len;
  size_t type_cap;
  char *id;
  size_t id_len;
  size_t id_cap;
  unsigned char head[3];
  size_t head_len;
  bool head_done;
  bool after_cr;
  bool failed;
};

static int reserve(char **buf, size_t *cap, size_t need, size_t max) {
  if (need > max) return -1;
  if (*cap >= need) return 0;
  size_t next = *cap ? *cap : 64;
  while (next < need) next *= 2;
  if (next > max) next = max;
  char *grown = realloc(*buf, next);
  if (grown == NULL) return -1;
  *buf = grown;
  *cap = next;
  return 0;
}

static int append(char **buf, size_t *len, size_t *cap, size_t max,
                  const char *src, size_t n) {
  if (n == 0) return 0;
  if (reserve(buf, cap, *len + n, max) != 0) return -1;
  memcpy(*buf + *len, src, n);
  *len += n;
  return 0;
}

static int set(char **buf, size_t *len, size_t *cap, size_t max,
               const char *src, size_t n) {
  *len = 0;
  return append(buf, len, cap, max, src, n);
}

static int field_is(const char *line, size_t len, const char *name) {
  size_t name_len = strlen(name);
  return len == name_len && memcmp(line, name, name_len) == 0;
}

static int dispatch(wd_sse *s) {
  if (s->data_len == 0) {
    s->type_len = 0;
    return 0;
  }
  size_t data_len = s->data_len;
  if (s->data[data_len - 1] == '\n') data_len--;
  wd_sse_event event;
  event.type = s->type_len ? s->type : "message";
  event.type_len = s->type_len ? s->type_len : strlen("message");
  event.data = s->data;
  event.data_len = data_len;
  event.id = s->id_len ? s->id : NULL;
  event.id_len = s->id_len;
  s->data_len = 0;
  s->type_len = 0;
  if (s->emit != NULL && s->emit(s->ctx, &event) != 0) {
    s->failed = true;
    return -1;
  }
  return 0;
}

static int field(wd_sse *s, const char *line, size_t len) {
  if (len == 0) return dispatch(s);
  if (line[0] == ':') return 0;
  size_t colon = 0;
  while (colon < len && line[colon] != ':') colon++;
  const char *value = "";
  size_t value_len = 0;
  if (colon < len) {
    value = line + colon + 1;
    value_len = len - colon - 1;
    if (value_len > 0 && value[0] == ' ') {
      value++;
      value_len--;
    }
  }
  if (field_is(line, colon, "data")) {
    if (append(&s->data, &s->data_len, &s->data_cap, WD_SSE_MAX_DATA, value,
               value_len) != 0)
      return -1;
    return append(&s->data, &s->data_len, &s->data_cap, WD_SSE_MAX_DATA, "\n",
                  1);
  }
  if (field_is(line, colon, "event"))
    return set(&s->type, &s->type_len, &s->type_cap, WD_SSE_MAX_TYPE, value,
               value_len);
  if (field_is(line, colon, "id")) {
    if (value_len > 0 && memchr(value, '\0', value_len) != NULL) return 0;
    return set(&s->id, &s->id_len, &s->id_cap, WD_SSE_MAX_ID, value, value_len);
  }
  return 0;
}

static int end_line(wd_sse *s) {
  int rc = field(s, s->line, s->line_len);
  s->line_len = 0;
  return rc;
}

static int byte(wd_sse *s, unsigned char c) {
  if (c == '\n') {
    if (s->after_cr) {
      s->after_cr = false;
      return 0;
    }
    return end_line(s);
  }
  if (c == '\r') {
    int rc = end_line(s);
    s->after_cr = true;
    return rc;
  }
  s->after_cr = false;
  return append(&s->line, &s->line_len, &s->line_cap, WD_SSE_MAX_LINE,
                (const char *)&c, 1);
}

static int head(wd_sse *s, const char *buf, size_t len, size_t *i) {
  static const unsigned char bom[3] = {0xef, 0xbb, 0xbf};
  while (*i < len && s->head_len < 3) {
    unsigned char c = (unsigned char)buf[*i];
    if (c != bom[s->head_len]) {
      s->head_done = true;
      for (size_t k = 0; k < s->head_len; k++)
        if (byte(s, s->head[k]) != 0) return -1;
      s->head_len = 0;
      return 0;
    }
    s->head[s->head_len++] = c;
    (*i)++;
    if (s->head_len == 3) {
      s->head_done = true;
      s->head_len = 0;
    }
  }
  return 0;
}

wd_sse *wd_sse_create(wd_sse_emit emit, void *ctx) {
  wd_sse *s = calloc(1, sizeof(*s));
  if (s == NULL) return NULL;
  s->emit = emit;
  s->ctx = ctx;
  return s;
}

void wd_sse_destroy(wd_sse *sse) {
  if (sse == NULL) return;
  free(sse->line);
  free(sse->data);
  free(sse->type);
  free(sse->id);
  free(sse);
}

bool wd_sse_failed(const wd_sse *sse) { return sse == NULL || sse->failed; }

int wd_sse_feed(wd_sse *sse, const char *buf, size_t len) {
  if (sse == NULL || sse->failed) return -1;
  if (buf == NULL && len != 0) {
    sse->failed = true;
    return -1;
  }
  size_t i = 0;
  while (!sse->head_done && i < len) {
    if (head(sse, buf, len, &i) != 0) {
      sse->failed = true;
      return -1;
    }
  }
  while (i < len) {
    if (byte(sse, (unsigned char)buf[i]) != 0) {
      sse->failed = true;
      return -1;
    }
    i++;
  }
  return 0;
}

int wd_sse_finish(wd_sse *sse) {
  if (sse == NULL || sse->failed) return -1;
  sse->line_len = 0;
  sse->data_len = 0;
  sse->type_len = 0;
  sse->after_cr = false;
  return 0;
}
