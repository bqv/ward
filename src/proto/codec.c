/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "proto/codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const error_names[] = {
    [HX_OK] = "ok",
    [HX_ERR_INVALID_FRAME] = "invalid_frame",
    [HX_ERR_VERSION] = "version",
    [HX_ERR_UNAUTHENTICATED] = "unauthenticated",
    [HX_ERR_NOT_FOUND] = "not_found",
    [HX_ERR_INVALID_PARAMS] = "invalid_params",
    [HX_ERR_ALREADY_RESOLVED] = "already_resolved",
    [HX_ERR_BUSY] = "busy",
    [HX_ERR_INTERNAL] = "internal",
    [HX_ERR_NOT_SUPPORTED] = "not_supported",
    [HX_ERR_CONFLICT] = "conflict",
    [HX_ERR_CANCELLED] = "cancelled",
    [HX_ERR_LIMIT] = "limit",
};

const char *hx_error_name(hx_error err) {
  if (err < 0 || (size_t)err >= sizeof(error_names) / sizeof(error_names[0]) ||
      error_names[err] == NULL)
    return "internal";
  return error_names[err];
}

cbor_item_t *hx_map(uint64_t n) { return cbor_new_definite_map((size_t)n); }

cbor_item_t *hx_array(uint64_t n) {
  return cbor_new_definite_array((size_t)n);
}

cbor_item_t *hx_uint(uint64_t v) { return cbor_build_uint64(v); }

cbor_item_t *hx_text_len(const char *s, size_t len) {
  return cbor_build_stringn(s, len);
}

cbor_item_t *hx_text(const char *s) { return cbor_build_string(s); }

cbor_item_t *hx_bool(bool v) { return cbor_build_bool(v); }

void hx_put(cbor_item_t *map, const char *key, cbor_item_t *value) {
  cbor_item_t *k = cbor_build_string(key);
  struct cbor_pair pair = {.key = k, .value = value};
  bool added = cbor_map_add(map, pair);
  (void)added;
  cbor_decref(&k);
  cbor_decref(&value);
}

cbor_item_t *hx_get(cbor_item_t *map, const char *key) {
  if (map == NULL || !cbor_isa_map(map)) return NULL;
  struct cbor_pair *pairs = cbor_map_handle(map);
  size_t n = cbor_map_size(map);
  size_t key_len = strlen(key);
  for (size_t i = 0; i < n; i++) {
    cbor_item_t *k = pairs[i].key;
    if (k == NULL || !cbor_isa_string(k)) continue;
    if (cbor_string_length(k) != key_len) continue;
    if (memcmp(cbor_string_handle(k), key, key_len) != 0) continue;
    return pairs[i].value;
  }
  return NULL;
}

bool hx_uint_of(cbor_item_t *item, uint64_t *out) {
  if (item == NULL || !cbor_isa_uint(item)) return false;
  *out = cbor_get_uint64(item);
  return true;
}

bool hx_bool_of(cbor_item_t *item, bool *out) {
  if (item == NULL || !cbor_is_bool(item)) return false;
  *out = cbor_get_bool(item);
  return true;
}

char *hx_text_copy(cbor_item_t *item) {
  if (item == NULL || !cbor_isa_string(item)) return NULL;
  size_t n = cbor_string_length(item);
  char *s = malloc(n + 1);
  if (s == NULL) return NULL;
  memcpy(s, cbor_string_handle(item), n);
  s[n] = '\0';
  return s;
}

uint8_t *hx_encode(cbor_item_t *item, size_t *len) {
  if (item == NULL) return NULL;
  size_t need = cbor_serialized_size(item);
  if (need == 0) {
    hx_free(item);
    return NULL;
  }
  uint8_t *buf = malloc(need);
  if (buf == NULL) {
    hx_free(item);
    return NULL;
  }
  size_t written = cbor_serialize(item, buf, need);
  hx_free(item);
  if (written == 0) {
    free(buf);
    return NULL;
  }
  *len = written;
  return buf;
}

static uint64_t read_be(const uint8_t *p, unsigned n) {
  uint64_t v = 0;
  for (unsigned i = 0; i < n; i++) v = (v << 8) | p[i];
  return v;
}

static bool scan_item(const uint8_t **cursor, const uint8_t *end, int depth);

