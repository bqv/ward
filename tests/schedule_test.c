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

static const wd_limits LIMITS = {.fuel = 400000000, .max_memory = 1 << 24,
                                 .max_instances = 4};

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

static uint64_t next_fire(wd_instance *inst, const char *cron, uint64_t from) {
  cbor_item_t *request = wd_map(2);
  wd_put(request, "cron", wd_text(cron));
  char text[32];
  snprintf(text, sizeof(text), "%llu", (unsigned long long)from);
  wd_put(request, "from_ms", wd_text(text));
  size_t len = 0;
  uint8_t *body = wd_encode(request, &len);
  assert(body != NULL);
  uint8_t *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  assert(wd_instance_call(inst, body, len, &out, &out_len, &err) == 0);
  free(body);
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  assert(reply != NULL);
  char *value = wd_text_copy(wd_get(reply, "next_ms"));
  assert(value != NULL);
  uint64_t ms = strtoull(value, NULL, 10);
  free(value);
  wd_free(reply);
  return ms;
}

int main(void) {
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  size_t len = 0;
  uint8_t *bytes = read_file("schedule.wasm", &len);
  static const char *const no_caps[] = {NULL};
  wd_module_spec spec = {"schedule", no_caps, 0, true};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, len, &err);
  assert(module != NULL);
  wd_instance *inst =
      wd_instance_new(module, no_caps, 0, &LIMITS, NULL, NULL, &err);
  assert(inst != NULL);

  uint64_t from = 1767225600000ull;
  uint64_t next = next_fire(inst, "*/5 * * * *", from);
  assert(next > from);
  assert(next % 300000ull == 0);
  assert(next - from <= 300000ull);

  uint64_t hourly = next_fire(inst, "0 * * * *", from);
  assert(hourly > from);
  assert(hourly % 3600000ull == 0);
  assert(hourly - from <= 3600000ull);

  uint64_t daily = next_fire(inst, "0 0 1 1 *", from);
  assert(daily > from);
  assert(daily - from <= 366ull * 86400000ull);

  uint64_t every = next_fire(inst, "@every 250", from);
  assert(every == from + 250);

  assert(next_fire(inst, "not a cron", from) == 0);
  assert(next_fire(inst, "61 * * * *", from) == 0);

  wd_instance_free(inst);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("schedule tests passed\n");
  return 0;
}
