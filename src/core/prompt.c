/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/prompt.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/codec.h"

#define SECTION_MAX 32
#define NAME_MAX_LEN 64
#define SOURCE_MAX 512
#define TEXT_MAX (64 * 1024)

typedef struct {
  char name[NAME_MAX_LEN];
  int order;
  char source[SOURCE_MAX];
  char *text;
} section;

struct wd_prompt {
  section items[SECTION_MAX];
  size_t len;
};

static const char identity_text[] =
    "You are an agent powered by ward: a small core that owns I/O and "
    "enforcement, and reloadable modules that own every decision.";

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  char *buf = malloc(TEXT_MAX + 1);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, TEXT_MAX, f);
  fclose(f);
  if (got == 0) {
    free(buf);
    return NULL;
  }
  buf[got] = '\0';
  while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = '\0';
  return buf;
}

static void place(wd_prompt *prompt, const char *name, int order,
                  const char *source, char *text) {
  if (prompt == NULL || name == NULL || text == NULL) {
    free(text);
    return;
  }
  for (size_t i = 0; i < prompt->len; i++) {
    if (strcmp(prompt->items[i].name, name) != 0) continue;
    free(prompt->items[i].text);
    prompt->items[i].text = text;
    prompt->items[i].order = order;
    snprintf(prompt->items[i].source, SOURCE_MAX, "%s", source);
    return;
  }
  if (prompt->len >= SECTION_MAX) {
    free(text);
    return;
  }
  section *slot = &prompt->items[prompt->len++];
  snprintf(slot->name, NAME_MAX_LEN, "%s", name);
  slot->order = order;
  snprintf(slot->source, SOURCE_MAX, "%s", source);
  slot->text = text;
}

static void load_dir(wd_prompt *prompt, const char *state_dir) {
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s/prompt", state_dir);
  DIR *d = opendir(dir);
  if (d == NULL) return;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    const char *file = entry->d_name;
    if (file[0] == '.') continue;
    const char *dash = strchr(file, '-');
    if (dash == NULL) continue;
    int order = 0;
    int digits = 0;
    for (const char *p = file; p < dash; p++) {
      if (*p < '0' || *p > '9') {
        digits = -1;
        break;
      }
      order = order * 10 + (*p - '0');
      digits++;
    }
    if (digits <= 0) continue;
    const char *name = dash + 1;
    char path[1500];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    char *text = slurp(path);
    if (text == NULL) continue;
    char trimmed[NAME_MAX_LEN];
    snprintf(trimmed, sizeof(trimmed), "%s", name);
    size_t len = strlen(trimmed);
    if (len > 3 && strcmp(trimmed + len - 3, ".md") == 0) trimmed[len - 3] = '\0';
    place(prompt, trimmed, order, path, text);
  }
  closedir(d);
}

static int compare(const void *left, const void *right) {
  const section *a = left;
  const section *b = right;
  return a->order - b->order;
}

wd_prompt *wd_prompt_build(const char *state_dir, const char *root,
                           const char *context) {
  wd_prompt *prompt = calloc(1, sizeof(*prompt));
  if (prompt == NULL) return NULL;
  place(prompt, "identity", -100, "built-in", strdup(identity_text));
  if (root != NULL) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/AGENTS.md", root);
    char *rules = slurp(path);
    if (rules != NULL) place(prompt, "rules", 100, path, rules);
  }
  if (context != NULL && context[0] != '\0')
    place(prompt, "attachments", 120, "session", strdup(context));
  if (state_dir != NULL) load_dir(prompt, state_dir);
  if (prompt->len > 1) qsort(prompt->items, prompt->len, sizeof(section), compare);
  return prompt;
}

void wd_prompt_free(wd_prompt *prompt) {
  if (prompt == NULL) return;
  for (size_t i = 0; i < prompt->len; i++) free(prompt->items[i].text);
  free(prompt);
}

size_t wd_prompt_len(const wd_prompt *prompt) {
  return prompt != NULL ? prompt->len : 0;
}

cbor_item_t *wd_prompt_to_cbor(const wd_prompt *prompt) {
  size_t len = wd_prompt_len(prompt);
  cbor_item_t *array = wd_array(len);
  for (size_t i = 0; i < len; i++) {
    const section *s = &prompt->items[i];
    cbor_item_t *item = wd_map(4);
    wd_put(item, "name", wd_text(s->name));
    wd_put(item, "order", wd_uint((uint64_t)(s->order + 1000)));
    wd_put(item, "source", wd_text(s->source));
    wd_put(item, "text", wd_text(s->text));
    if (!cbor_array_push(array, item)) {
      cbor_decref(&item);
      break;
    }
    cbor_decref(&item);
  }
  return array;
}

cbor_item_t *wd_prompt_to_history(const wd_prompt *prompt,
                                 cbor_item_t *history) {
  size_t len = wd_prompt_len(prompt);
  size_t extra = history != NULL ? cbor_array_size(history) : 0;
  if (len == 0 && extra == 0) return NULL;
  cbor_item_t *array = wd_array(len + extra);
  for (size_t i = 0; i < len; i++) {
    cbor_item_t *entry = wd_map(2);
    wd_put(entry, "role", wd_text("system"));
    wd_put(entry, "text", wd_text(prompt->items[i].text));
    if (!cbor_array_push(array, entry)) {
      cbor_decref(&entry);
      break;
    }
    cbor_decref(&entry);
  }
  if (history != NULL) {
    cbor_item_t **items = cbor_array_handle(history);
    for (size_t i = 0; i < extra; i++) {
      if (!cbor_incref(items[i])) continue;
      if (!cbor_array_push(array, items[i])) break;
    }
  }
  return array;
}
