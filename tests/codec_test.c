/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/codec.h"

static void test_hello(void) {
  cbor_item_t *hello = wd_hello();
  size_t len = 0;
  uint8_t *bytes = wd_encode(hello, &len);
  assert(bytes != NULL && len > 0);
  cbor_item_t *root = wd_decode(bytes, len);
  free(bytes);
  assert(root != NULL);
  unsigned version = 0;
  char *client = NULL;
  assert(wd_read_hello(root, &version, &client));
  assert(version == WD_PROTOCOL_VERSION);
  assert(client != NULL && strstr(client, "ward-cli/") == client);
  free(client);
  wd_free(root);
}

static void test_request_round_trip(void) {
  cbor_item_t *params = wd_map(1);
  wd_put(params, "limit", wd_uint(7));
  cbor_item_t *request = wd_request(9, "session.list", params);
  size_t len = 0;
  uint8_t *bytes = wd_encode(request, &len);
  assert(bytes != NULL);
  cbor_item_t *root = wd_decode(bytes, len);
  free(bytes);
  assert(root != NULL);
  uint64_t id = 0;
  char method[WD_MAX_METHOD];
  cbor_item_t *got = NULL;
  assert(wd_read_request(root, &id, method, &got));
  assert(id == 9);
  assert(strcmp(method, "session.list") == 0);
  uint64_t limit = 0;
  assert(wd_uint_of(wd_get(got, "limit"), &limit) && limit == 7);
  wd_free(root);
}

static void test_error_response(void) {
  cbor_item_t *response = wd_response_err(3, WD_ERR_NOT_FOUND, "gone");
  size_t len = 0;
  uint8_t *bytes = wd_encode(response, &len);
  cbor_item_t *root = wd_decode(bytes, len);
  free(bytes);
  assert(root != NULL);
  uint64_t id = 0;
  bool ok = true;
  cbor_item_t *value = NULL;
  assert(wd_read_response(root, &id, &ok, &value));
  assert(id == 3 && !ok);
  char *name = wd_text_copy(wd_get(value, "name"));
  char *message = wd_text_copy(wd_get(value, "message"));
  assert(name != NULL && strcmp(name, "not_found") == 0);
  assert(message != NULL && strcmp(message, "gone") == 0);
  free(name);
  free(message);
  wd_free(root);
  assert(strcmp(wd_error_name(WD_ERR_LIMIT), "limit") == 0);
  assert(strcmp(wd_error_name((wd_error)999), "internal") == 0);
}

static void test_json(void) {
  cbor_item_t *root = wd_map(3);
  wd_put(root, "id", wd_text("ab\"c"));
  wd_put(root, "n", wd_uint(42));
  cbor_item_t *array = wd_array(2);
  cbor_item_t *one = wd_uint(1);
  cbor_item_t *yes = wd_bool(true);
  assert(cbor_array_push(array, one));
  assert(cbor_array_push(array, yes));
  cbor_decref(&one);
  cbor_decref(&yes);
  wd_put(root, "list", array);
  char buffer[512];
  size_t n = wd_json(root, buffer, sizeof(buffer));
  assert(strstr(buffer, "\"id\":\"ab\\\"c\"") != NULL);
  assert(strstr(buffer, "\"n\":42") != NULL);
  assert(strstr(buffer, "\"list\":[1,true]") != NULL);
  assert(n == strlen(buffer));
  wd_free(root);
}

static void test_rejections(void) {
  cbor_item_t *root = wd_map(2);
  wd_put(root, "id", wd_uint(0));
  wd_put(root, "method", wd_text("x"));
  uint64_t id = 0;
  char method[WD_MAX_METHOD];
  cbor_item_t *params = NULL;
  assert(!wd_read_request(root, &id, method, &params));
  wd_free(root);

  cbor_item_t *empty = wd_decode((const uint8_t *)"\xff", 1);
  assert(empty == NULL);

  const uint8_t huge_array[] = {0x62, 0x69, 0x64, 0x9b, 0x00, 0x00, 0x00,
                                0x80, 0x00, 0x00, 0x01, 0x66};
  assert(wd_decode(huge_array, sizeof(huge_array)) == NULL);

  uint8_t deep[80];
  for (size_t i = 0; i < sizeof(deep) - 1; i++) deep[i] = 0x81;
  deep[sizeof(deep) - 1] = 0x00;
  assert(wd_decode(deep, sizeof(deep)) == NULL);
}

int main(void) {
  test_hello();
  test_request_round_trip();
  test_error_response();
  test_json();
  test_rejections();
  printf("codec tests passed\n");
  return 0;
}
