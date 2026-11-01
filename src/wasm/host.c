/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wasm/host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wasmtime.h>

#include "core/util.h"

struct wd_engine {
  wasm_engine_t *engine;
  wasmtime_linker_t *linker;
};

struct wd_module {
  wd_engine *engine;
  wasmtime_module_t *module;
  char name[WD_MAX_MODULE_NAME];
  const char *caps[WD_MAX_CAPABILITIES];
  size_t ncap;
  bool pure;
};

struct wd_instance {
  wasmtime_store_t *store;
  wasmtime_context_t *ctx;
  wasmtime_instance_t instance;
  wasmtime_memory_t memory;
  wasmtime_func_t alloc;
  wasmtime_func_t call;
  bool has_call;
  wasmtime_func_t state_export;
  bool has_state_export;
  wasmtime_func_t state_import;
  bool has_state_import;
  wasmtime_func_t state_migrate;
  bool has_state_migrate;
  const char *caps[WD_MAX_CAPABILITIES];
  size_t ncap;
  wd_capability_handler handler;
  void *handler_ctx;
  uint64_t fuel;
  uint8_t *output;
  size_t output_len;
  size_t output_cap;
};

static void set_err(char **err, const char *fmt, ...) {
  if (err == NULL) return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  *err = strdup(buf);
}

static void wasmtime_err(char **err, wasmtime_error_t *e) {
  char buf[512];
  wasm_name_t message;
  wasmtime_error_message(e, &message);
  size_t n = message.size < sizeof(buf) - 1 ? message.size : sizeof(buf) - 1;
  memcpy(buf, message.data, n);
  buf[n] = '\0';
  wasm_byte_vec_delete(&message);
  wasmtime_error_delete(e);
  if (err == NULL) return;
  *err = strdup(buf);
}

static wasm_trap_t *trap_text(const char *text) {
  return wasmtime_trap_new(text, strlen(text));
}

static void trap_err(char **err, wasm_trap_t *trap) {
  wasmtime_trap_code_t code = 0;
  if (wasmtime_trap_code(trap, &code)) {
    if (code == WASMTIME_TRAP_CODE_OUT_OF_FUEL) {
      set_err(err, "fuel exhausted");
      wasm_trap_delete(trap);
      return;
    }
    if (code == WASMTIME_TRAP_CODE_INTERRUPT) {
      set_err(err, "interrupted");
      wasm_trap_delete(trap);
      return;
    }
  }
  char buf[512];
  wasm_message_t message;
  wasm_trap_message(trap, &message);
  size_t n = message.size < sizeof(buf) - 1 ? message.size : sizeof(buf) - 1;
  memcpy(buf, message.data, n);
  buf[n] = '\0';
  wasm_byte_vec_delete(&message);
  wasm_trap_delete(trap);
  set_err(err, "%s", buf);
}

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  size_t n = strlen(text);
  if (fwrite(text, 1, n, f) != n) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

static void enable_cache(wasm_config_t *config, const char *state_dir) {
  char *dir = wd_path_join(state_dir, "cache");
  char *store = NULL;
  char *toml = NULL;
  if (dir == NULL) return;
  if (wd_mkdir_p(dir, 0700) != 0) goto done;
  store = wd_path_join(dir, "wasmtime");
  toml = wd_path_join(dir, "wasmtime.toml");
  if (store == NULL || toml == NULL) goto done;
  if (wd_mkdir_p(store, 0700) != 0) goto done;
  char text[1024];
  snprintf(text, sizeof(text), "[cache]\nenabled = true\ndirectory = \"%s\"\n",
           store);
  if (write_file(toml, text) != 0) goto done;
  wasmtime_error_t *e = wasmtime_config_cache_config_load(config, toml);
  if (e != NULL) wasmtime_error_delete(e);
done:
  free(store);
  free(toml);
  free(dir);
}

static uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int append_output(wd_instance *inst, const unsigned char *buf,
                         size_t len) {
  if (inst->output_len + len > inst->output_cap) {
    size_t cap = inst->output_cap ? inst->output_cap : 512;
    while (cap < inst->output_len + len) cap *= 2;
    uint8_t *grown = realloc(inst->output, cap);
    if (grown == NULL) return -1;
    inst->output = grown;
    inst->output_cap = cap;
  }
  memcpy(inst->output + inst->output_len, buf, len);
  inst->output_len += len;
  return 0;
}

static ptrdiff_t wasi_sink(void *data, const unsigned char *buf, size_t len) {
  wd_instance *inst = data;
  if (append_output(inst, buf, len) != 0) return -1;
  return (ptrdiff_t)len;
}

