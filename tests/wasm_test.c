/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm/host.h"

static uint8_t *read_file(const char *name, size_t *len) {
  const char *dir = getenv("WARD_GUEST_DIR");
  assert(dir != NULL);
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "cannot open guest %s\n", path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)size);
  assert(buf != NULL);
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    fprintf(stderr, "cannot read guest %s\n", path);
    exit(1);
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

static int echo_handler(void *ctx, const char *capability, const uint8_t *in,
                        size_t in_len, uint8_t **out, size_t *out_len,
                        char **err) {
  int *calls = ctx;
  (void)err;
  if (strcmp(capability, "echo") != 0) return -1;
  (*calls)++;
  if (in_len > 0) {
    uint8_t *copy = malloc(in_len);
    assert(copy != NULL);
    memcpy(copy, in, in_len);
    *out = copy;
    *out_len = in_len;
  } else {
    *out = NULL;
    *out_len = 0;
  }
  return 0;
}

static const wd_limits LIMITS = {.fuel = 10000000, .max_memory = 1 << 24,
                                 .max_instances = 4};

static wd_engine *engine;

static wd_module *load(const wd_module_spec *spec, const char *name,
                       char **err) {
  size_t len = 0;
  uint8_t *bytes = read_file(name, &len);
  wd_module *module = wd_module_load(engine, spec, bytes, len, err);
  free(bytes);
  return module;
}

static wd_instance *instantiate(wd_module *module, const char *const *granted,
                                size_t ngranted, int *calls, char **err) {
  return wd_instance_new(module, granted, ngranted, &LIMITS, echo_handler,
                         calls, err);
}

static void expect_call(wd_instance *inst, const uint8_t *in, size_t in_len,
                        const uint8_t *want, size_t want_len) {
  uint8_t *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  assert(wd_instance_call(inst, in, in_len, &out, &out_len, &err) == 0);
  free(err);
  assert(out_len == want_len);
  if (want_len > 0) assert(memcmp(out, want, want_len) == 0);
  free(out);
}

static void expect_error(wd_instance *inst, const uint8_t *in, size_t in_len,
                         const char *fragment) {
  uint8_t *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  assert(wd_instance_call(inst, in, in_len, &out, &out_len, &err) == -1);
  assert(err != NULL);
  if (strstr(err, fragment) == NULL) {
    fprintf(stderr, "expected error containing '%s', got '%s'\n", fragment,
            err);
    assert(0);
  }
  free(err);
  free(out);
}

static const char *const GRANTED[] = {"echo"};

static void test_echo_and_counter(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  assert(strcmp(wd_module_name(module), "echo") == 0);
  int calls = 0;
  wd_instance *inst = instantiate(module, GRANTED, 1, &calls, &err);
  assert(inst != NULL);
  static const uint8_t hello[] = {0, 'h', 'e', 'l', 'l', 'o'};
  expect_call(inst, hello, sizeof(hello), hello + 1, sizeof(hello) - 1);
  uint8_t counter[] = {1};
  uint8_t got[4];
  uint8_t *out = NULL;
  size_t out_len = 0;
  assert(wd_instance_call(inst, counter, 1, &out, &out_len, &err) == 0);
  memcpy(got, out, 4);
  free(out);
  assert(got[0] == 1 && got[1] == 0 && got[2] == 0 && got[3] == 0);
  assert(wd_instance_call(inst, counter, 1, &out, &out_len, &err) == 0);
  memcpy(got, out, 4);
  free(out);
  assert(got[0] == 2);
  wd_instance_free(inst);
  wd_module_free(module);
}

static void test_state(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  int calls = 0;
  wd_instance *a = instantiate(module, GRANTED, 1, &calls, &err);
  assert(a != NULL);
  static const uint8_t set[] = {2, 's', 't', 'a', 't', 'e'};
  expect_call(a, set, sizeof(set), set + 1, sizeof(set) - 1);
  uint8_t *state = NULL;
  size_t state_len = 0;
  assert(wd_instance_state_export(a, &state, &state_len, &err) == 0);
  assert(state_len == 5 && memcmp(state, "state", 5) == 0);
  wd_instance *b = instantiate(module, GRANTED, 1, &calls, &err);
  assert(b != NULL);
  assert(wd_instance_state_import(b, state, state_len, &err) == 0);
  static const uint8_t get[] = {5};
  expect_call(b, get, 1, (const uint8_t *)"state", 5);
  wd_instance *m = instantiate(module, GRANTED, 1, &calls, &err);
  assert(m != NULL);
  uint8_t *migrated = NULL;
  size_t migrated_len = 0;
  assert(wd_instance_state_migrate(m, 1, state, state_len, &migrated,
                                   &migrated_len, &err) == 0);
  assert(migrated_len == state_len + 1);
  assert(migrated[0] == 2);
  assert(memcmp(migrated + 1, "state", 5) == 0);
  free(migrated);
  wd_instance_free(m);
  free(state);
  wd_instance_free(a);
  wd_instance_free(b);
  wd_module_free(module);
}

