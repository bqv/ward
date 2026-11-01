/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_MODELS_H
#define WARD_CORE_MODELS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *id;
  char *provider;
  char *model;
  char *context;
  char *tags;
  char *price;
  char *endpoint;
  char *key;
  char *max_output;
  char *device;
  char *slots;
  char *stop;
} wd_model;

typedef struct wd_models wd_models;

wd_models *wd_models_load(const char *path, char **err);
void wd_models_free(wd_models *models);

size_t wd_models_count(const wd_models *models);
const wd_model *wd_models_at(const wd_models *models, size_t at);
const wd_model *wd_models_find(const wd_models *models, const char *id);

char *wd_models_path(const char *state_dir);

#endif
