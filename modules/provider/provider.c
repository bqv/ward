/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdio.h>

#include "wd_abi.h"

#define HEAP_SIZE (2u * 1024u * 1024u)
#define TEXT_SIZE 2048u
#define JSON_SIZE (2u * 1024u * 1024u)
#define REPLY_SIZE (JSON_SIZE + 8192u)
#define CALL_SIZE 8192u
#define TOOL_SIZE 128u
#define ARG_SIZE 8192u
#define IMAGE_MAX 2u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static uint8_t callbuf[CALL_SIZE];
static char answer[TEXT_SIZE];
static char parsed_tool[TOOL_SIZE];
static char parsed_arguments[ARG_SIZE];

static const char tools_json[] =
    ",\"tools\":["
    "{\"type\":\"function\",\"function\":{\"name\":\"read\","
    "\"description\":\"Read a text file from the session directory.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
    "\"string\",\"description\":\"Path relative to the session directory.\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write\","
    "\"description\":\"Write a whole text file in the session directory.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
    "\"string\",\"description\":\"Path relative to the session directory.\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"The complete new file "
    "contents.\"}},\"required\":[\"path\",\"text\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit\","
    "\"description\":\"Replace the first occurrence of find with replace in a "
    "file in the session directory.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
    "\"string\",\"description\":\"Path relative to the session directory.\"},"
    "\"find\":{\"type\":\"string\",\"description\":\"Exact text to replace.\"},"
    "\"replace\":{\"type\":\"string\",\"description\":\"Replacement text.\"}},"
    "\"required\":[\"path\",\"find\",\"replace\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"grep\","
    "\"description\":\"Search files in the session directory with a regular "
    "expression.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":"
    "\"string\",\"description\":\"Regular expression to search for.\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Optional file or "
    "directory relative to the session directory.\"}},"
    "\"required\":[\"pattern\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"exec\","
    "\"description\":\"Run one command in the session directory with no "
    "shell.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":"
    "\"string\",\"description\":\"Command and arguments separated by spaces.\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"python\","
    "\"description\":\"Run a Python program in the session directory.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"program\":{\"type\":"
    "\"string\",\"description\":\"The Python source to run.\"}},"
    "\"required\":[\"program\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"plan\","
    "\"description\":\"Record or update the session plan.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"plan\":{\"type\":"
    "\"string\",\"description\":\"The plan text.\"}},"
    "\"required\":[\"plan\"]}}}"
    "]";

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
  if (n > REPLY_SIZE) n = REPLY_SIZE;
  for (uint32_t i = 0; i < n; i++) reply[i] = data[i];
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, at);
  wd_put32(o + 4, n);
}

typedef struct {
  char *buf;
  uint32_t cap;
  uint32_t len;
  int overflow;
} json_builder;

static void json_add(json_builder *b, const char *p, uint32_t n) {
  if (b->overflow) return;
  if (n > b->cap - b->len) {
    b->overflow = 1;
    return;
  }
  for (uint32_t i = 0; i < n; i++) b->buf[b->len + i] = p[i];
  b->len += n;
}

static void json_addz(json_builder *b, const char *p) {
  json_add(b, p, wd_len(p));
}

static void json_add_escaped(json_builder *b, const uint8_t *in, uint32_t len) {
  static const char hex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < len; i++) {
    uint8_t c = in[i];
    char tmp[8];
    uint32_t n = 0;
    if (c == '"' || c == '\\') {
      tmp[n++] = '\\';
      tmp[n++] = (char)c;
    } else if (c == '\n') {
      tmp[n++] = '\\';
      tmp[n++] = 'n';
    } else if (c == '\r') {
      tmp[n++] = '\\';
      tmp[n++] = 'r';
    } else if (c == '\t') {
      tmp[n++] = '\\';
      tmp[n++] = 't';
    } else if (c < 0x20) {
      tmp[n++] = '\\';
      tmp[n++] = 'u';
      tmp[n++] = '0';
      tmp[n++] = '0';
      tmp[n++] = hex[(c >> 4) & 0xf];
      tmp[n++] = hex[c & 0xf];
    } else {
      tmp[n++] = (char)c;
    }
    json_add(b, tmp, n);
  }
}