static bool scan_string_chunk(const uint8_t **cursor, const uint8_t *end,
                              uint8_t major) {
  const uint8_t *p = *cursor;
  if (p >= end) return false;
  uint8_t initial = *p++;
  uint8_t got_major = (uint8_t)(initial >> 5);
  uint8_t ai = (uint8_t)(initial & 0x1f);
  if (got_major != major || ai == 31) return false;
  uint64_t length = 0;
  if (ai < 24) {
    length = ai;
  } else if (ai <= 27) {
    unsigned n = 1u << (ai - 24);
    if ((size_t)(end - p) < n) return false;
    length = read_be(p, n);
    p += n;
  } else {
    return false;
  }
  if (length > (uint64_t)(end - p)) return false;
  *cursor = p + length;
  return true;
}

static bool scan_item(const uint8_t **cursor, const uint8_t *end, int depth) {
  if (depth > 64) return false;
  const uint8_t *p = *cursor;
  if (p >= end) return false;
  uint8_t initial = *p++;
  uint8_t major = (uint8_t)(initial >> 5);
  uint8_t ai = (uint8_t)(initial & 0x1f);
  uint64_t value = 0;
  bool indefinite = false;
  if (ai < 24) {
    value = ai;
  } else if (ai <= 27) {
    unsigned n = 1u << (ai - 24);
    if ((size_t)(end - p) < n) return false;
    value = read_be(p, n);
    p += n;
  } else if (ai == 31) {
    indefinite = true;
  } else {
    return false;
  }
  switch (major) {
    case 0:
    case 1:
      if (indefinite) return false;
      break;
    case 2:
    case 3:
      if (!indefinite) {
        if (value > (uint64_t)(end - p)) return false;
        p += value;
      } else {
        while (p < end && *p != 0xff) {
          if (!scan_string_chunk(&p, end, major)) return false;
        }
        if (p >= end) return false;
        p++;
      }
      break;
    case 4:
      if (!indefinite) {
        if (value > (uint64_t)(end - p)) return false;
        for (uint64_t i = 0; i < value; i++)
          if (!scan_item(&p, end, depth + 1)) return false;
      } else {
        while (p < end && *p != 0xff) {
          if (!scan_item(&p, end, depth + 1)) return false;
        }
        if (p >= end) return false;
        p++;
      }
      break;
    case 5:
      if (!indefinite) {
        if (value > (uint64_t)(end - p) / 2) return false;
        for (uint64_t i = 0; i < value * 2; i++)
          if (!scan_item(&p, end, depth + 1)) return false;
      } else {
        while (p < end && *p != 0xff) {
          if (!scan_item(&p, end, depth + 1)) return false;
          if (!scan_item(&p, end, depth + 1)) return false;
        }
        if (p >= end) return false;
        p++;
      }
      break;
    case 6:
      if (indefinite) return false;
      if (!scan_item(&p, end, depth + 1)) return false;
      break;
    case 7:
      if (indefinite) return false;
      break;
    default:
      return false;
  }
  *cursor = p;
  return true;
}

static bool scan_cbor(const uint8_t *buf, size_t len) {
  const uint8_t *p = buf;
  const uint8_t *end = buf + len;
  if (!scan_item(&p, end, 0)) return false;
  return p == end;
}

cbor_item_t *hx_decode(const uint8_t *buf, size_t len) {
  if (!scan_cbor(buf, len)) return NULL;
  struct cbor_load_result result;
  memset(&result, 0, sizeof(result));
  cbor_item_t *root = cbor_load(buf, len, &result);
  if (root == NULL || result.error.code != CBOR_ERR_NONE || result.read != len) {
    if (root != NULL) cbor_decref(&root);
    return NULL;
  }
  return root;
}

void hx_free(cbor_item_t *item) {
  if (item != NULL) cbor_decref(&item);
}

cbor_item_t *hx_hello(void) {
  cbor_item_t *root = hx_map(3);
  hx_put(root, "v", hx_uint(HX_PROTOCOL_VERSION));
  hx_put(root, "client", hx_text("harness-cli/" HX_VERSION_TEXT));
  cbor_item_t *caps = hx_array(1);
  cbor_item_t *cap = hx_text("events");
  bool pushed = cbor_array_push(caps, cap);
  (void)pushed;
  cbor_decref(&cap);
  hx_put(root, "caps", caps);
  return root;
}

cbor_item_t *hx_welcome(uid_t uid, uint64_t max_body) {
  cbor_item_t *root = hx_map(4);
  hx_put(root, "v", hx_uint(HX_PROTOCOL_VERSION));
  hx_put(root, "server", hx_text("harness/" HX_VERSION_TEXT));
  cbor_item_t *auth = hx_map(2);
  hx_put(auth, "uid", hx_uint((uint64_t)uid));
  hx_put(auth, "scope", hx_text("local"));
  hx_put(root, "auth", auth);
  cbor_item_t *limits = hx_map(2);
  hx_put(limits, "body", hx_uint(max_body));
  hx_put(limits, "inflight", hx_uint(64));
  hx_put(root, "limits", limits);
  return root;
}

