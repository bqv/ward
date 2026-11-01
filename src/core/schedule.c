/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/schedule.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"
#include "proto/codec.h"

#define WD_SCHEDULE_LIMITS                                                     \
  (wd_limits) { .fuel = 400000000, .max_memory = 1 << 24, .max_instances = 1 }

struct wd_scheduler {
  wd_module *module;
  wd_instance *instance;
};

static uint8_t *read_module(const char *dir, const char *name, size_t *len) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

wd_scheduler *wd_scheduler_new(wd_engine *engine, const char *module_dir,
                               char **err) {
  size_t len = 0;
  uint8_t *bytes = read_module(module_dir, "schedule.wasm", &len);
  if (bytes == NULL) {
    *err = strdup("schedule.wasm is missing");
    return NULL;
  }
  wd_scheduler *scheduler = calloc(1, sizeof(*scheduler));
  if (scheduler == NULL) {
    free(bytes);
    *err = strdup("out of memory");
    return NULL;
  }
  static const char *const no_caps[] = {NULL};
  wd_module_spec spec = {"schedule", no_caps, 0, true};
  scheduler->module = wd_module_load(engine, &spec, bytes, len, err);
  free(bytes);
  if (scheduler->module == NULL) {
    free(scheduler);
    return NULL;
  }
  wd_limits limits = WD_SCHEDULE_LIMITS;
  scheduler->instance =
      wd_instance_new(scheduler->module, no_caps, 0, &limits, NULL, NULL, err);
  if (scheduler->instance == NULL) {
    wd_module_free(scheduler->module);
    free(scheduler);
    return NULL;
  }
  return scheduler;
}

void wd_scheduler_free(wd_scheduler *scheduler) {
  if (scheduler == NULL) return;
  wd_instance_free(scheduler->instance);
  wd_module_free(scheduler->module);
  free(scheduler);
}

int wd_scheduler_next(wd_scheduler *scheduler, const char *cron, uint64_t from_ms,
                      uint64_t *next_ms, char **err) {
  cbor_item_t *request = wd_map(2);
  wd_put(request, "cron", wd_text(cron));
  char from[32];
  snprintf(from, sizeof(from), "%llu", (unsigned long long)from_ms);
  wd_put(request, "from_ms", wd_text(from));
  size_t len = 0;
  uint8_t *body = wd_encode(request, &len);
  if (body == NULL) {
    *err = strdup("cannot encode the schedule request");
    return -1;
  }
  uint8_t *out = NULL;
  size_t out_len = 0;
  int rc = wd_instance_call(scheduler->instance, body, len, &out, &out_len, err);
  free(body);
  if (rc != 0) return -1;
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  if (reply == NULL) {
    *err = strdup("the schedule guest answered nothing");
    return -1;
  }
  char *value = wd_text_copy(wd_get(reply, "next_ms"));
  wd_free(reply);
  if (value == NULL) {
    *err = strdup("the schedule guest answered no time");
    return -1;
  }
  *next_ms = strtoull(value, NULL, 10);
  free(value);
  return 0;
}