static bool capability_granted(const wd_instance *inst, const char *name) {
  for (size_t i = 0; i < inst->ncap; i++)
    if (strcmp(inst->caps[i], name) == 0) return true;
  return false;
}

static bool memory_span(wd_instance *inst, uint32_t at, uint64_t len,
                        uint8_t **base) {
  uint8_t *data = wasmtime_memory_data(inst->ctx, &inst->memory);
  size_t size = wasmtime_memory_data_size(inst->ctx, &inst->memory);
  if ((uint64_t)at + len > size) return false;
  *base = data;
  return true;
}

static wasm_trap_t *cap_call(void *env, wasmtime_caller_t *caller,
                             const wasmtime_val_t *args, size_t nargs,
                             wasmtime_val_t *results, size_t nresults) {
  (void)env;
  (void)results;
  if (nargs != 3 || nresults != 0) return trap_text("ward.cap arity");
  wasmtime_context_t *ctx = wasmtime_caller_context(caller);
  wd_instance *inst = wasmtime_context_get_data(ctx);
  if (inst == NULL) return trap_text("ward.cap no instance");
  uint64_t at = (uint32_t)args[0].of.i32;
  uint64_t len = (uint32_t)args[1].of.i32;
  uint32_t out_at = (uint32_t)args[2].of.i32;

  wasmtime_extern_t mem;
  if (!wasmtime_caller_export_get(caller, "memory", 6, &mem))
    return trap_text("ward.cap no memory");
  const uint8_t *base = wasmtime_memory_data(ctx, &mem.of.memory);
  size_t size = wasmtime_memory_data_size(ctx, &mem.of.memory);
  wasmtime_extern_delete(&mem);
  if (len < 4 || at + len > size) return trap_text("ward.cap out of bounds");
  uint32_t name_len = get_u32(base + at);
  if (name_len >= 64 || (uint64_t)name_len + 4 > len)
    return trap_text("ward.cap bad name");
  char name[64];
  memcpy(name, base + at + 4, name_len);
  name[name_len] = '\0';

  size_t payload_len = (size_t)(len - 4 - name_len);
  uint8_t *payload = NULL;
  if (payload_len > 0) {
    payload = malloc(payload_len);
    if (payload == NULL) return trap_text("ward.cap out of memory");
    memcpy(payload, base + at + 4 + name_len, payload_len);
  }

  if (!capability_granted(inst, name)) {
    free(payload);
    char msg[128];
    snprintf(msg, sizeof(msg), "capability denied: %s", name);
    return trap_text(msg);
  }
  if (inst->handler == NULL) {
    free(payload);
    return trap_text("ward.cap no handler");
  }

  uint8_t *out = NULL;
  size_t out_len = 0;
  char *err = NULL;
  if (inst->handler(inst->handler_ctx, name, payload, payload_len, &out,
                    &out_len, &err) != 0) {
    free(payload);
    char msg[512];
    snprintf(msg, sizeof(msg), "%s", err != NULL ? err : "ward.cap failed");
    free(err);
    free(out);
    return trap_text(msg);
  }
  free(payload);

  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = (int32_t)out_len;
  wasmtime_val_t res;
  wasm_trap_t *trap = NULL;
  wasmtime_error_t *we =
      wasmtime_func_call(ctx, &inst->alloc, &arg, 1, &res, 1, &trap);
  if (we != NULL) {
    char *text = NULL;
    wasmtime_err(&text, we);
    char msg[512];
    snprintf(msg, sizeof(msg), "ward.cap alloc: %s",
             text != NULL ? text : "failed");
    free(text);
    free(out);
    return trap_text(msg);
  }
  if (trap != NULL) {
    free(out);
    return trap;
  }
  uint32_t dest = (uint32_t)res.of.i32;

  uint8_t *base2 = NULL;
  if (!memory_span(inst, dest, out_len, &base2) ||
      !memory_span(inst, out_at, 8, &base2)) {
    free(out);
    return trap_text("ward.cap result out of bounds");
  }
  if (out_len > 0) memcpy(base2 + dest, out, out_len);
  put_u32(base2 + out_at, dest);
  put_u32(base2 + out_at + 4, (uint32_t)out_len);
  free(out);
  return NULL;
}

