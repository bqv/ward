/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net/estate.h"

static const char busy[] =
    "{\"group\":\"gpu\",\"holders\":[\"sd\"],\"since_ms\":1793711314557,"
    "\"slots\":[{\"name\":\"llama\",\"state\":\"stopped\"},"
    "{\"name\":\"sd\",\"state\":\"running\"}]}";
static const char idle[] =
    "{\"group\":\"gpu\",\"holders\":[],\"since_ms\":0,"
    "\"slots\":[{\"name\":\"sd\",\"state\":\"loading\"}]}";

static void check_parse(void) {
  wd_estate_card card;
  assert(wd_estate_parse(busy, &card) == 0);
  assert(strcmp(card.group, "gpu") == 0);
  assert(card.holders_len == 1 && strcmp(card.holders[0], "sd") == 0);
  assert(card.since_ms == 1793711314557ull);
  assert(card.slots_len == 2 && strcmp(card.slots[1].state, "running") == 0);

  assert(wd_estate_parse(idle, &card) == 0);
  assert(card.holders_len == 0);
  assert(card.since_ms == 0);
  assert(card.slots_len == 1 && strcmp(card.slots[0].state, "loading") == 0);
}

int main(void) {
  alarm(10);
  check_parse();
  printf("estate parse tests passed\n");
  return 0;
}
