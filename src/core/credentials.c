/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/credentials.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CREDENTIAL_MAX 32
#define CREDENTIAL_NAME 64
#define CREDENTIAL_VALUE 256

typedef struct {
  char name[CREDENTIAL_NAME];
  char value[CREDENTIAL_VALUE];
} entry;

struct wd_credentials {
  entry items[CREDENTIAL_MAX];
  size_t len;
};

char *wd_credentials_path(const char *state_dir) {
  if (state_dir == NULL) return NULL;
  size_t len = strlen(state_dir) + 16;
  char *path = malloc(len);
  if (path == NULL) return NULL;
  snprintf(path, len, "%s/credentials", state_dir);
  return path;
}

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t') text++;
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                     text[len - 1] == ' ' || text[len - 1] == '\t'))
    text[--len] = '\0';
  if (len >= 2 && (text[0] == '"' || text[0] == '\'')) {
    size_t end = len - 1;
    if (text[end] == text[0]) {
      memmove(text, text + 1, end - 1);
      text[end - 1] = '\0';
    }
  }
  return text;
}

wd_credentials *wd_credentials_load(const char *path, char **err) {
  wd_credentials *credentials = calloc(1, sizeof(*credentials));
  if (credentials == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  if (path == NULL) return credentials;
  struct stat st;
  if (stat(path, &st) != 0) return credentials;
  FILE *f = fopen(path, "r");
  if (f == NULL) return credentials;
  char line[CREDENTIAL_VALUE + CREDENTIAL_NAME + 8];
  while (fgets(line, sizeof(line), f) != NULL && credentials->len < CREDENTIAL_MAX) {
    char *text = trim(line);
    if (text[0] == '\0' || text[0] == '#') continue;
    char *eq = strchr(text, '=');
    if (eq == NULL) continue;
    *eq = '\0';
    char *name = trim(text);
    char *value = trim(eq + 1);
    if (name[0] == '\0' || value[0] == '\0') continue;
    entry *slot = &credentials->items[credentials->len++];
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    snprintf(slot->value, sizeof(slot->value), "%s", value);
  }
  fclose(f);
  return credentials;
}

void wd_credentials_free(wd_credentials *credentials) { free(credentials); }

const char *wd_credentials_find(const wd_credentials *credentials,
                                const char *name) {
  if (credentials == NULL || name == NULL) return NULL;
  for (size_t i = 0; i < credentials->len; i++)
    if (strcmp(credentials->items[i].name, name) == 0)
      return credentials->items[i].value;
  return NULL;
}