cbor_item_t *hx_request(uint64_t id, const char *method, cbor_item_t *params) {
  cbor_item_t *root = hx_map(3);
  hx_put(root, "id", hx_uint(id));
  hx_put(root, "method", hx_text(method));
  hx_put(root, "params", params != NULL ? params : hx_map(0));
  return root;
}

cbor_item_t *hx_response_ok(uint64_t id, cbor_item_t *result) {
  cbor_item_t *root = hx_map(3);
  hx_put(root, "id", hx_uint(id));
  hx_put(root, "ok", hx_bool(true));
  hx_put(root, "result", result != NULL ? result : hx_map(0));
  return root;
}

cbor_item_t *hx_response_err(uint64_t id, hx_error err, const char *message) {
  cbor_item_t *root = hx_map(3);
  hx_put(root, "id", hx_uint(id));
  hx_put(root, "ok", hx_bool(false));
  cbor_item_t *detail = hx_map(3);
  hx_put(detail, "code", hx_uint((uint64_t)err));
  hx_put(detail, "name", hx_text(hx_error_name(err)));
  hx_put(detail, "message", hx_text(message != NULL ? message : ""));
  hx_put(root, "error", detail);
  return root;
}

cbor_item_t *hx_event(const char *kind, cbor_item_t *payload) {
  cbor_item_t *root = hx_map(2);
  hx_put(root, "kind", hx_text(kind));
  hx_put(root, "payload", payload != NULL ? payload : hx_map(0));
  return root;
}

int hx_read_hello(cbor_item_t *root, unsigned *version, char **client) {
  if (root == NULL || !cbor_isa_map(root)) return 0;
  uint64_t v = 0;
  if (!hx_uint_of(hx_get(root, "v"), &v)) return 0;
  *version = (unsigned)v;
  cbor_item_t *c = hx_get(root, "client");
  if (c != NULL && !cbor_isa_string(c)) return 0;
  if (client != NULL) *client = hx_text_copy(c);
  return 1;
}

int hx_read_welcome(cbor_item_t *root, unsigned *version, uint64_t *uid) {
  if (root == NULL || !cbor_isa_map(root)) return 0;
  uint64_t v = 0;
  if (!hx_uint_of(hx_get(root, "v"), &v)) return 0;
  *version = (unsigned)v;
  if (uid != NULL) {
    cbor_item_t *auth = hx_get(root, "auth");
    uint64_t u = 0;
    if (auth == NULL || !hx_uint_of(hx_get(auth, "uid"), &u)) return 0;
    *uid = u;
  }
  return 1;
}

int hx_read_request(cbor_item_t *root, uint64_t *id,
                    char method[HX_MAX_METHOD], cbor_item_t **params) {
  if (root == NULL || !cbor_isa_map(root)) return 0;
  uint64_t mid = 0;
  if (!hx_uint_of(hx_get(root, "id"), &mid) || mid == 0) return 0;
  char *m = hx_text_copy(hx_get(root, "method"));
  if (m == NULL) return 0;
  if (strlen(m) >= HX_MAX_METHOD) {
    free(m);
    return 0;
  }
  cbor_item_t *p = hx_get(root, "params");
  if (p == NULL || !cbor_isa_map(p)) {
    free(m);
    return 0;
  }
  memcpy(method, m, strlen(m) + 1);
  free(m);
  *id = mid;
  *params = p;
  return 1;
}

int hx_read_response(cbor_item_t *root, uint64_t *id, bool *ok,
                     cbor_item_t **value) {
  if (root == NULL || !cbor_isa_map(root)) return 0;
  uint64_t mid = 0;
  bool good = false;
  if (!hx_uint_of(hx_get(root, "id"), &mid) || mid == 0) return 0;
  if (!hx_bool_of(hx_get(root, "ok"), &good)) return 0;
  cbor_item_t *v = hx_get(root, good ? "result" : "error");
  if (v == NULL) return 0;
  *id = mid;
  *ok = good;
  *value = v;
  return 1;
}

int hx_read_event(cbor_item_t *root, char **kind, cbor_item_t **payload) {
  if (root == NULL || !cbor_isa_map(root)) return 0;
  char *k = hx_text_copy(hx_get(root, "kind"));
  if (k == NULL) return 0;
  cbor_item_t *p = hx_get(root, "payload");
  if (p == NULL || !cbor_isa_map(p)) {
    free(k);
    return 0;
  }
  *kind = k;
  *payload = p;
  return 1;
}