static void json_add_base64(json_builder *b, const uint8_t *in, uint32_t len) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char chunk[4];
  uint32_t i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) |
                 (uint32_t)in[i + 2];
    chunk[0] = alphabet[(v >> 18) & 0x3f];
    chunk[1] = alphabet[(v >> 12) & 0x3f];
    chunk[2] = alphabet[(v >> 6) & 0x3f];
    chunk[3] = alphabet[v & 0x3f];
    json_add(b, chunk, 4);
  }
  if (i < len) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
    chunk[0] = alphabet[(v >> 18) & 0x3f];
    chunk[1] = alphabet[(v >> 12) & 0x3f];
    chunk[2] = i + 1 < len ? alphabet[(v >> 6) & 0x3f] : '=';
    chunk[3] = '=';
    json_add(b, chunk, 4);
  }
}

static int wd_r_bytes_ptr(wd_r *r, const uint8_t **p, uint32_t *n) {
  uint8_t major = 0;
  uint32_t len = 0;
  if (wd_r_head(r, &major, &len) != 0 || major != 2) {
    r->failed = 1;
    return -1;
  }
  if (r->at > r->len || len > r->len - r->at) {
    r->failed = 1;
    return -1;
  }
  *p = r->buf + r->at;
  *n = len;
  r->at += len;
  return 0;
}

static void append_text(uint32_t *at, const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n && *at + 1 < TEXT_SIZE; i++) answer[(*at)++] = (char)p[i];
  answer[*at] = '\0';
}

static uint32_t set_answer(const char *text) {
  uint32_t n = 0;
  while (text[n] != '\0' && n + 1 < sizeof(answer)) {
    answer[n] = text[n];
    n++;
  }
  answer[n] = '\0';
  return n;
}

