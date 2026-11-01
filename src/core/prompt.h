/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_PROMPT_H
#define WARD_CORE_PROMPT_H

#include <cbor.h>
#include <stddef.h>

typedef struct wd_prompt wd_prompt;

wd_prompt *wd_prompt_build(const char *state_dir, const char *root,
                           const char *context, const char *session_system);
void wd_prompt_free(wd_prompt *prompt);
size_t wd_prompt_len(const wd_prompt *prompt);
cbor_item_t *wd_prompt_to_cbor(const wd_prompt *prompt);
cbor_item_t *wd_prompt_to_history(const wd_prompt *prompt,
                                 cbor_item_t *history);

#endif
