/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    text++;
  size_t n = strlen(text);
  while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
                   text[n - 1] == '\r' || text[n - 1] == '\n'))
    text[--n] = '\0';
  return text;
}

static int unquote(const char *value, char *out, size_t cap) {
  size_t n = strlen(value);
  if (n < 2 || value[0] != '"' || value[n - 1] != '"') return -1;
  n -= 2;
  if (n >= cap) return -1;
  memcpy(out, value + 1, n);
  out[n] = '\0';
  return 0;
}

static int parse_list(const char *value, wd_manifest *out, char **err) {
  const char *at = value;
  while (*at == ' ' || *at == '\t') at++;
  if (*at != '[') {
    if (err != NULL) *err = strdup("capabilities must be a list");
    return -1;
  }
  at++;
  for (;;) {
    while (*at == ' ' || *at == '\t' || *at == ',') at++;
    if (*at == ']') return 0;
    if (*at == '\0') {
      if (err != NULL) *err = strdup("capabilities list is not closed");
      return -1;
    }
    if (*at != '"') {
      if (err != NULL) *err = strdup("a capability must be quoted");
      return -1;
    }
    const char *open = at + 1;
    const char *close = strchr(open, '"');
    if (close == NULL) {
      if (err != NULL) *err = strdup("a capability is not closed");
      return -1;
    }
    size_t n = (size_t)(close - open);
    if (n == 0 || n >= WD_MANIFEST_NAME) {
      if (err != NULL) *err = strdup("a capability name is too long");
      return -1;
    }
    if (out->capabilities_len >= WD_MANIFEST_CAPS) {
      if (err != NULL) *err = strdup("too many capabilities");
      return -1;
    }
    memcpy(out->capabilities[out->capabilities_len], open, n);
    out->capabilities[out->capabilities_len][n] = '\0';
    out->capabilities_len++;
    at = close + 1;
  }
}

int wd_manifest_load(const char *dir, const char *name, wd_manifest *out,
                     char **err) {
  if (err != NULL) *err = NULL;
  if (dir == NULL || name == NULL || out == NULL) {
    if (err != NULL) *err = strdup("a directory and a name are needed");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->state_schema = 1;
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.toml", dir, name);
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (err != NULL) *err = strdup("the manifest is missing");
    return -1;
  }
  char line[1024];
  int failed = 0;
  while (!failed && fgets(line, sizeof(line), file) != NULL) {
    char *hash = strchr(line, '#');
    if (hash != NULL) *hash = '\0';
    char *text = trim(line);
    if (text[0] == '\0') continue;
    char *equals = strchr(text, '=');
    if (equals == NULL) continue;
    *equals = '\0';
    char *key = trim(text);
    char *value = trim(equals + 1);
    if (strcmp(key, "name") == 0) {
      if (unquote(value, out->name, sizeof(out->name)) != 0) {
        if (err != NULL) *err = strdup("the name is not a quoted string");
        failed = 1;
      }
    } else if (strcmp(key, "version") == 0) {
      if (unquote(value, out->version, sizeof(out->version)) != 0) {
        if (err != NULL) *err = strdup("the version is not a quoted string");
        failed = 1;
      }
    } else if (strcmp(key, "state_schema") == 0) {
      char *end = NULL;
      unsigned long schema = strtoul(value, &end, 10);
      if (end == value || schema == 0 || schema > 1000) {
        if (err != NULL) *err = strdup("the state schema is not a number");
        failed = 1;
      } else {
        out->state_schema = (uint32_t)schema;
      }
    } else if (strcmp(key, "lazy") == 0) {
      out->lazy = strcmp(value, "true") == 0;
    } else if (strcmp(key, "capabilities") == 0) {
      if (parse_list(value, out, err) != 0) failed = 1;
    }
  }
  fclose(file);
  if (failed) return -1;
  if (out->name[0] == '\0') {
    if (err != NULL) *err = strdup("the manifest has no name");
    return -1;
  }
  if (strcmp(out->name, name) != 0) {
    if (err != NULL) *err = strdup("the manifest name does not match the file");
    return -1;
  }
  if (out->version[0] == '\0')
    snprintf(out->version, sizeof(out->version), "1");
  return 0;
}

int wd_manifest_compare_version(const char *a, const char *b) {
  if (a == NULL || b == NULL) return 0;
  unsigned long va = strtoul(a, NULL, 10);
  unsigned long vb = strtoul(b, NULL, 10);
  if (va < vb) return -1;
  if (va > vb) return 1;
  return strcmp(a, b);
}