static void test_fuel(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  int calls = 0;
  wd_instance *inst = instantiate(module, GRANTED, 1, &calls, &err);
  assert(inst != NULL);
  static const uint8_t spin[] = {3};
  expect_error(inst, spin, 1, "fuel");
  static const uint8_t hello[] = {0, 'o', 'k'};
  expect_call(inst, hello, sizeof(hello), hello + 1, sizeof(hello) - 1);
  wd_instance_free(inst);
  wd_module_free(module);
}

static void test_capability(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  int calls = 0;
  wd_instance *inst = instantiate(module, GRANTED, 1, &calls, &err);
  assert(inst != NULL);
  static const uint8_t want_echo[] = {4, 'e', 'c', 'h', 'o'};
  uint8_t *out = NULL;
  size_t out_len = 0;
  assert(wd_instance_call(inst, want_echo, sizeof(want_echo), &out, &out_len,
                          &err) == 0);
  assert(calls == 1);
  assert(out_len == 0);
  free(out);
  static const uint8_t want_nope[] = {4, 'n', 'o', 'p', 'e'};
  expect_error(inst, want_nope, sizeof(want_nope), "capability denied: nope");
  wd_instance_free(inst);
  wd_module_free(module);
}

static void test_capability_refused_at_instantiate(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  int calls = 0;
  static const char *const other[] = {"net"};
  wd_instance *inst = instantiate(module, other, 1, &calls, &err);
  assert(inst == NULL);
  assert(err != NULL && strstr(err, "not granted") != NULL);
  free(err);
  wd_module_free(module);
}

static void test_pure_module() {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec pure = {"engine", caps, 0, true};
  wd_module *rejected = load(&pure, "wasi.wasm", &err);
  assert(rejected == NULL);
  assert(err != NULL && strstr(err, "pure module") != NULL);
  free(err);
  err = NULL;
  wd_module_spec impure = {"driver", caps, 0, false};
  wd_module *accepted = load(&impure, "wasi.wasm", &err);
  assert(accepted != NULL);
  wd_module_free(accepted);
}

static void test_unknown_import(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"driver", caps, 0, false};
  wd_module *module = load(&spec, "importer.wasm", &err);
  assert(module == NULL);
  assert(err != NULL && strstr(err, "unknown import") != NULL);
  free(err);
}

static void test_memory_limit(void) {
  char *err = NULL;
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"echo", caps, 1, false};
  wd_module *module = load(&spec, "echo.wasm", &err);
  assert(module != NULL);
  int calls = 0;
  wd_limits tiny = {.fuel = 100000, .max_memory = 4096, .max_instances = 1};
  wd_instance *inst = wd_instance_new(module, GRANTED, 1, &tiny, echo_handler,
                                      &calls, &err);
  assert(inst == NULL);
  assert(err != NULL);
  assert(strstr(err, "memory") != NULL);
  free(err);
  wd_module_free(module);
}

static void test_missing_module(void) {
  char *err = NULL;
  static const uint8_t junk[] = {0, 'a', 's', 'm'};
  static const char *const caps[] = {"echo"};
  wd_module_spec spec = {"engine", caps, 0, true};
  wd_module *module = wd_module_load(engine, &spec, junk, sizeof(junk), &err);
  assert(module == NULL);
  assert(err != NULL);
  free(err);
}

int main(void) {
  engine = wd_engine_new(NULL);
  assert(engine != NULL);
  test_echo_and_counter();
  test_state();
  test_fuel();
  test_capability();
  test_capability_refused_at_instantiate();
  test_pure_module();
  test_unknown_import();
  test_memory_limit();
  test_missing_module();
  wd_engine_free(engine);
  printf("wasm host tests passed\n");
  return 0;
}
