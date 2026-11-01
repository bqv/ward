/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_PROBE_H
#define WARD_CORE_PROBE_H

#include <stddef.h>

size_t wd_context_budget(int n_ctx, int max_output_tokens);
int wd_context_from_text(const char *text);
int wd_probe_scan(const char *json, int *n_ctx, int *slots);
int wd_probe_router_models(const char *json, const char *want, int *n_ctx,
                           int *slots);

#endif
