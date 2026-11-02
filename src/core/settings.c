/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/settings.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int path_for(const char *state_dir, char *out, size_t cap) {
  int written = snprintf(out, cap, "%s/settings.txt", state_dir);
  return written > 0 && (size_t)written < cap ? 0 : -1;
}

void wd_settings_defaults(wd_settings *settings) {
  settings->max_runs = 8;
  settings->debug = false;
  settings->disabled_len = 0;
}

bool wd_settings_module_disabled(const wd_settings *settings, const char *name) {
  if (settings == NULL || name == NULL) return false;
  for (size_t i = 0; i < settings->disabled_len; i++)
    if (strcmp(settings->disabled[i], name) == 0) return true;
  return false;
}

int wd_settings_set_module(wd_settings *settings, const char *name, bool enabled,
                           char **err) {
  size_t found = settings->disabled_len;
  for (size_t i = 0; i < settings->disabled_len; i++)
    if (strcmp(settings->disabled[i], name) == 0) {
      found = i;
      break;
    }
  if (enabled) {
    if (found == settings->disabled_len) return 0;
    for (size_t i = found + 1; i < settings->disabled_len; i++)
      memcpy(settings->disabled[i - 1], settings->disabled[i],
             WD_SETTINGS_NAME_MAX);
    settings->disabled_len--;
    return 0;
  }
  if (found != settings->disabled_len) return 0;
  if (settings->disabled_len >= WD_SETTINGS_DISABLED_MAX) {
    if (err != NULL) *err = strdup("settings: too many disabled modules");
    return -1;
  }
  snprintf(settings->disabled[settings->disabled_len], WD_SETTINGS_NAME_MAX,
           "%s", name);
  settings->disabled_len++;
  return 0;
}

static void trim(char *text) {
  size_t len = strlen(text);
  while (len > 0 && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
}

int wd_settings_load(const char *state_dir, wd_settings *settings) {
  wd_settings_defaults(settings);
  char path[512];
  if (state_dir == NULL || path_for(state_dir, path, sizeof(path)) != 0)
    return 0;
  FILE *file = fopen(path, "r");
  if (file == NULL) return 0;
  char line[256];
  while (fgets(line, sizeof(line), file) != NULL) {
    trim(line);
    char *eq = strchr(line, '=');
    if (eq == NULL) continue;
    *eq = '\0';
    char *key = line;
    char *value = eq + 1;
    while (*key != '\0' && isspace((unsigned char)*key)) key++;
    trim(key);
    while (*value != '\0' && isspace((unsigned char)*value)) value++;
    if (strncmp(key, "disabled.", 9) == 0) {
      if (strcmp(value, "true") == 0) {
        char *err = NULL;
        wd_settings_set_module(settings, key + 9, false, &err);
        free(err);
      }
      continue;
    }
    char *err = NULL;
    wd_settings_apply(settings, key, value, &err);
    free(err);
  }
  fclose(file);
  return 0;
}

int wd_settings_apply(wd_settings *settings, const char *key, const char *value,
                      char **err) {
  if (key == NULL || value == NULL) {
    if (err != NULL) *err = strdup("settings: a key and a value are required");
    return -1;
  }
  if (strcmp(key, "max_runs") == 0) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 64) {
      if (err != NULL)
        *err = strdup("settings: max_runs is a number between 1 and 64");
      return -1;
    }
    settings->max_runs = parsed;
    return 0;
  }
  if (strcmp(key, "debug") == 0) {
    if (strcmp(value, "true") != 0 && strcmp(value, "false") != 0) {
      if (err != NULL) *err = strdup("settings: debug is true or false");
      return -1;
    }
    settings->debug = strcmp(value, "true") == 0;
    return 0;
  }
  if (err != NULL) *err = strdup("settings: no such setting");
  return -1;
}

int wd_settings_save(const char *state_dir, const wd_settings *settings) {
  char path[512];
  if (state_dir == NULL || path_for(state_dir, path, sizeof(path)) != 0)
    return -1;
  char temp[560];
  snprintf(temp, sizeof(temp), "%s.new", path);
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  char text[128];
  int wrote = snprintf(text, sizeof(text), "max_runs = %ld\ndebug = %s\n",
                       settings->max_runs, settings->debug ? "true" : "false");
  ssize_t put = write(fd, text, (size_t)wrote);
  for (size_t i = 0; i < settings->disabled_len; i++) {
    char line[64];
    int made = snprintf(line, sizeof(line), "disabled.%s = true\n",
                        settings->disabled[i]);
    if (made > 0) put = write(fd, line, (size_t)made);
  }
  close(fd);
  if (put != (ssize_t)wrote || rename(temp, path) != 0) {
    unlink(temp);
    return -1;
  }
  return 0;
}