static int hex_value(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int num_field(const char *body, uint32_t len, const char *key,
                     uint32_t *out) {
  uint32_t key_len = (uint32_t)wd_len(key);
  for (uint32_t i = 0; i + key_len < len; i++) {
    uint32_t k = 0;
    while (k < key_len && body[i + k] == key[k]) k++;
    if (k != key_len) continue;
    uint32_t j = i + key_len;
    while (j < len && (body[j] == ':' || body[j] == ' ')) j++;
    uint32_t value = 0;
    uint32_t digits = 0;
    while (j < len && body[j] >= '0' && body[j] <= '9') {
      value = value * 10 + (uint32_t)(body[j] - '0');
      j++;
      digits++;
    }
    if (digits == 0) continue;
    *out = value;
    return 1;
  }
  return 0;
}

static void strip_slashes(const char *in, char *out, uint32_t cap) {
  uint32_t used = 0;
  for (uint32_t i = 0; in[i] != '\0' && used + 1 < cap; i++) {
    if (in[i] == '\\' && in[i + 1] != '\0') continue;
    out[used++] = in[i];
  }
  out[used] = '\0';
}

static void quoted_after(const char *text, const char *key, char *out,
                         uint32_t cap) {
  out[0] = '\0';
  uint32_t key_len = (uint32_t)wd_len(key);
  for (uint32_t i = 0; text[i] != '\0'; i++) {
    uint32_t k = 0;
    while (k < key_len && text[i + k] == key[k]) k++;
    if (k != key_len) continue;
    uint32_t j = i + key_len;
    while (text[j] == ' ' || text[j] == ':' || text[j] == '"') j++;
    if (text[j] == '\0') return;
    uint32_t used = 0;
    while (text[j] != '\0' && text[j] != '"' && used + 1 < cap)
      out[used++] = text[j++];
    out[used] = '\0';
    return;
  }
}

static int has_token(const char *body, uint32_t len, const char *token) {
  uint32_t token_len = (uint32_t)wd_len(token);
  for (uint32_t i = 0; i + token_len <= len; i++) {
    uint32_t k = 0;
    while (k < token_len && body[i + k] == token[k]) k++;
    if (k == token_len) return 1;
  }
  return 0;
}

static void skip_space(const char *b, uint32_t len, uint32_t *at) {
  while (*at < len) {
    char c = b[*at];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      (*at)++;
    else
      break;
  }
}

static int skip_string(const char *b, uint32_t len, uint32_t *at) {
  if (*at >= len || b[*at] != '"') return -1;
  (*at)++;
  while (*at < len) {
    char c = b[*at];
    if (c == '\\') {
      if (*at + 1 >= len) return -1;
      *at += 2;
      continue;
    }
    (*at)++;
    if (c == '"') return 0;
  }
  return -1;
}

static int skip_value(const char *b, uint32_t len, uint32_t *at, int depth) {
  if (depth > 24) return -1;
  skip_space(b, len, at);
  if (*at >= len) return -1;
  char c = b[*at];
  if (c == '"') return skip_string(b, len, at);
  if (c == '{') {
    (*at)++;
    skip_space(b, len, at);
    if (*at < len && b[*at] == '}') {
      (*at)++;
      return 0;
    }
    for (;;) {
      skip_space(b, len, at);
      if (skip_string(b, len, at) != 0) return -1;
      skip_space(b, len, at);
      if (*at >= len || b[*at] != ':') return -1;
      (*at)++;
      if (skip_value(b, len, at, depth + 1) != 0) return -1;
      skip_space(b, len, at);
      if (*at >= len) return -1;
      if (b[*at] == ',') {
        (*at)++;
        continue;
      }
      if (b[*at] == '}') {
        (*at)++;
        return 0;
      }
      return -1;
    }
  }
  if (c == '[') {
    (*at)++;
    skip_space(b, len, at);
    if (*at < len && b[*at] == ']') {
      (*at)++;
      return 0;
    }
    for (;;) {
      if (skip_value(b, len, at, depth + 1) != 0) return -1;
      skip_space(b, len, at);
      if (*at >= len) return -1;
      if (b[*at] == ',') {
        (*at)++;
        continue;
      }
      if (b[*at] == ']') {
        (*at)++;
        return 0;
      }
      return -1;
    }
  }
  uint32_t start = *at;
  while (*at < len) {
    char v = b[*at];
    if (v == ',' || v == ']' || v == '}' || v == ' ' || v == '\t' ||
        v == '\r' || v == '\n')
      break;
    (*at)++;
  }
  return *at == start ? -1 : 0;
}

static void append_bytes(char *out, uint32_t limit, uint32_t *used,
                         const char *p, uint32_t n, int *truncated) {
  if (*used + n > limit) {
    *truncated = 1;
    return;
  }
  for (uint32_t i = 0; i < n; i++) out[(*used)++] = p[i];
}

static void append_codepoint(char *out, uint32_t limit, uint32_t *used,
                             uint32_t cp, int *truncated) {
  char tmp[4];
  uint32_t n = 0;
  if (cp < 0x80) {
    tmp[n++] = (char)cp;
  } else if (cp < 0x800) {
    tmp[n++] = (char)(0xc0 | (cp >> 6));
    tmp[n++] = (char)(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    tmp[n++] = (char)(0xe0 | (cp >> 12));
    tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    tmp[n++] = (char)(0x80 | (cp & 0x3f));
  } else {
    tmp[n++] = (char)(0xf0 | (cp >> 18));
    tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
    tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    tmp[n++] = (char)(0x80 | (cp & 0x3f));
  }
  append_bytes(out, limit, used, tmp, n, truncated);
}

static int read_json_string(const char *b, uint32_t len, uint32_t *at,
                            char *out, uint32_t cap, uint32_t *used_io,
                            int *truncated) {
  if (*at >= len || b[*at] != '"') return -1;
  (*at)++;
  uint32_t used = *used_io;
  uint32_t limit = cap > 0 ? cap - 1 : 0;
  while (*at < len) {
    uint8_t c = (uint8_t)b[*at];
    if (c == '"') {
      (*at)++;
      if (out != 0 && cap > 0) out[used] = '\0';
      *used_io = used;
      return 0;
    }
    if (c == '\\') {
      (*at)++;
      if (*at >= len) return -1;
      uint8_t e = (uint8_t)b[*at];
      (*at)++;
      if (e == 'u') {
        uint32_t cp = 0;
        if (*at + 4 > len) return -1;
        for (int h = 0; h < 4; h++) {
          int hv = hex_value((uint8_t)b[*at + (uint32_t)h]);
          if (hv < 0) return -1;
          cp = (cp << 4) | (uint32_t)hv;
        }
        *at += 4;
        if (cp >= 0xd800 && cp <= 0xdbff && *at + 6 <= len &&
            b[*at] == '\\' && b[*at + 1] == 'u') {
          uint32_t low = 0;
          int ok = 1;
          for (int h = 0; h < 4; h++) {
            int hv = hex_value((uint8_t)b[*at + 2 + (uint32_t)h]);
            if (hv < 0) {
              ok = 0;
              break;
            }
            low = (low << 4) | (uint32_t)hv;
          }
          if (ok && low >= 0xdc00 && low <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            *at += 6;
          }
        }
        if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
        append_codepoint(out, limit, &used, cp, truncated);
      } else {
        char d;
        if (e == 'n')
          d = '\n';
        else if (e == 'r')
          d = '\r';
        else if (e == 't')
          d = '\t';
        else if (e == 'b')
          d = '\b';
        else if (e == 'f')
          d = '\f';
        else
          d = (char)e;
        append_bytes(out, limit, &used, &d, 1, truncated);
      }
      continue;
    }
    append_bytes(out, limit, &used, (const char *)&c, 1, truncated);
    (*at)++;
  }
  return -1;
}

typedef struct {
  int have_index;
  uint32_t index;
  char *name;
  uint32_t name_cap;
  uint32_t name_len;
  int name_trunc;
  char *args;
  uint32_t args_cap;
  uint32_t args_len;
  int args_trunc;
} call_fields;

static void zero_fields(call_fields *f) {
  f->have_index = 0;
  f->index = 0;
  f->name = 0;
  f->name_cap = 0;
  f->name_len = 0;
  f->name_trunc = 0;
  f->args = 0;
  f->args_cap = 0;
  f->args_len = 0;
  f->args_trunc = 0;
}

static int scan_call_object(const char *b, uint32_t len, uint32_t *at,
                            call_fields *f, int depth) {
  if (depth > 24) return -1;
  skip_space(b, len, at);
  if (*at >= len || b[*at] != '{') return -1;
  (*at)++;
  for (;;) {
    skip_space(b, len, at);
    if (*at >= len) return -1;
    if (b[*at] == '}') {
      (*at)++;
      return 0;
    }
    char key[32];
    uint32_t key_len = 0;
    int key_trunc = 0;
    if (read_json_string(b, len, at, key, sizeof(key), &key_len, &key_trunc) != 0)
      return -1;
    skip_space(b, len, at);
    if (*at >= len || b[*at] != ':') return -1;
    (*at)++;
    skip_space(b, len, at);
    if (*at >= len) return -1;
    int is_index = !key_trunc &&
                   wd_key_eq((const uint8_t *)key, key_len, "index");
    int is_name = !key_trunc && f->name != 0 &&
                  wd_key_eq((const uint8_t *)key, key_len, "name");
    int is_args = !key_trunc && f->args != 0 &&
                  wd_key_eq((const uint8_t *)key, key_len, "arguments");
    if (is_index && b[*at] >= '0' && b[*at] <= '9') {
      uint32_t value = 0;
      while (*at < len && b[*at] >= '0' && b[*at] <= '9') {
        value = value * 10 + (uint32_t)(b[*at] - '0');
        (*at)++;
      }
      f->have_index = 1;
      f->index = value;
    } else if (is_name && b[*at] == '"') {
      if (f->name_len != 0) {
        if (skip_string(b, len, at) != 0) return -1;
      } else if (read_json_string(b, len, at, f->name, f->name_cap, &f->name_len,
                                  &f->name_trunc) != 0) {
        return -1;
      }
    } else if (is_args && b[*at] == '"') {
      if (read_json_string(b, len, at, f->args, f->args_cap, &f->args_len,
                           &f->args_trunc) != 0)
        return -1;
    } else if (b[*at] == '{') {
      if (scan_call_object(b, len, at, f, depth + 1) != 0) return -1;
    } else {
      if (skip_value(b, len, at, depth + 1) != 0) return -1;
    }
    skip_space(b, len, at);
    if (*at < len && b[*at] == ',') {
      (*at)++;
      continue;
    }
    if (*at < len && b[*at] == '}') {
      (*at)++;
      return 0;
    }
    return -1;
  }
}

static int collect_tool_calls(const char *body, uint32_t len, char *tool,
                              uint32_t tool_cap, char *args, uint32_t args_cap,
                              int *truncated) {
  call_fields f;
  zero_fields(&f);
  f.name = tool;
  f.name_cap = tool_cap;
  f.args = args;
  f.args_cap = args_cap;
  tool[0] = '\0';
  args[0] = '\0';
  int streaming = has_token(body, len, "\"delta\":");
  int multiple = 0;
  int have_first = 0;
  uint32_t first_index = 0;
  uint32_t ordinal = 0;
  uint32_t at = 0;
  while (at < len) {
    if (body[at] != '"') {
      at++;
      continue;
    }
    char key[32];
    uint32_t key_len = 0;
    int key_trunc = 0;
    if (read_json_string(body, len, &at, key, sizeof(key), &key_len,
                         &key_trunc) != 0)
      return -1;
    skip_space(body, len, &at);
    if (at >= len || body[at] != ':') continue;
    if (key_trunc ||
        !wd_key_eq((const uint8_t *)key, key_len, "tool_calls"))
      continue;
    at++;
    skip_space(body, len, &at);
    if (at >= len || body[at] != '[') continue;
    at++;
    for (;;) {
      skip_space(body, len, &at);
      if (at >= len) break;
      if (body[at] == ']') {
        at++;
        break;
      }
      if (body[at] != '{') {
        if (skip_value(body, len, &at, 0) != 0) break;
        continue;
      }
      uint32_t start = at;
      call_fields probe;
      zero_fields(&probe);
      uint32_t scan = start;
      if (scan_call_object(body, len, &scan, &probe, 0) != 0) break;
      uint32_t index =
          probe.have_index ? probe.index : (streaming ? 0u : ordinal);
      if (!have_first) {
        have_first = 1;
        first_index = index;
      } else if (index != first_index) {
        multiple = 1;
      }
      if (!multiple && index == first_index) {
        scan = start;
        if (scan_call_object(body, len, &scan, &f, 0) != 0) break;
      }
      at = scan;
      ordinal++;
      skip_space(body, len, &at);
      if (at < len && body[at] == ',') {
        at++;
        continue;
      }
      if (at < len && body[at] == ']') {
        at++;
        break;
      }
      break;
    }
  }
  if (truncated != 0) *truncated = f.name_trunc || f.args_trunc;
  if (!have_first) return 0;
  if (multiple) return 2;
  return 1;
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

static void emit_buffer_error(uint32_t out) {
  wd_w full;
  wd_w_init(&full, reply, REPLY_SIZE);
  wd_w_map(&full, 2);
  wd_w_textz(&full, "text");
  wd_w_textz(&full,
             "the chat request did not fit the provider's buffers");
  wd_w_textz(&full, "status");
  wd_w_uint(&full, 0);
  if (!full.failed) pair(out, reply, full.at);
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
  const uint8_t *image_data[IMAGE_MAX];
  uint32_t image_data_len[IMAGE_MAX];
  const uint8_t *image_type[IMAGE_MAX];
  uint32_t image_type_len[IMAGE_MAX];
  uint32_t image_count = 0;
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
      for (uint32_t e = 0; e < hn; e++) {
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
          if (hist_count == 32) {
            for (uint32_t k = 2; k < 32; k++) {
              hist_text[k - 1] = hist_text[k];
              hist_text_len[k - 1] = hist_text_len[k];
              hist_role[k - 1] = hist_role[k];
              hist_role_len[k - 1] = hist_role_len[k];
            }
            hist_count = 31;
          }
          hist_text[hist_count] = tp;
          hist_text_len[hist_count] = tn;
          hist_role[hist_count] = rp;
          hist_role_len[hist_count] = rn;
          hist_count++;
        }
      }
    } else if (wd_key_eq(kp, kn, "images")) {
      uint32_t in = 0;
      if (wd_r_array(&r, &in) != 0) return;
      for (uint32_t e = 0; e < in; e++) {
        uint32_t mn = 0;
        if (wd_r_map(&r, &mn) != 0) return;
        const uint8_t *dp = 0;
        uint32_t dn = 0;
        const uint8_t *mp = 0;
        uint32_t mlen = 0;
        for (uint32_t k = 0; k < mn; k++) {
          const uint8_t *hk = 0;
          uint32_t hl = 0;
          if (wd_r_text_ptr(&r, &hk, &hl) != 0) return;
          if (wd_key_eq(hk, hl, "media_type")) {
            if (wd_r_text_ptr(&r, &mp, &mlen) != 0) return;
          } else if (wd_key_eq(hk, hl, "data")) {
            if (wd_r_bytes_ptr(&r, &dp, &dn) != 0) return;
          } else if (wd_r_skip(&r) != 0) {
            return;
          }
        }
        if (dp == 0 || mp == 0 || mlen == 0 || image_count == IMAGE_MAX)
          continue;
        image_data[image_count] = dp;
        image_data_len[image_count] = dn;
        image_type[image_count] = mp;
        image_type_len[image_count] = mlen;
        image_count++;
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
    uint32_t prompt_n = 0;
    uint32_t prompt_ms = 0;
    uint32_t predicted_n = 0;
    uint32_t predicted_ms = 0;
    uint32_t cache_n = 0;
    num_field((const char *)body, body_len, "\"prompt_n\"", &prompt_n);
    num_field((const char *)body, body_len, "\"prompt_ms\"", &prompt_ms);
    num_field((const char *)body, body_len, "\"predicted_n\"", &predicted_n);
    num_field((const char *)body, body_len, "\"predicted_ms\"", &predicted_ms);
    num_field((const char *)body, body_len, "\"cache_n\"", &cache_n);
    parsed_tool[0] = '\0';
    parsed_arguments[0] = '\0';
    if (status == 200 &&
        has_token((const char *)body, body_len, "tool_calls")) {
      int truncated = 0;
      int calls = collect_tool_calls(
          (const char *)body, body_len, parsed_tool, sizeof(parsed_tool),
          parsed_arguments, sizeof(parsed_arguments), &truncated);
      if (calls == 2) {
        parsed_tool[0] = '\0';
        parsed_arguments[0] = '\0';
        static const char multiple[] =
            "the model asked for more than one tool call; refusing rather than "
            "running only the first";
        text_at = set_answer(multiple);
      } else if (calls == 1 && truncated) {
        parsed_tool[0] = '\0';
        parsed_arguments[0] = '\0';
        static const char too_long[] =
            "the model's tool call was too long for this build; refusing "
            "rather than running a truncated call";
        text_at = set_answer(too_long);
      } else if (calls < 0) {
        parsed_tool[0] = '\0';
        parsed_arguments[0] = '\0';
      }
    }
    if (parsed_tool[0] == '\0') {
      static char plain[TEXT_SIZE];
      strip_slashes(answer, plain, sizeof(plain));
      if (has_token(plain, (uint32_t)wd_len(plain), "\"tool\"")) {
        quoted_after(plain, "\"tool\"", parsed_tool, sizeof(parsed_tool));
        char path[512];
        quoted_after(plain, "\"path\"", path, sizeof(path));
        if (parsed_tool[0] != '\0' && path[0] != '\0') {
          uint32_t used = 0;
          static const char open_key[] = "{\"path\":\"";
          for (uint32_t k = 0;
               open_key[k] != '\0' && used + 1 < sizeof(parsed_arguments);
               k++)
            parsed_arguments[used++] = open_key[k];
          for (uint32_t k = 0;
               path[k] != '\0' && used + 1 < sizeof(parsed_arguments); k++)
            parsed_arguments[used++] = path[k];
          if (used + 3 < sizeof(parsed_arguments)) {
            parsed_arguments[used++] = '"';
            parsed_arguments[used++] = '}';
          }
          parsed_arguments[used] = '\0';
        } else if (parsed_tool[0] != '\0') {
          static const char incomplete[] =
              "the model asked for a tool without a path; refusing rather than "
              "guessing";
          text_at = set_answer(incomplete);
        }
      }
    }
    wd_w_init(&w, reply, REPLY_SIZE);
    wd_w_map(&w, 9);
    wd_w_textz(&w, "text");
    wd_w_text(&w, answer, text_at);
    wd_w_textz(&w, "status");
    wd_w_uint(&w, status);
    wd_w_textz(&w, "prompt_n");
    wd_w_uint(&w, prompt_n);
    wd_w_textz(&w, "prompt_ms");
    wd_w_uint(&w, prompt_ms);
    wd_w_textz(&w, "predicted_n");
    wd_w_uint(&w, predicted_n);
    wd_w_textz(&w, "predicted_ms");
    wd_w_uint(&w, predicted_ms);
    wd_w_textz(&w, "cache_n");
    wd_w_uint(&w, cache_n);
    wd_w_textz(&w, "tool");
    wd_w_textz(&w, parsed_tool);
    wd_w_textz(&w, "arguments");
    wd_w_textz(&w, parsed_arguments);
    if (!w.failed) pair(out, reply, w.at);
    return;
  }
  if (!have_prompt) {
    wd_w empty;
    wd_w_init(&empty, reply, REPLY_SIZE);
    wd_w_map(&empty, 2);
    wd_w_textz(&empty, "text");
    wd_w_textz(&empty, "");
    wd_w_textz(&empty, "status");
    wd_w_uint(&empty, 0);
    if (!empty.failed) pair(out, reply, empty.at);
    return;
  }

  static const char prefix[] = "{\"model\":\"";
  static char json[JSON_SIZE];
  json_builder jb;
  jb.buf = json;
  jb.cap = sizeof(json);
  jb.len = 0;
  jb.overflow = 0;
  json_addz(&jb, prefix);
  {
    const char *chosen = model[0] != '\0' ? model : "local";
    json_addz(&jb, chosen);
  }
  json_addz(&jb, "\",\"stream\":true,\"max_tokens\":");
  json_addz(&jb, max_tokens);
  json_addz(&jb, tools_json);
  json_addz(&jb, ",\"messages\":[");
  for (uint32_t e = 0; e < hist_count; e++) {
    static const char role_open[] = "{\"role\":\"";
    static const char content_open[] = "\",\"content\":\"";
    json_addz(&jb, role_open);
    json_add(&jb, (const char *)hist_role[e], hist_role_len[e]);
    json_addz(&jb, content_open);
    json_add_escaped(&jb, hist_text[e], hist_text_len[e]);
    json_addz(&jb, "\"},");
  }
  {
    static const char user_open[] = "{\"role\":\"user\",\"content\":\"";
    static const char suffix[] = "\"}]}";
    if (image_count == 0) {
      json_addz(&jb, user_open);
      json_add_escaped(&jb, prompt, prompt_len);
      json_addz(&jb, suffix);
    } else {
      static const char text_open[] =
          "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
      static const char text_close[] = "\"}";
      static const char image_open[] =
          ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:";
      json_addz(&jb, text_open);
      json_add_escaped(&jb, prompt, prompt_len);
      json_addz(&jb, text_close);
      for (uint32_t i = 0; i < image_count; i++) {
        json_addz(&jb, image_open);
        json_add(&jb, (const char *)image_type[i], image_type_len[i]);
        json_addz(&jb, ";base64,");
        json_add_base64(&jb, image_data[i], image_data_len[i]);
        json_addz(&jb, "\"}}");
      }
      json_addz(&jb, "]}]}");
    }
  }
  if (jb.overflow || jb.len + 128 > REPLY_SIZE) {
    emit_buffer_error(out);
    return;
  }

  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "request");
  wd_w_map(&w, 2);
  wd_w_textz(&w, "path");
  wd_w_textz(&w, "/v1/chat/completions");
  wd_w_textz(&w, "body");
  wd_w_text(&w, json, jb.len);
  if (w.failed) {
    emit_buffer_error(out);
    return;
  }
  pair(out, reply, w.at);
}
