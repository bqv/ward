/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/turn.h"
#include "fake_endpoint.h"
#include "proto/codec.h"

typedef struct {
  int deltas;
  int ended;
  char text[512];
} events;

static int emit(void *ctx, const char *event, cbor_item_t *payload, char **err) {
  events *seen = ctx;
  (void)err;
  if (strcmp(event, "text.delta") == 0) {
    char *text = wd_text_copy(wd_get(payload, "text"));
    assert(text != NULL);
    snprintf(seen->text, sizeof(seen->text), "%s", text);
    free(text);
    seen->deltas++;
    return 0;
  }
  if (strcmp(event, "turn.end") == 0) {
    seen->ended++;
    return 0;
  }
  assert(0);
  return -1;
}

int main(void) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  int port = 0;
  pid_t server = 0;
  fake_endpoint(&port, &server);
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  char *err = NULL;
  wd_turn *turn = wd_turn_new(engine, dir, ".", &err);
  if (turn == NULL) {
    fprintf(stderr, "wd_turn_new: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  events seen = {0, 0, {0}};
  int rc = wd_turn_prompt(turn, "hello", emit, &seen, &err);
  if (rc != 0) {
    fprintf(stderr, "wd_turn_prompt: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  assert(seen.deltas == 1);
  assert(strstr(seen.text, "hello") != NULL);
  assert(seen.ended == 1);

  events second = {0, 0, {0}};
  rc = wd_turn_prompt(turn, "again", emit, &second, &err);
  if (rc != 0) {
    fprintf(stderr, "wd_turn_prompt second: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  assert(second.deltas == 1);
  assert(strstr(second.text, "again") != NULL);
  assert(second.ended == 1);

  wd_turn_free(turn);
  wd_engine_free(engine);
  stop_server(server);
  printf("turn tests passed\n");
  return 0;
}