wd_engine *wd_engine_new(const char *state_dir) {
  wd_engine *e = calloc(1, sizeof(*e));
  if (e == NULL) return NULL;
  wasm_config_t *config = wasm_config_new();
  if (config == NULL) {
    free(e);
    return NULL;
  }
  wasmtime_config_consume_fuel_set(config, true);
  if (state_dir != NULL) enable_cache(config, state_dir);
  e->engine = wasm_engine_new_with_config(config);
  if (e->engine == NULL) {
    free(e);
    return NULL;
  }
  e->linker = wasmtime_linker_new(e->engine);
  if (e->linker == NULL) {
    wasm_engine_delete(e->engine);
    free(e);
    return NULL;
  }
  wasm_valtype_t *params[3] = {wasm_valtype_new_i32(), wasm_valtype_new_i32(),
                               wasm_valtype_new_i32()};
  wasm_valtype_vec_t params_vec;
  wasm_valtype_vec_new(&params_vec, 3, params);
  wasm_valtype_vec_t returns_vec;
  wasm_valtype_vec_new_empty(&returns_vec);
  wasm_functype_t *type = wasm_functype_new(&params_vec, &returns_vec);
  wasmtime_error_t *err =
      wasmtime_linker_define_wasi(e->linker);
  if (err == NULL)
    err = wasmtime_linker_define_func(e->linker, "ward", 4, "cap", 3, type,
                                      cap_call, NULL, NULL);
  wasm_functype_delete(type);
  if (err != NULL) {
    wasmtime_error_delete(err);
    wasmtime_linker_delete(e->linker);
    wasm_engine_delete(e->engine);
    free(e);
    return NULL;
  }
  return e;
}

void wd_engine_free(wd_engine *engine) {
  if (engine == NULL) return;
  if (engine->linker != NULL) wasmtime_linker_delete(engine->linker);
  if (engine->engine != NULL) wasm_engine_delete(engine->engine);
  free(engine);
}

static bool name_is(const wasm_name_t *name, const char *text) {
  size_t n = strlen(text);
  return name->size == n && memcmp(name->data, text, n) == 0;
}

static bool check_imports(wd_module *mod, const wd_module_spec *spec,
                          char **err) {
  wasm_importtype_vec_t imports;
  wasmtime_module_imports(mod->module, &imports);
  bool ok = true;
  for (size_t i = 0; i < imports.size && ok; i++) {
    const wasm_name_t *module = wasm_importtype_module(imports.data[i]);
    const wasm_name_t *name = wasm_importtype_name(imports.data[i]);
    if (spec->pure) {
      set_err(err, "%s: pure module may not import %.*s.%.*s", spec->name,
              (int)module->size, module->data, (int)name->size, name->data);
      ok = false;
    } else if (name_is(module, "wasi_snapshot_preview1")) {
      continue;
    } else if (name_is(module, "ward") && name_is(name, "cap")) {
      continue;
    } else {
      set_err(err, "%s: unknown import %.*s.%.*s", spec->name,
              (int)module->size, module->data, (int)name->size, name->data);
      ok = false;
    }
  }
  wasm_importtype_vec_delete(&imports);
  return ok;
}

static bool check_exports(wd_module *mod, char **err) {
  static const char *required[] = {"memory", "ward_alloc", "ward_call"};
  wasm_exporttype_vec_t exports;
  wasmtime_module_exports(mod->module, &exports);
  for (size_t r = 0; r < sizeof(required) / sizeof(required[0]); r++) {
    bool found = false;
    for (size_t i = 0; i < exports.size && !found; i++)
      found = name_is(wasm_exporttype_name(exports.data[i]), required[r]);
    if (!found) {
      set_err(err, "%s: missing export %s", mod->name, required[r]);
      wasm_exporttype_vec_delete(&exports);
      return false;
    }
  }
  wasm_exporttype_vec_delete(&exports);
  return true;
}

wd_module *wd_module_load(wd_engine *engine, const wd_module_spec *spec,
                          const uint8_t *wasm, size_t len, char **err) {
  wd_module *mod = calloc(1, sizeof(*mod));
  if (mod == NULL) {
    set_err(err, "out of memory");
    return NULL;
  }
  wasmtime_error_t *e =
      wasmtime_module_new(engine->engine, wasm, len, &mod->module);
  if (e != NULL) {
    wasmtime_err(err, e);
    free(mod);
    return NULL;
  }
  mod->engine = engine;
  mod->pure = spec->pure;
  snprintf(mod->name, sizeof(mod->name), "%s", spec->name);
  for (size_t i = 0; i < spec->ncapabilities && i < WD_MAX_CAPABILITIES; i++)
    mod->caps[mod->ncap++] = spec->capabilities[i];
  if (!check_imports(mod, spec, err) || !check_exports(mod, err)) {
    wasmtime_module_delete(mod->module);
    free(mod);
    return NULL;
  }
  return mod;
}

