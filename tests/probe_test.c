/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>

#include "core/probe.h"

int main(void) {
  assert(wd_context_from_text("32768") == 32768);
  assert(wd_context_from_text("32k") == 32768);
  assert(wd_context_from_text("32K") == 32768);
  assert(wd_context_from_text("") == 0);
  assert(wd_context_from_text("lots") == 0);
  assert(wd_context_from_text("-5") == 0);
  assert(wd_context_from_text("999999999999") == 0);

  static const char props[] =
      "{\"default_generation_settings\":{\"n_ctx\":65536,\"temperature\":0.8},"
      "\"total_slots\":4,\"model_path\":\"/models/x.gguf\"}";
  int n_ctx = 0;
  int slots = 0;
  assert(wd_probe_scan(props, &n_ctx, &slots) == 0);
  assert(n_ctx == 65536);
  assert(slots == 4);

  assert(wd_probe_scan("{}", &n_ctx, &slots) == -1);
  assert(wd_probe_scan("{\"n_ctx\": 8192}", &n_ctx, NULL) == 0);
  assert(n_ctx == 8192);

  static const char models[] =
      "{\"data\":[{\"id\":\"Kev-0.8B\",\"status\":{\"args\":[\"--port\",\"59251\"]}},"
      "{\"id\":\"qwen1_5-1_8b-chat-q4_k_m\",\"status\":{\"value\":\"loaded\","
      "\"args\":[\"--ctx-size\",\"16384\",\"--parallel\",\"2\",\"--port\",\"41001\"]}}]}";
  n_ctx = 0;
  slots = 0;
  assert(wd_probe_router_models(models, "qwen1_5-1_8b-chat-q4_k_m", &n_ctx,
                                &slots) == 0);
  assert(n_ctx == 16384);
  assert(slots == 2);
  assert(wd_probe_router_models(models, "absent", &n_ctx, &slots) == -1);

  assert(wd_context_budget(8192, 1024) == (size_t)(8192 - 1024 - 256) * 4);
  assert(wd_context_budget(65536, 1024) == (size_t)(65536 - 1024 - 256) * 4);
  assert(wd_context_budget(2048, 1024) == 4096);
  assert(wd_context_budget(0, 0) == 4096);
  assert(wd_context_budget(1 << 22, 1024) == (size_t)(1 << 20));

  printf("probe tests passed\n");
  return 0;
}
