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

static char *classify(const char *tool, const char *payload) {
  cbor_item_t *request = wd_map(2);
  wd_put(request, "tool", wd_text(tool));
  wd_put(request, "payload", wd_text(payload));
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
  char *verdict = wd_text_copy(wd_get(reply, "verdict"));
  assert(verdict != NULL);
  assert(wd_get(reply, "reason") != NULL);
  wd_free(reply);
  return verdict;
}

int main(void) {
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  size_t len = 0;
  uint8_t *bytes = read_file("judge.wasm", &len);
  static const char *const no_caps[] = {NULL};
  wd_module_spec spec = {"judge", no_caps, 0, true};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, len, &err);
  assert(module != NULL);
  instance = wd_instance_new(module, no_caps, 0, &LIMITS, NULL, NULL, &err);
  assert(instance != NULL);

  char *v = classify("write", "ls -l");
  assert(strcmp(v, "allow") == 0);
  free(v);
  v = classify("exec", "rm -rf /");
  assert(strcmp(v, "ask") == 0);
  free(v);
  v = classify("exec", "curl http://example.com");
  assert(strcmp(v, "ask") == 0);
  free(v);
  v = classify("read", "notes.txt");
  assert(strcmp(v, "allow") == 0);
  free(v);

  wd_instance_free(instance);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("judge guest tests passed\n");
  return 0;
}
