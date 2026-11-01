/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/models.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wd_models {
  wd_model *items;
  size_t len;
};

char *wd_models_path(const char *state_dir) {
  size_t len = strlen(state_dir) + strlen("models.toml") + 2;
  char *path = malloc(len);
  if (path == NULL) return NULL;
  snprintf(path, len, "%s/models.toml", state_dir);
  return path;
}

static char *trim(char *text) {
  while (*text != '\0' && isspace((unsigned char)*text)) text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) end--;
  *end = '\0';
  return text;
}

static char *unquote(const char *value) {
  size_t len = strlen(value);
  if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
    value++;
    len -= 2;
  }
  char *out = malloc(len + 1);
  if (out == NULL) return NULL;
  memcpy(out, value, len);
  out[len] = '\0';
  return out;
}

static int set_field(wd_model *model, const char *key, const char *value) {
  char **field = NULL;
  if (strcmp(key, "id") == 0)
    field = &model->id;
  else if (strcmp(key, "provider") == 0)
    field = &model->provider;
  else if (strcmp(key, "model") == 0)
    field = &model->model;
  else if (strcmp(key, "context") == 0)
    field = &model->context;
  else if (strcmp(key, "tags") == 0)
    field = &model->tags;
  else if (strcmp(key, "price") == 0)
    field = &model->price;
  else
    return 0;
  free(*field);
  *field = unquote(value);
  return *field == NULL ? -1 : 0;
}

static int push(wd_models *models, const wd_model *model, char **err) {
  for (size_t i = 0; i < models->len; i++) {
    if (strcmp(models->items[i].id, model->id) == 0) {
      if (err != NULL) *err = strdup("a model id is declared twice");
      return -1;
    }
  }
  wd_model *grown = realloc(models->items, (models->len + 1) * sizeof(*grown));
  if (grown == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return -1;
  }
  models->items = grown;
  models->items[models->len] = *model;
  models->len++;
  return 0;
}

wd_models *wd_models_load(const char *path, char **err) {
  if (err != NULL) *err = NULL;
  wd_models *models = calloc(1, sizeof(*models));
  if (models == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  FILE *file = fopen(path, "r");
  if (file == NULL) return models;

  wd_model current;
  memset(&current, 0, sizeof(current));
  int open = 0;
  char line[1024];
  while (fgets(line, sizeof(line), file) != NULL) {
    char *text = line;
    char *hash = strchr(text, '#');
    if (hash != NULL) *hash = '\0';
    text = trim(text);
    if (text[0] == '\0') continue;
    if (strcmp(text, "[[model]]") == 0) {
      if (open) {
        if (current.id == NULL) {
          if (err != NULL) *err = strdup("a model entry has no id");
          goto bad;
        }
        if (push(models, &current, err) != 0) goto bad;
      }
      memset(&current, 0, sizeof(current));
      open = 1;
      continue;
    }
    char *equals = strchr(text, '=');
    if (equals == NULL) continue;
    *equals = '\0';
    char *key = trim(text);
    char *value = trim(equals + 1);
    if (!open) continue;
    if (set_field(&current, key, value) != 0) {
      if (err != NULL) *err = strdup("out of memory");
      goto bad;
    }
  }
  if (open) {
    if (current.id == NULL) {
      if (err != NULL) *err = strdup("a model entry has no id");
      goto bad;
    }
    if (push(models, &current, err) != 0) goto bad;
  }
  fclose(file);
  return models;

bad:
  fclose(file);
  free(current.id);
  free(current.provider);
  free(current.model);
  free(current.context);
  free(current.tags);
  free(current.price);
  wd_models_free(models);
  return NULL;
}

void wd_models_free(wd_models *models) {
  if (models == NULL) return;
  for (size_t i = 0; i < models->len; i++) {
    free(models->items[i].id);
    free(models->items[i].provider);
    free(models->items[i].model);
    free(models->items[i].context);
    free(models->items[i].tags);
    free(models->items[i].price);
  }
  free(models->items);
  free(models);
}

size_t wd_models_count(const wd_models *models) {
  return models != NULL ? models->len : 0;
}

const wd_model *wd_models_at(const wd_models *models, size_t at) {
  if (models == NULL || at >= models->len) return NULL;
  return &models->items[at];
}

const wd_model *wd_models_find(const wd_models *models, const char *id) {
  if (models == NULL || id == NULL) return NULL;
  for (size_t i = 0; i < models->len; i++)
    if (strcmp(models->items[i].id, id) == 0) return &models->items[i];
  return NULL;
}
