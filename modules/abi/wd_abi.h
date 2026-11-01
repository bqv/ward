/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_MODULE_ABI_H
#define WARD_MODULE_ABI_H

#include <stdint.h>

typedef struct {
  uint8_t *buf;
  uint32_t cap;
  uint32_t at;
  int failed;
} wd_w;

typedef struct {
  const uint8_t *buf;
  uint32_t len;
  uint32_t at;
  int failed;
} wd_r;

static void wd_w_init(wd_w *w, void *buf, uint32_t cap) {
  w->buf = (uint8_t *)buf;
  w->cap = cap;
  w->at = 0;
  w->failed = 0;
}

static void wd_w_byte(wd_w *w, uint8_t b) {
  if (w->at >= w->cap) {
    w->failed = 1;
    return;
  }
  w->buf[w->at++] = b;
}

static void wd_w_head(wd_w *w, uint8_t major, uint32_t v) {
  if (v < 24) {
    wd_w_byte(w, (uint8_t)(major << 5 | v));
  } else if (v < 256) {
    wd_w_byte(w, (uint8_t)(major << 5 | 24));
    wd_w_byte(w, (uint8_t)v);
  } else if (v < 65536) {
    wd_w_byte(w, (uint8_t)(major << 5 | 25));
    wd_w_byte(w, (uint8_t)(v >> 8));
    wd_w_byte(w, (uint8_t)(v & 0xff));
  } else {
    wd_w_byte(w, (uint8_t)(major << 5 | 26));
    for (int i = 3; i >= 0; i--) wd_w_byte(w, (uint8_t)((v >> (8 * i)) & 0xff));
  }
}

static void wd_w_uint(wd_w *w, uint32_t v) { wd_w_head(w, 0, v); }

static void wd_w_text(wd_w *w, const char *s, uint32_t n) {
  wd_w_head(w, 3, n);
  for (uint32_t i = 0; i < n; i++) wd_w_byte(w, (uint8_t)s[i]);
}

static uint32_t wd_len(const char *s) {
  uint32_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

static void wd_w_textz(wd_w *w, const char *s) { wd_w_text(w, s, wd_len(s)); }

static void wd_w_array(wd_w *w, uint32_t n) { wd_w_head(w, 4, n); }

static void wd_w_map(wd_w *w, uint32_t n) { wd_w_head(w, 5, n); }

static void wd_w_bool(wd_w *w, int v) { wd_w_byte(w, v ? 0xf5 : 0xf4); }

static uint8_t wd_r_byte(wd_r *r) {
  if (r->at >= r->len) {
    r->failed = 1;
    return 0;
  }
  return r->buf[r->at++];
}

static int wd_r_head(wd_r *r, uint8_t *major, uint32_t *v) {
  uint8_t b = wd_r_byte(r);
  if (r->failed) return -1;
  *major = (uint8_t)(b >> 5);
  uint8_t info = b & 0x1f;
  if (info < 24) {
    *v = info;
    return 0;
  }
  if (info == 24) {
    *v = wd_r_byte(r);
  } else if (info == 25) {
    uint32_t hi = wd_r_byte(r);
    uint32_t lo = wd_r_byte(r);
    *v = (hi << 8) | lo;
  } else if (info == 26) {
    uint32_t acc = 0;
    for (int i = 0; i < 4; i++) acc = (acc << 8) | wd_r_byte(r);
    *v = acc;
  } else if (info == 27) {
    uint32_t acc = 0;
    for (int i = 0; i < 4; i++) (void)wd_r_byte(r);
    for (int i = 0; i < 4; i++) acc = (acc << 8) | wd_r_byte(r);
    *v = acc;
  } else {
    r->failed = 1;
    return -1;
  }
  return r->failed ? -1 : 0;
}

static int wd_r_uint(wd_r *r, uint32_t *out) {
  uint8_t major = 0;
  uint32_t v = 0;
  if (wd_r_head(r, &major, &v) != 0 || major != 0) {
    r->failed = 1;
    return -1;
  }
  *out = v;
  return 0;
}

static int wd_r_text_ptr(wd_r *r, const uint8_t **p, uint32_t *n) {
  uint8_t major = 0;
  uint32_t len = 0;
  if (wd_r_head(r, &major, &len) != 0 || major != 3) {
    r->failed = 1;
    return -1;
  }
  if (r->at + len > r->len) {
    r->failed = 1;
    return -1;
  }
  *p = r->buf + r->at;
  *n = len;
  r->at += len;
  return 0;
}

static int wd_r_text(wd_r *r, char *out, uint32_t cap, uint32_t *len) {
  const uint8_t *p = 0;
  uint32_t n = 0;
  if (wd_r_text_ptr(r, &p, &n) != 0) return -1;
  if (n + 1 > cap) {
    r->failed = 1;
    return -1;
  }
  for (uint32_t i = 0; i < n; i++) out[i] = (char)p[i];
  out[n] = '\0';
  if (len != 0) *len = n;
  return 0;
}

static int wd_r_array(wd_r *r, uint32_t *n) {
  uint8_t major = 0;
  uint32_t v = 0;
  if (wd_r_head(r, &major, &v) != 0 || major != 4) {
    r->failed = 1;
    return -1;
  }
  *n = v;
  return 0;
}

static int wd_r_map(wd_r *r, uint32_t *n) {
  uint8_t major = 0;
  uint32_t v = 0;
  if (wd_r_head(r, &major, &v) != 0 || major != 5) {
    r->failed = 1;
    return -1;
  }
  *n = v;
  return 0;
}

static int wd_r_bool(wd_r *r, int *v) {
  uint8_t b = wd_r_byte(r);
  if (r->failed) return -1;
  if (b == 0xf5) {
    *v = 1;
    return 0;
  }
  if (b == 0xf4) {
    *v = 0;
    return 0;
  }
  r->failed = 1;
  return -1;
}

static int wd_r_skip(wd_r *r) {
  uint8_t major = 0;
  uint32_t v = 0;
  uint32_t start = r->at;
  if (r->at < r->len && (r->buf[r->at] == 0xf4 || r->buf[r->at] == 0xf5)) {
    r->at++;
    return 0;
  }
  if (wd_r_head(r, &major, &v) != 0) return -1;
  (void)start;
  if (major == 0 || major == 1) return 0;
  if (major == 2 || major == 3) {
    if (r->at + v > r->len) {
      r->failed = 1;
      return -1;
    }
    r->at += v;
    return 0;
  }
  if (major == 4) {
    for (uint32_t i = 0; i < v; i++)
      if (wd_r_skip(r) != 0) return -1;
    return 0;
  }
  if (major == 5) {
    for (uint32_t i = 0; i < v; i++)
      if (wd_r_skip(r) != 0 || wd_r_skip(r) != 0) return -1;
    return 0;
  }
  r->failed = 1;
  return -1;
}

static uint32_t wd_get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void wd_put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int wd_key_eq(const uint8_t *p, uint32_t n, const char *key) {
  uint32_t want = wd_len(key);
  if (n != want) return 0;
  for (uint32_t i = 0; i < n; i++)
    if (p[i] != (uint8_t)key[i]) return 0;
  return 1;
}

static int wd_r_key(wd_r *r, const char *key) {
  const uint8_t *p = 0;
  uint32_t n = 0;
  if (wd_r_text_ptr(r, &p, &n) != 0) return -1;
  uint32_t want = wd_len(key);
  if (n != want) return 0;
  for (uint32_t i = 0; i < n; i++)
    if (p[i] != (uint8_t)key[i]) return 0;
  return 1;
}

#endif
