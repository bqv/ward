/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HARNESS_PROTO_CODEC_H
#define HARNESS_PROTO_CODEC_H

#include <cbor.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define HX_PROTOCOL_VERSION 1
#define HX_VERSION_TEXT "0.0.1"
#define HX_MAX_METHOD 64

typedef enum {
  HX_OK = 0,
  HX_ERR_INVALID_FRAME = 1,
  HX_ERR_VERSION = 2,
  HX_ERR_UNAUTHENTICATED = 3,
  HX_ERR_NOT_FOUND = 4,
  HX_ERR_INVALID_PARAMS = 5,
  HX_ERR_ALREADY_RESOLVED = 6,
  HX_ERR_BUSY = 7,
  HX_ERR_INTERNAL = 8,
  HX_ERR_NOT_SUPPORTED = 9,
  HX_ERR_CONFLICT = 10,
  HX_ERR_CANCELLED = 11,
  HX_ERR_LIMIT = 12,
} hx_error;

const char *hx_error_name(hx_error err);

cbor_item_t *hx_map(uint64_t n);
cbor_item_t *hx_array(uint64_t n);
cbor_item_t *hx_uint(uint64_t v);
cbor_item_t *hx_text(const char *s);
cbor_item_t *hx_text_len(const char *s, size_t len);
cbor_item_t *hx_bool(bool v);
void hx_put(cbor_item_t *map, const char *key, cbor_item_t *value);
cbor_item_t *hx_get(cbor_item_t *map, const char *key);
bool hx_uint_of(cbor_item_t *item, uint64_t *out);
bool hx_bool_of(cbor_item_t *item, bool *out);
char *hx_text_copy(cbor_item_t *item);

uint8_t *hx_encode(cbor_item_t *item, size_t *len);
cbor_item_t *hx_decode(const uint8_t *buf, size_t len);
void hx_free(cbor_item_t *item);

cbor_item_t *hx_hello(void);
cbor_item_t *hx_welcome(uid_t uid, uint64_t max_body);
cbor_item_t *hx_request(uint64_t id, const char *method, cbor_item_t *params);
cbor_item_t *hx_response_ok(uint64_t id, cbor_item_t *result);
cbor_item_t *hx_response_err(uint64_t id, hx_error err, const char *message);
cbor_item_t *hx_event(const char *kind, cbor_item_t *payload);

int hx_read_hello(cbor_item_t *root, unsigned *version, char **client);
int hx_read_welcome(cbor_item_t *root, unsigned *version, uint64_t *uid);
int hx_read_request(cbor_item_t *root, uint64_t *id, char method[HX_MAX_METHOD],
                    cbor_item_t **params);
int hx_read_response(cbor_item_t *root, uint64_t *id, bool *ok,
                     cbor_item_t **value);
int hx_read_event(cbor_item_t *root, char **kind, cbor_item_t **payload);

size_t hx_json(cbor_item_t *item, char *out, size_t cap);

#endif
