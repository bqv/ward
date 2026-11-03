/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_SEARCH_H
#define WARD_CORE_SEARCH_H

#include <stddef.h>
#include <stdint.h>

typedef struct wd_search wd_search;

typedef struct {
  char session[40];
  uint64_t seq;
  char role[16];
  char ts[32];
  char *text;
  char *snippet;
  double score;
} wd_search_hit;

wd_search *wd_search_open(const char *dir, char **err);
void wd_search_free(wd_search *search);

int wd_search_index(wd_search *search, const char *session, uint64_t seq,
                    const char *role, const char *ts, const char *text);

size_t wd_search_query(wd_search *search, const char *query,
                       const char *session, size_t limit, uint64_t before,
                       wd_search_hit *hits, size_t cap, uint64_t *next,
                       uint64_t *total);

int wd_search_rows(wd_search *search, size_t *out);

void wd_search_hits_free(wd_search_hit *hits, size_t count);

#endif
