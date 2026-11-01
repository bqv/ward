/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/turn.h"
#include "fake_endpoint.h"
#include "proto/codec.h"

typedef struct {
  int deltas;
  char text[512];
} seen;

static int emit(void *ctx, const char *event, cbor_item_t *payload, char **err) {
  seen *s = ctx;
  (void)err;
  if (strcmp(event, "text.delta") == 0) {
    char *piece = wd_text_copy(wd_get(payload, "text"));
    assert(piece != NULL);
    size_t have = strlen(s->text);
    snprintf(s->text + have, sizeof(s->text) - have, "%s", piece);
    free(piece);
    s->deltas++;
  }
  return 0;
}

static int run_turn(wd_turn *turn, const char *text, wd_turn_emit emit,
                   void *ctx, char **err) {
  int rc = wd_turn_prompt(turn, text, emit, ctx, err);
  while (rc == WD_TURN_PENDING) {
    int kind = wd_turn_pending_kind(turn);
    if (kind == 1)
      rc = wd_turn_resume(turn, true, emit, ctx, err);
    else if (kind == 2)
      rc = wd_turn_resume_tool(turn, emit, ctx, err);
    else
      rc = wd_turn_resume_http(turn, emit, ctx, err);
  }
  return rc;
}

int main(void) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  int port = 0;
  pid_t server = 0;
  fake_endpoint(&port, &server);

  wd_engine *engine = wd_engine_new(getenv("WARD_STATE"));
  assert(engine != NULL);
  char *err = NULL;
  char root[] = "/tmp/ward-turn-XXXXXX";
  assert(mkdtemp(root) != NULL);
  wd_turn *turn = wd_turn_new(engine, dir, root, &err);
  if (turn == NULL) {
    fprintf(stderr, "ward turn: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  seen s = {0, {0}};
  int rc = run_turn(turn, "hello m1", emit, &s, &err);
  if (rc != 0) {
    fprintf(stderr, "prompt: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  assert(s.deltas == 1);
  if (strstr(s.text, "hello m1") == NULL)
    fprintf(stderr, "gate: text=%s\n", s.text);
  assert(strstr(s.text, "hello m1") != NULL);

  wd_turn_free(turn);
  wd_engine_free(engine);
  stop_server(server);
  printf("m1 gate passed: %s\n", s.text);
  return 0;
}
