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

static wd_instance *instance;

static cbor_item_t *outline(const char *text) {
  cbor_item_t *request = wd_map(1);
  wd_put(request, "text", wd_text(text));
  size_t in_len = 0;
  uint8_t *in = wd_encode(request, &in_len);
  assert(in != NULL);
  uint8_t *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  assert(wd_instance_call(instance, in, in_len, &out, &out_len, &err) == 0);
  free(in);
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  assert(reply != NULL);
  return reply;
}

static char *name_at(cbor_item_t *reply, size_t index) {
  cbor_item_t *symbols = wd_get(reply, "symbols");
  assert(symbols != NULL);
  cbor_item_t *entry = cbor_array_get(symbols, index);
  assert(entry != NULL);
  char *name = wd_text_copy(wd_get(entry, "name"));
  assert(name != NULL);
  assert(wd_get(entry, "line") != NULL);
  cbor_decref(&entry);
  return name;
}

int main(void) {
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  size_t len = 0;
  uint8_t *bytes = read_file("code.wasm", &len);
  static const char *const no_caps[] = {NULL};
  wd_module_spec spec = {"code", no_caps, 0, true};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, len, &err);
  assert(module != NULL);
  instance = wd_instance_new(module, no_caps, 0, &LIMITS, NULL, NULL, &err);
  assert(instance != NULL);

  cbor_item_t *reply = outline(
      "int main(void) {\n  return 0;\n}\n\nstatic void helper(int x) {\n"
      "  if (x) { x++; }\n}\n");
  cbor_item_t *symbols = wd_get(reply, "symbols");
  assert(symbols != NULL && cbor_array_size(symbols) == 2);
  char *first = name_at(reply, 0);
  char *second = name_at(reply, 1);
  assert(strcmp(first, "main") == 0);
  assert(strcmp(second, "helper") == 0);
  free(first);
  free(second);
  wd_free(reply);

  reply = outline("struct s {\n  int a;\n};\n");
  symbols = wd_get(reply, "symbols");
  assert(symbols != NULL && cbor_array_size(symbols) == 0);
  wd_free(reply);

  wd_instance_free(instance);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("code guest tests passed\n");
  return 0;
}
