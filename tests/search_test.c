/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/search.h"

int main(void) {
  char dir[] = "/tmp/ward-search-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char *err = NULL;
  wd_search *search = wd_search_open(dir, &err);
  assert(search != NULL);

  assert(wd_search_index(search, "s1", 1, "user", "t1", "deploy the widget") == 0);
  assert(wd_search_index(search, "s1", 2, "assistant", "t2", "the widget is deployed") == 0);
  assert(wd_search_index(search, "s1", 3, "assistant", "t3", "nothing to see") == 0);
  assert(wd_search_index(search, "s2", 4, "user", "t4", "widget settings") == 0);

  wd_search_hit hits[8];
  uint64_t next = 0;
  uint64_t total = 0;
  size_t found = wd_search_query(search, "widget", NULL, 8, 0, hits, 8, &next, &total);
  assert(found == 3);
  assert(total == 3);
  assert(strcmp(hits[0].session, "s2") == 0);
  assert(hits[0].seq == 4);
  assert(strcmp(hits[0].role, "user") == 0);
  assert(hits[0].snippet != NULL && strstr(hits[0].snippet, "widget") != NULL);
  wd_search_hits_free(hits, found);

  found = wd_search_query(search, "widget", NULL, 2, 0, hits, 8, &next, &total);
  assert(found == 2);
  assert(total == 3);
  uint64_t page1_last = hits[1].seq;
  wd_search_hits_free(hits, found);

  found = wd_search_query(search, "widget", NULL, 2, page1_last, hits, 8, &next, &total);
  assert(found == 1);
  assert(hits[0].seq < page1_last);
  assert(total >= found);
  wd_search_hits_free(hits, found);

  found = wd_search_query(search, "widget", "s1", 8, 0, hits, 8, &next, &total);
  assert(found == 2 && total == 2);
  wd_search_hits_free(hits, found);

  found = wd_search_query(search, "absent", NULL, 8, 0, hits, 8, &next, &total);
  assert(found == 0 && total == 0);

  found = wd_search_query(search, "widget", NULL, 500, 0, hits, 8, &next, &total);
  assert(found <= 8);
  wd_search_hits_free(hits, found);

  wd_search_free(search);
  printf("search tests passed\n");
  return 0;
}
