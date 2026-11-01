/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_PROTO_CODEC_H
#define WARD_PROTO_CODEC_H

#include <cbor.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define WD_PROTOCOL_VERSION 1
#define WD_VERSION_TEXT "0.0.1"
#define WD_MAX_METHOD 64

typedef enum {
  WD_OK = 0,
  WD_ERR_INVALID_FRAME = 1,
  WD_ERR_VERSION = 2,
  WD_ERR_UNAUTHENTICATED = 3,
  WD_ERR_NOT_FOUND = 4,
  WD_ERR_INVALID_PARAMS = 5,
  WD_ERR_ALREADY_RESOLVED = 6,
  WD_ERR_BUSY = 7,
  WD_ERR_INTERNAL = 8,
  WD_ERR_NOT_SUPPORTED = 9,
  WD_ERR_CONFLICT = 10,
  WD_ERR_CANCELLED = 11,
  WD_ERR_LIMIT = 12,
} wd_error;

const char *wd_error_name(wd_error err);

cbor_item_t *wd_map(uint64_t n);
cbor_item_t *wd_array(uint64_t n);
cbor_item_t *wd_uint(uint64_t v);
cbor_item_t *wd_text(const char *s);
cbor_item_t *wd_text_len(const char *s, size_t len);
cbor_item_t *wd_bool(bool v);
void wd_put(cbor_item_t *map, const char *key, cbor_item_t *value);
cbor_item_t *wd_get(cbor_item_t *map, const char *key);
bool wd_uint_of(cbor_item_t *item, uint64_t *out);
bool wd_bool_of(cbor_item_t *item, bool *out);
char *wd_text_copy(cbor_item_t *item);

uint8_t *wd_encode(cbor_item_t *item, size_t *len);
cbor_item_t *wd_decode(const uint8_t *buf, size_t len);
void wd_free(cbor_item_t *item);

cbor_item_t *wd_hello(void);
cbor_item_t *wd_welcome(uid_t uid, uint64_t max_body);
cbor_item_t *wd_request(uint64_t id, const char *method, cbor_item_t *params);
cbor_item_t *wd_response_ok(uint64_t id, cbor_item_t *result);
cbor_item_t *wd_response_err(uint64_t id, wd_error err, const char *message);
cbor_item_t *wd_event(const char *kind, cbor_item_t *payload);

int wd_read_hello(cbor_item_t *root, unsigned *version, char **client);
int wd_read_welcome(cbor_item_t *root, unsigned *version, uint64_t *uid);
int wd_read_request(cbor_item_t *root, uint64_t *id, char method[WD_MAX_METHOD],
                    cbor_item_t **params);
int wd_read_response(cbor_item_t *root, uint64_t *id, bool *ok,
                     cbor_item_t **value);
int wd_read_event(cbor_item_t *root, char **kind, cbor_item_t **payload);

size_t wd_json(cbor_item_t *item, char *out, size_t cap);

#endif