static size_t json_put(char *out, size_t cap, size_t pos, const char *text,
                       size_t n) {
  if (cap > 0 && pos < cap) {
    size_t room = cap - pos - 1;
    size_t take = n < room ? n : room;
    memcpy(out + pos, text, take);
    out[pos + take] = '\0';
  }
  return pos + n;
}

static size_t json_string(char *out, size_t cap, size_t pos, const uint8_t *s,
                          size_t n) {
  pos = json_put(out, cap, pos, "\"", 1);
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\') {
      char esc[2] = {'\\', (char)c};
      pos = json_put(out, cap, pos, esc, 2);
    } else if (c == '\n') {
      pos = json_put(out, cap, pos, "\\n", 2);
    } else if (c == '\t') {
      pos = json_put(out, cap, pos, "\\t", 2);
    } else if (c < 0x20) {
      char esc[7];
      int w = snprintf(esc, sizeof(esc), "\\u%04x", c);
      pos = json_put(out, cap, pos, esc, (size_t)w);
    } else {
      pos = json_put(out, cap, pos, (const char *)&c, 1);
    }
  }
  return json_put(out, cap, pos, "\"", 1);
}

static size_t json_value(cbor_item_t *item, char *out, size_t cap, size_t pos) {
  if (item == NULL) return json_put(out, cap, pos, "null", 4);
  switch (cbor_typeof(item)) {
    case CBOR_TYPE_UINT: {
      char buf[24];
      int n = snprintf(buf, sizeof(buf), "%llu",
                       (unsigned long long)cbor_get_uint64(item));
      return json_put(out, cap, pos, buf, (size_t)n);
    }
    case CBOR_TYPE_NEGINT: {
      char buf[24];
      int n = snprintf(buf, sizeof(buf), "-%llu",
                       (unsigned long long)cbor_get_int(item));
      return json_put(out, cap, pos, buf, (size_t)n);
    }
    case CBOR_TYPE_BYTESTRING: {
      static const char hex[] = "0123456789abcdef";
      pos = json_put(out, cap, pos, "\"", 1);
      size_t n = cbor_bytestring_length(item);
      const uint8_t *p = cbor_bytestring_handle(item);
      for (size_t i = 0; i < n; i++) {
        char pair[2] = {hex[p[i] >> 4], hex[p[i] & 0xf]};
        pos = json_put(out, cap, pos, pair, 2);
      }
      return json_put(out, cap, pos, "\"", 1);
    }
    case CBOR_TYPE_STRING:
      return json_string(out, cap, pos, cbor_string_handle(item),
                         cbor_string_length(item));
    case CBOR_TYPE_ARRAY: {
      size_t n = cbor_array_size(item);
      cbor_item_t **items = cbor_array_handle(item);
      pos = json_put(out, cap, pos, "[", 1);
      for (size_t i = 0; i < n; i++) {
        if (i > 0) pos = json_put(out, cap, pos, ",", 1);
        pos = json_value(items[i], out, cap, pos);
      }
      return json_put(out, cap, pos, "]", 1);
    }
    case CBOR_TYPE_MAP: {
      size_t n = cbor_map_size(item);
      struct cbor_pair *pairs = cbor_map_handle(item);
      pos = json_put(out, cap, pos, "{", 1);
      for (size_t i = 0; i < n; i++) {
        if (i > 0) pos = json_put(out, cap, pos, ",", 1);
        cbor_item_t *k = pairs[i].key;
        if (k != NULL && cbor_isa_string(k))
          pos = json_string(out, cap, pos, cbor_string_handle(k),
                            cbor_string_length(k));
        else
          pos = json_put(out, cap, pos, "\"?\"", 3);
        pos = json_put(out, cap, pos, ":", 1);
        pos = json_value(pairs[i].value, out, cap, pos);
      }
      return json_put(out, cap, pos, "}", 1);
    }
    case CBOR_TYPE_FLOAT_CTRL:
      if (cbor_is_bool(item))
        return json_put(out, cap, pos, cbor_get_bool(item) ? "true" : "false",
                        cbor_get_bool(item) ? 4 : 5);
      return json_put(out, cap, pos, "null", 4);
    case CBOR_TYPE_TAG:
      return json_put(out, cap, pos, "null", 4);
    default: {
      char buf[32];
      int n = snprintf(buf, sizeof(buf), "\"<%d>\"", (int)cbor_typeof(item));
      return json_put(out, cap, pos, buf, (size_t)n);
    }
  }
}

size_t hx_json(cbor_item_t *item, char *out, size_t cap) {
  if (cap == 0) return 0;
  out[0] = '\0';
  return json_value(item, out, cap, 0);
}