void wd_module_free(wd_module *module) {
  if (module == NULL) return;
  wasmtime_module_delete(module->module);
  free(module);
}

const char *wd_module_name(const wd_module *module) { return module->name; }

static void wasi_setup(wd_instance *inst) {
  wasi_config_t *wasi = wasi_config_new();
  if (wasi == NULL) return;
  wasi_config_set_argv(wasi, 0, NULL);
  wasi_config_set_env(wasi, 0, NULL, NULL);
  wasi_config_set_stdout_custom(wasi, wasi_sink, inst, NULL);
  wasi_config_set_stderr_custom(wasi, wasi_sink, inst, NULL);
  wasmtime_context_set_wasi(inst->ctx, wasi);
}

static bool resolve_export(wd_instance *inst, const char *name,
                           wasmtime_extern_t *out) {
  return wasmtime_instance_export_get(inst->ctx, &inst->instance, name,
                                      strlen(name), out);
}

static bool func_export(wd_instance *inst, const char *name,
                        wasmtime_func_t *out) {
  wasmtime_extern_t item;
  bool found = false;
  if (resolve_export(inst, name, &item)) {
    if (item.kind == WASMTIME_EXTERN_FUNC) {
      *out = item.of.func;
      found = true;
    }
    wasmtime_extern_delete(&item);
  }
  return found;
}

static bool bind_exports(wd_instance *inst) {
  wasmtime_extern_t item;
  if (resolve_export(inst, "memory", &item)) {
    if (item.kind == WASMTIME_EXTERN_MEMORY) inst->memory = item.of.memory;
    wasmtime_extern_delete(&item);
  }
  bool alloc = func_export(inst, "ward_alloc", &inst->alloc);
  inst->has_call = func_export(inst, "ward_call", &inst->call);
  inst->has_state_export =
      func_export(inst, "ward_state_export", &inst->state_export);
  inst->has_state_import =
      func_export(inst, "ward_state_import", &inst->state_import);
  inst->has_state_migrate =
      func_export(inst, "ward_state_migrate", &inst->state_migrate);
  return alloc;
}

wd_instance *wd_instance_new(wd_module *module, const char *const *granted,
                             size_t ngranted, const wd_limits *limits,
                             wd_capability_handler handler, void *ctx,
                             char **err) {
  for (size_t i = 0; i < module->ncap; i++) {
    bool ok = false;
    for (size_t g = 0; g < ngranted && !ok; g++)
      ok = strcmp(module->caps[i], granted[g]) == 0;
    if (!ok) {
      set_err(err, "%s: capability not granted: %s", module->name,
              module->caps[i]);
      return NULL;
    }
  }
  if (limits->fuel == 0) {
    set_err(err, "%s: fuel must be positive", module->name);
    return NULL;
  }
  wd_instance *inst = calloc(1, sizeof(*inst));
  if (inst == NULL) {
    set_err(err, "out of memory");
    return NULL;
  }
  inst->handler = handler;
  inst->handler_ctx = ctx;
  inst->fuel = limits->fuel;
  for (size_t i = 0; i < ngranted && i < WD_MAX_CAPABILITIES; i++)
    inst->caps[inst->ncap++] = granted[i];

  inst->store = wasmtime_store_new(module->engine->engine, inst, NULL);
  if (inst->store == NULL) {
    set_err(err, "store");
    free(inst);
    return NULL;
  }
  inst->ctx = wasmtime_store_context(inst->store);
  wasmtime_store_limiter(inst->store, limits->max_memory, -1,
                         limits->max_instances, -1, 1);
  wasi_setup(inst);

  wasm_trap_t *trap = NULL;
  wasmtime_error_t *e = wasmtime_linker_instantiate(
      module->engine->linker, inst->ctx, module->module, &inst->instance, &trap);
  if (e != NULL || trap != NULL) {
    if (e != NULL) wasmtime_err(err, e);
    else trap_err(err, trap);
    wd_instance_free(inst);
    return NULL;
  }
  if (!bind_exports(inst)) {
    set_err(err, "%s: unresolved exports", module->name);
    wd_instance_free(inst);
    return NULL;
  }
  return inst;
}

void wd_instance_free(wd_instance *instance) {
  if (instance == NULL) return;
  if (instance->store != NULL) wasmtime_store_delete(instance->store);
  free(instance->output);
  free(instance);
}

