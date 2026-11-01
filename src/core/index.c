/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/index.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "proto/codec.h"

#define INDEX_FILE_MAX 200
#define INDEX_TEXT_MAX 262144
#define INDEX_DEPTH_MAX 6

typedef struct {
  FILE *out;
  wd_instance *code;
  uint64_t hash;
  int files;
  char *err;
} index_state;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t len) {
  const uint8_t *p = data;
  for (size_t i = 0; i < len; i++) {
    hash ^= p[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static int source_file(const char *name) {
  size_t len = strlen(name);
  if (len < 3) return 0;
  const char *dot = strrchr(name, '.');
  return dot != NULL && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0);
}

static uint8_t *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  size_t cap = 1 << 16, used = 0;
  uint8_t *bytes = malloc(cap);
  if (bytes == NULL) {
    fclose(f);
    return NULL;
  }
  size_t n;
  while ((n = fread(bytes + used, 1, cap - used, f)) > 0) {
    used += n;
    if (used > INDEX_TEXT_MAX) {
      free(bytes);
      fclose(f);
      return NULL;
    }
    if (used == cap) {
      uint8_t *grown = realloc(bytes, cap * 2);
      if (grown == NULL) {
        free(bytes);
        fclose(f);
        return NULL;
      }
      bytes = grown;
      cap *= 2;
    }
  }
  fclose(f);
  *len = used;
  return bytes;
}

static int index_one(index_state *st, const char *path, const char *rel) {
  size_t len = 0;
  uint8_t *bytes = slurp(path, &len);
  if (bytes == NULL) return 0;
  bytes = realloc(bytes, len + 1);
  if (bytes == NULL) return 0;
  bytes[len] = '\0';
  cbor_item_t *request = wd_map(1);
  wd_put(request, "text", wd_text((const char *)bytes));
  free(bytes);
  size_t in_len = 0;
  uint8_t *in = wd_encode(request, &in_len);
  if (in == NULL) return 0;
  uint8_t *reply = NULL;
  size_t reply_len = 0;
  char *err = NULL;
  int rc = wd_instance_call(st->code, in, in_len, &reply, &reply_len, &err);
  free(in);
  free(err);
  if (rc != 0) return 0;
  cbor_item_t *decoded = wd_decode(reply, reply_len);
  free(reply);
  if (decoded == NULL) return 0;
  cbor_item_t *symbols = wd_get(decoded, "symbols");
  size_t count = symbols != NULL ? cbor_array_size(symbols) : 0;
  for (size_t i = 0; i < count; i++) {
    cbor_item_t *entry = cbor_array_get(symbols, i);
    char *name = wd_text_copy(wd_get(entry, "name"));
    char *line = wd_text_copy(wd_get(entry, "line"));
    if (name != NULL && line != NULL) {
      fprintf(st->out, "%s\t%s\t%s\n", rel, name, line);
      st->hash = hash_bytes(st->hash, rel, strlen(rel));
      st->hash = hash_bytes(st->hash, name, strlen(name));
    }
    free(name);
    free(line);
    cbor_decref(&entry);
  }
  wd_free(decoded);
  return 1;
}

static void walk(index_state *st, const char *root, const char *rel, int depth) {
  if (depth > INDEX_DEPTH_MAX || st->files >= INDEX_FILE_MAX) return;
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s%s", root, rel);
  DIR *d = opendir(dir);
  if (d == NULL) return;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL && st->files < INDEX_FILE_MAX) {
    if (entry->d_name[0] == '.') continue;
    char child[1024];
    snprintf(child, sizeof(child), "%s/%s", rel, entry->d_name);
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", root, child);
    struct stat sb;
    if (stat(path, &sb) != 0) continue;
    if (S_ISDIR(sb.st_mode)) {
      walk(st, root, child, depth + 1);
    } else if (S_ISREG(sb.st_mode) && source_file(entry->d_name)) {
      if (index_one(st, path, child + 1) != 0) st->files++;
    }
  }
  closedir(d);
}

int wd_index_build(const char *root, const char *cache_dir, wd_instance *code,
                   uint64_t *generation, char **err) {
  if (root == NULL || cache_dir == NULL || code == NULL) {
    if (err != NULL && *err == NULL) *err = strdup("index: bad arguments");
    return -1;
  }
  char symbols[1024];
  snprintf(symbols, sizeof(symbols), "%s/symbols.tsv", cache_dir);
  index_state st = {NULL, code, 1469598103934665603ull, 0, NULL};
  st.out = fopen(symbols, "w");
  if (st.out == NULL) {
    if (err != NULL && *err == NULL) *err = strdup("index: cannot write cache");
    return -1;
  }
  walk(&st, root, "", 0);
  fclose(st.out);
  char gen_path[1024];
  snprintf(gen_path, sizeof(gen_path), "%s/symbols.gen", cache_dir);
  FILE *g = fopen(gen_path, "w");
  if (g != NULL) {
    fprintf(g, "%llu\n", (unsigned long long)st.hash);
    fclose(g);
  }
  if (generation != NULL) *generation = st.hash;
  return 0;
}

int wd_index_find(const char *cache_dir, const char *name, char *out,
                  size_t cap) {
  if (cache_dir == NULL || name == NULL || out == NULL || cap == 0) return -1;
  char symbols[1024];
  snprintf(symbols, sizeof(symbols), "%s/symbols.tsv", cache_dir);
  FILE *f = fopen(symbols, "r");
  if (f == NULL) return -1;
  char line[1024];
  size_t used = 0;
  int found = 0;
  out[0] = '\0';
  while (fgets(line, sizeof(line), f) != NULL) {
    char *first = strchr(line, '\t');
    if (first == NULL) continue;
    char *second = strchr(first + 1, '\t');
    if (second == NULL) continue;
    char *newline = strchr(second + 1, '\n');
    if (newline != NULL) *newline = '\0';
    size_t name_len = (size_t)(second - first - 1);
    if (strncmp(first + 1, name, name_len) != 0 || strlen(name) != name_len)
      continue;
    char symbol[128];
    size_t copy = name_len < sizeof(symbol) - 1 ? name_len : sizeof(symbol) - 1;
    memcpy(symbol, first + 1, copy);
    symbol[copy] = '\0';
    *first = '\0';
    int n = snprintf(out + used, cap - used, "%s:%s %s\n", line, second + 1,
                     symbol);
    if (n > 0) used += (size_t)n;
    found++;
    if (used + 128 >= cap) break;
  }
  fclose(f);
  return found;
}
