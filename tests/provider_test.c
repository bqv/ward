/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/codec.h"
#include "wasm/host.h"

static const wd_limits LIMITS = {.fuel = 50000000, .max_memory = 16 << 20,
                                 .max_instances = 2};

static uint8_t *read_file(const char *name, size_t *len) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  assert(f != NULL);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)size);
  assert(buf != NULL);
  assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
  fclose(f);
  *len = (size_t)size;
  return buf;
}

int main(void) {
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  size_t len = 0;
  uint8_t *bytes = read_file("provider.wasm", &len);
  static const char *const caps[] = {"net.request"};
  wd_module_spec spec = {"provider", caps, 1, false};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, len, &err);
  assert(module != NULL);
  wd_instance *inst =
      wd_instance_new(module, caps, 1, &LIMITS, NULL, NULL, &err);
  assert(inst != NULL);

  cbor_item_t *history = wd_array(1);
  cbor_item_t *entry = wd_map(2);
  wd_put(entry, "role", wd_text("system"));
  wd_put(entry, "text", wd_text("be kind to the tools"));
  assert(cbor_array_push(history, entry));
  cbor_decref(&entry);

  cbor_item_t *payload = wd_map(2);
  wd_put(payload, "prompt", wd_text("hello"));
  wd_put(payload, "history", history);

  size_t in_len = 0;
  uint8_t *in = wd_encode(payload, &in_len);
  assert(in != NULL);
  uint8_t *out = NULL;
  size_t out_len = 0;
  assert(wd_instance_call(inst, in, in_len, &out, &out_len, &err) == 0);
  free(in);
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  assert(reply != NULL);
  cbor_item_t *request = wd_get(reply, "request");
  assert(request != NULL);
  char *body = wd_text_copy(wd_get(request, "body"));
  assert(body != NULL);
  assert(strstr(body, "be kind to the tools") != NULL);
  assert(strstr(body, "hello") != NULL);
  free(body);
  wd_free(reply);

  wd_instance_free(inst);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("provider tests passed\n");
  return 0;
}