const uint8_t *wd_instance_output(const wd_instance *instance, size_t *len) {
  if (len != NULL) *len = instance->output_len;
  return instance->output;
}

void wd_instance_clear_output(wd_instance *instance) { instance->output_len = 0; }

static int call_func(wd_instance *inst, wasmtime_func_t *func,
                     const wasmtime_val_t *args, size_t nargs,
                     wasmtime_val_t *results, size_t nresults, char **err) {
  wasmtime_error_t *fuel = wasmtime_context_set_fuel(inst->ctx, inst->fuel);
  if (fuel != NULL) {
    wasmtime_err(err, fuel);
    return -1;
  }
  wasm_trap_t *trap = NULL;
  wasmtime_error_t *e = wasmtime_func_call(inst->ctx, func, args, nargs, results,
                                           nresults, &trap);
  if (e != NULL) {
    wasmtime_err(err, e);
    return -1;
  }
  if (trap != NULL) {
    trap_err(err, trap);
    return -1;
  }
  return 0;
}

static int alloc_guest(wd_instance *inst, size_t len, uint32_t *at, char **err) {
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = (int32_t)len;
  wasmtime_val_t res;
  if (call_func(inst, &inst->alloc, &arg, 1, &res, 1, err) != 0) return -1;
  *at = (uint32_t)res.of.i32;
  uint8_t *base = NULL;
  if (!memory_span(inst, *at, len, &base)) {
    set_err(err, "allocator returned an out-of-bounds span");
    return -1;
  }
  return 0;
}

static int read_pair(wd_instance *inst, uint32_t at, uint32_t *ptr, uint32_t *len,
                     char **err) {
  uint8_t *base = NULL;
  if (!memory_span(inst, at, 8, &base)) {
    set_err(err, "result descriptor out of bounds");
    return -1;
  }
  *ptr = get_u32(base + at);
  *len = get_u32(base + at + 4);
  return 0;
}

int wd_instance_call(wd_instance *instance, const uint8_t *in, size_t in_len,
                     uint8_t **out, size_t *out_len, char **err) {
  if (!instance->has_call) {
    set_err(err, "module does not export ward_call");
    return -1;
  }
  if (in_len > UINT32_MAX) {
    set_err(err, "message too large");
    return -1;
  }
  uint32_t desc = 0;
  if (alloc_guest(instance, 8, &desc, err) != 0) return -1;
  uint32_t at = 0;
  if (alloc_guest(instance, in_len, &at, err) != 0) return -1;
  uint8_t *base = NULL;
  if (!memory_span(instance, at, in_len, &base)) {
    set_err(err, "input out of bounds");
    return -1;
  }
  if (in_len > 0) memcpy(base + at, in, in_len);

  wasmtime_val_t args[3];
  args[0].kind = WASMTIME_I32;
  args[0].of.i32 = (int32_t)at;
  args[1].kind = WASMTIME_I32;
  args[1].of.i32 = (int32_t)in_len;
  args[2].kind = WASMTIME_I32;
  args[2].of.i32 = (int32_t)desc;
  if (call_func(instance, &instance->call, args, 3, NULL, 0, err) != 0)
    return -1;

  uint32_t rat = 0;
  uint32_t rlen = 0;
  if (read_pair(instance, desc, &rat, &rlen, err) != 0) return -1;
  if (!memory_span(instance, rat, rlen, &base)) {
    set_err(err, "ward_call returned an out-of-bounds span");
    return -1;
  }
  uint8_t *copy = NULL;
  if (rlen > 0) {
    copy = malloc(rlen);
    if (copy == NULL) {
      set_err(err, "out of memory");
      return -1;
    }
    memcpy(copy, base + rat, rlen);
  }
  *out = copy;
  *out_len = rlen;
  return 0;
}

int wd_instance_state_export(wd_instance *instance, uint8_t **out,
                             size_t *out_len, char **err) {
  if (!instance->has_state_export) {
    set_err(err, "module does not export state");
    return -1;
  }
  uint32_t desc = 0;
  if (alloc_guest(instance, 8, &desc, err) != 0) return -1;
  wasmtime_val_t arg;
  arg.kind = WASMTIME_I32;
  arg.of.i32 = (int32_t)desc;
  if (call_func(instance, &instance->state_export, &arg, 1, NULL, 0, err) != 0)
    return -1;
  uint32_t at = 0;
  uint32_t len = 0;
  if (read_pair(instance, desc, &at, &len, err) != 0) return -1;
  uint8_t *base = NULL;
  if (!memory_span(instance, at, len, &base)) {
    set_err(err, "state export out of bounds");
    return -1;
  }
  uint8_t *copy = NULL;
  if (len > 0) {
    copy = malloc(len);
    if (copy == NULL) {
      set_err(err, "out of memory");
      return -1;
    }
    memcpy(copy, base + at, len);
  }
  *out = copy;
  *out_len = len;
  return 0;
}

