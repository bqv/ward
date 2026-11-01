/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_WASM_HOST_H
#define WARD_WASM_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_MAX_CAPABILITIES 16
#define WD_MAX_MODULE_NAME 32

typedef struct wd_engine wd_engine;
typedef struct wd_module wd_module;
typedef struct wd_instance wd_instance;

typedef struct {
  const char *name;
  const char *const *capabilities;
  size_t ncapabilities;
  bool pure;
} wd_module_spec;

typedef struct {
  uint64_t fuel;
  int64_t max_memory;
  int64_t max_instances;
} wd_limits;

typedef int (*wd_capability_handler)(void *ctx, const char *capability,
                                     const uint8_t *in, size_t in_len,
                                     uint8_t **out, size_t *out_len,
                                     char **err);

wd_engine *wd_engine_new(const char *state_dir);
void wd_engine_free(wd_engine *engine);

wd_module *wd_module_load(wd_engine *engine, const wd_module_spec *spec,
                          const uint8_t *wasm, size_t len, char **err);
void wd_module_free(wd_module *module);
const char *wd_module_name(const wd_module *module);

wd_instance *wd_instance_new(wd_module *module, const char *const *granted,
                             size_t ngranted, const wd_limits *limits,
                             wd_capability_handler handler, void *ctx,
                             char **err);
void wd_instance_free(wd_instance *instance);

int wd_instance_call(wd_instance *instance, const uint8_t *in, size_t in_len,
                     uint8_t **out, size_t *out_len, char **err);
int wd_instance_state_export(wd_instance *instance, uint8_t **out,
                             size_t *out_len, char **err);
int wd_instance_state_import(wd_instance *instance, const uint8_t *in,
                             size_t in_len, char **err);
int wd_state_import_migrated(wd_instance *to, const uint8_t *blob,
                             size_t blob_len, uint32_t from_schema,
                             uint32_t to_schema, char **err);
int wd_state_handover(wd_instance *from, wd_instance *to,
                      uint32_t from_schema, uint32_t to_schema, char **err);
int wd_instance_state_migrate(wd_instance *instance, uint32_t from_schema,
                              const uint8_t *in, size_t in_len, uint8_t **out,
                              size_t *out_len, char **err);

const uint8_t *wd_instance_output(const wd_instance *instance, size_t *len);
void wd_instance_clear_output(wd_instance *instance);

#endif
