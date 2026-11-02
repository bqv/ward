/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/presets.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const wd_preset presets[] = {
    {"default", "Ask writes",
     "Reads and grep allowed. Writes, edits and exec ask first. Observation off.",
     "allow", "ask", "ask", "ask", "allow", "off", false, NULL},
    {"reads-only", "Reads only",
     "Reads and grep allowed. Write, edit and exec are denied. Observation off.",
     "allow", "deny", "deny", "deny", "allow", "off", false, NULL},
    {"full-access", "Full access",
     "Writes, edits and exec run without asking. Every command the model "
     "chooses will run on this machine.",
     "allow", "allow", "allow", "allow", "allow", "off", true,
     "Writes, edits and exec run without asking. Every command the model "
     "chooses will run on this machine."},
    {"workspace", "Workspace",
     "Reads and grep allowed. Writes, edits and exec ask first. A file must be "
     "read before it is overwritten.",
     "allow", "ask", "ask", "ask", "allow", "require-read", false, NULL},
};

size_t wd_preset_count(void) { return sizeof(presets) / sizeof(presets[0]); }

const wd_preset *wd_preset_at(size_t index) {
  if (index >= wd_preset_count()) return NULL;
  return &presets[index];
}

const wd_preset *wd_preset_find(const char *id) {
  if (id == NULL) return NULL;
  for (size_t i = 0; i < wd_preset_count(); i++)
    if (strcmp(presets[i].id, id) == 0) return &presets[i];
  return NULL;
}

static int preset_path(const char *state_dir, char *out, size_t cap) {
  int written = snprintf(out, cap, "%s/policy.preset", state_dir);
  return written > 0 && (size_t)written < cap ? 0 : -1;
}

static bool confirmed_selection(const char *state_dir, const char *id);

const wd_preset *wd_preset_current(const char *state_dir) {
  char path[512];
  const wd_preset *fallback = wd_preset_find("default");
  if (state_dir == NULL || preset_path(state_dir, path, sizeof(path)) != 0)
    return fallback;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fallback;
  char id[64];
  ssize_t got = read(fd, id, sizeof(id) - 1);
  close(fd);
  if (got <= 0) return fallback;
  id[got] = '\0';
  size_t len = strlen(id);
  while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r'))
    id[--len] = '\0';
  const wd_preset *found = wd_preset_find(id);
  if (found != NULL && found->dangerous && !confirmed_selection(state_dir, id))
    return fallback;
  return found != NULL ? found : fallback;
}

static int confirmation_path(const char *state_dir, char *out, size_t cap) {
  int written = snprintf(out, cap, "%s/policy.confirmed", state_dir);
  return written > 0 && (size_t)written < cap ? 0 : -1;
}

static bool confirmed_selection(const char *state_dir, const char *id) {
  char path[512];
  if (state_dir == NULL || confirmation_path(state_dir, path, sizeof(path)) != 0)
    return false;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char held[64];
  ssize_t got = read(fd, held, sizeof(held) - 1);
  close(fd);
  if (got <= 0) return false;
  held[got] = '\0';
  size_t len = strlen(held);
  while (len > 0 && (held[len - 1] == '\n' || held[len - 1] == '\r'))
    held[--len] = '\0';
  return strcmp(held, id) == 0;
}

static int write_selection(const char *state_dir, const char *id, char **err) {
  char path[512];
  if (state_dir == NULL || preset_path(state_dir, path, sizeof(path)) != 0) {
    if (err != NULL) *err = strdup("policy: no state directory");
    return -1;
  }
  char temp[560];
  snprintf(temp, sizeof(temp), "%s.new", path);
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (err != NULL) *err = strdup("policy: cannot write the selection");
    return -1;
  }
  size_t len = strlen(id);
  ssize_t written = write(fd, id, len);
  close(fd);
  if (written != (ssize_t)len) {
    unlink(temp);
    if (err != NULL) *err = strdup("policy: cannot write the selection");
    return -1;
  }
  if (rename(temp, path) != 0) {
    unlink(temp);
    if (err != NULL) *err = strdup("policy: cannot store the selection");
    return -1;
  }
  return 0;
}

int wd_preset_select(const char *state_dir, const char *id, bool confirmed,
                     char **err) {
  const wd_preset *chosen = wd_preset_find(id);
  if (chosen == NULL) {
    if (err != NULL) *err = strdup("policy: no such preset");
    return -1;
  }
  if (chosen->dangerous && !confirmed) {
    if (err != NULL) *err = strdup(chosen->warning);
    return -1;
  }
  if (chosen->dangerous) {
    char path[512];
    if (state_dir == NULL || confirmation_path(state_dir, path, sizeof(path)) != 0) {
      if (err != NULL) *err = strdup("policy: no state directory");
      return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (err != NULL) *err = strdup("policy: cannot record the confirmation");
      return -1;
    }
    ssize_t written = write(fd, id, strlen(id));
    close(fd);
    if (written != (ssize_t)strlen(id)) {
      if (err != NULL) *err = strdup("policy: cannot record the confirmation");
      return -1;
    }
  } else if (state_dir != NULL) {
    char path[512];
    if (confirmation_path(state_dir, path, sizeof(path)) == 0) unlink(path);
  }
  return write_selection(state_dir, id, err);
}


static const char *axis_rule(const wd_preset *preset, const char *tool) {
  if (preset == NULL || tool == NULL) return NULL;
  if (strcmp(tool, "read") == 0) return preset->read;
  if (strcmp(tool, "write") == 0) return preset->write;
  if (strcmp(tool, "edit") == 0) return preset->edit;
  if (strcmp(tool, "exec") == 0) return preset->exec;
  if (strcmp(tool, "grep") == 0) return preset->grep;
  return NULL;
}

static int rank(const char *decision) {
  if (decision == NULL) return 2;
  if (strcmp(decision, "allow") == 0) return 0;
  if (strcmp(decision, "ask") == 0) return 1;
  return 2;
}

static const char *name_of(int level) {
  return level == 0 ? "allow" : (level == 1 ? "ask" : "deny");
}

const char *wd_preset_clamp(const wd_preset *preset, const char *tool,
                            const char *decision) {
  const char *rule = axis_rule(preset, tool);
  if (rule == NULL) return decision;
  int wanted = rank(rule);
  int had = rank(decision);
  return name_of(wanted > had ? wanted : had);
}