int wd_state_handover(wd_instance *from, wd_instance *to,
                      uint32_t from_schema, uint32_t to_schema, char **err) {
  if (from == NULL || to == NULL) {
    set_err(err, "no instances to hand over between");
    return -1;
  }
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  if (wd_instance_state_export(from, &blob, &blob_len, err) != 0) return -1;
  if (from_schema != to_schema) {
    uint8_t *migrated = NULL;
    size_t migrated_len = 0;
    if (wd_instance_state_migrate(to, from_schema, blob, blob_len, &migrated,
                                  &migrated_len, err) != 0) {
      free(blob);
      return -1;
    }
    free(blob);
    blob = migrated;
    blob_len = migrated_len;
  }
  if (wd_instance_state_import(to, blob, blob_len, err) != 0) {
    free(blob);
    return -1;
  }
  free(blob);
  return 0;
}

int wd_instance_state_migrate(wd_instance *instance, uint32_t from_schema,
                              const uint8_t *in, size_t in_len, uint8_t **out,
                              size_t *out_len, char **err) {
  if (!instance->has_state_migrate) {
    set_err(err, "module does not migrate state");
    return -1;
  }
  if (in_len > UINT32_MAX) {
    set_err(err, "state too large");
    return -1;
  }
  uint32_t at = 0;
  if (alloc_guest(instance, in_len, &at, err) != 0) return -1;
  uint8_t *base = NULL;
  if (!memory_span(instance, at, in_len, &base)) {
    set_err(err, "state out of bounds");
    return -1;
  }
  if (in_len > 0) memcpy(base + at, in, in_len);
  uint32_t desc = 0;
  if (alloc_guest(instance, 8, &desc, err) != 0) return -1;
  wasmtime_val_t args[4];
  args[0].kind = WASMTIME_I32;
  args[0].of.i32 = (int32_t)from_schema;
  args[1].kind = WASMTIME_I32;
  args[1].of.i32 = (int32_t)at;
  args[2].kind = WASMTIME_I32;
  args[2].of.i32 = (int32_t)in_len;
  args[3].kind = WASMTIME_I32;
  args[3].of.i32 = (int32_t)desc;
  wasmtime_val_t res[1];
  if (call_func(instance, &instance->state_migrate, args, 4, res, 0, err) != 0)
    return -1;
  uint32_t ptr = 0;
  uint32_t len = 0;
  if (read_pair(instance, desc, &ptr, &len, err) != 0) return -1;
  uint8_t *span = NULL;
  if (!memory_span(instance, ptr, len, &span)) {
    set_err(err, "migrated state out of bounds");
    return -1;
  }
  uint8_t *copy = malloc(len == 0 ? 1 : len);
  if (copy == NULL) {
    set_err(err, "out of memory");
    return -1;
  }
  if (len > 0) memcpy(copy, span + ptr, len);
  *out = copy;
  *out_len = len;
  return 0;
}

int wd_instance_state_import(wd_instance *instance, const uint8_t *in,
                             size_t in_len, char **err) {
  if (!instance->has_state_import) {
    set_err(err, "module does not import state");
    return -1;
  }
  if (in_len > UINT32_MAX) {
    set_err(err, "state too large");
    return -1;
  }
  uint32_t at = 0;
  if (alloc_guest(instance, in_len, &at, err) != 0) return -1;
  uint8_t *base = NULL;
  if (!memory_span(instance, at, in_len, &base)) {
    set_err(err, "state out of bounds");
    return -1;
  }
  if (in_len > 0) memcpy(base + at, in, in_len);
  wasmtime_val_t args[2];
  args[0].kind = WASMTIME_I32;
  args[0].of.i32 = (int32_t)at;
  args[1].kind = WASMTIME_I32;
  args[1].of.i32 = (int32_t)in_len;
  wasmtime_val_t res;
  if (call_func(instance, &instance->state_import, args, 2, &res, 1, err) != 0)
    return -1;
  if ((uint32_t)res.of.i32 != 0) {
    set_err(err, "state import rejected");
    return -1;
  }
  return 0;
}
