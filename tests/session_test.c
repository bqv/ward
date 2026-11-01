/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/session.h"
#include "proto/codec.h"

typedef struct {
  int count;
  uint64_t seqs[8];
  char kinds[8][32];
} seen;

static int note(void *ctx, uint64_t seq, cbor_item_t *event) {
  seen *s = ctx;
  if (s->count < 8) {
    s->seqs[s->count] = seq;
    char *kind = wd_text_copy(wd_get(event, "kind"));
    snprintf(s->kinds[s->count], sizeof(s->kinds[0]), "%s", kind ? kind : "?");
    free(kind);
  }
  s->count++;
  return 0;
}

int main(void) {
  char dir[] = "/tmp/ward-session-XXXXXX";
  assert(mkdtemp(dir) != NULL);

  hs_store store;
  assert(hs_open(&store, dir) == 0);
  assert(store.len == 0);

  hs_session *first = NULL;
  hs_session *second = NULL;
  assert(hs_create(&store, "/tmp", "deepseek", &first) == 0);
  usleep(2000);
  assert(hs_create(&store, "/tmp", NULL, &second) == 0);
  assert(store.len == 2);
  assert(strcmp(store.items[0]->id, second->id) == 0);
  assert(strcmp(second->model, "unset") == 0);
  assert(hs_find(&store, first->id) == first);
  assert(hs_find(&store, "nope") == NULL);

  uint64_t seq = 0;
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "session", hs_to_cbor(first));
  assert(hs_emit(first, "session.updated", payload, &seq) == 0);
  assert(seq == 1);
  assert(first->last_seq == 1);

  seen s = {0};
  assert(hs_replay(first, 0, note, &s) == 1);
  assert(s.count == 1);
  assert(s.seqs[0] == 1);
  assert(strcmp(s.kinds[0], "session.updated") == 0);

  seen from_one = {0};
  assert(hs_replay(first, 1, note, &from_one) == 0);
  assert(from_one.count == 0);

  char first_id[WD_ID_LEN];
  snprintf(first_id, sizeof(first_id), "%s", first->id);
  hs_close(&store);

  assert(hs_open(&store, dir) == 0);
  assert(store.len == 2);
  hs_session *reloaded = hs_find(&store, first_id);
  assert(reloaded != NULL);
  assert(reloaded->last_seq == 1);
  assert(strcmp(reloaded->cwd, "/tmp") == 0);
  assert(strcmp(reloaded->model, "deepseek") == 0);
  uint64_t checkpoint = 0;
  assert(hs_checkpoint(reloaded, "before the branch", &checkpoint) == 0);
  assert(checkpoint == 2);
  uint64_t last = 0;
  assert(hs_last_checkpoint(reloaded, &last) == 0);
  assert(last == checkpoint);
  assert(hs_checkpoint(reloaded, "", &checkpoint) == 0);
  assert(checkpoint == 3);
  assert(hs_last_checkpoint(reloaded, &last) == 0);
  assert(last == 3);

  hs_session *child = NULL;
  assert(hs_fork(&store, reloaded, 0, &child) == 0);
  assert(child != NULL);
  assert(strcmp(child->id, reloaded->id) != 0);
  assert(strcmp(child->cwd, reloaded->cwd) == 0);
  assert(strcmp(child->model, reloaded->model) == 0);
  assert(child->last_seq == 4);
  seen fork_seen = {0};
  assert(hs_replay(child, 0, note, &fork_seen) == 4);
  assert(strcmp(fork_seen.kinds[0], "session.updated") == 0);
  assert(strcmp(fork_seen.kinds[1], "session.checkpoint") == 0);
  assert(strcmp(fork_seen.kinds[2], "session.checkpoint") == 0);
  assert(strcmp(fork_seen.kinds[3], "session.forked") == 0);

  hs_session *cut = NULL;
  assert(hs_fork(&store, reloaded, 1, &cut) == 0);
  assert(cut->last_seq == 2);
  hs_session *hist = NULL;
  assert(hs_create(&store, "/tmp", NULL, &hist) == 0);
  cbor_item_t *asked = wd_map(1);
  wd_put(asked, "text", wd_text("first question"));
  assert(hs_emit(hist, "session.prompted", asked, NULL) == 0);
  cbor_item_t *delta = wd_map(1);
  wd_put(delta, "text", wd_text("first answer"));
  assert(hs_emit(hist, "text.delta", delta, NULL) == 0);
  assert(hs_emit(hist, "turn.end", wd_map(0), NULL) == 0);
  cbor_item_t *asked2 = wd_map(1);
  wd_put(asked2, "text", wd_text("second question"));
  assert(hs_emit(hist, "session.prompted", asked2, NULL) == 0);
  cbor_item_t *delta2 = wd_map(1);
  wd_put(delta2, "text", wd_text("second answer"));
  assert(hs_emit(hist, "text.delta", delta2, NULL) == 0);
  assert(hs_emit(hist, "turn.end", wd_map(0), NULL) == 0);

  cbor_item_t *built = NULL;
  assert(hs_history(hist, 0, &built) == 0);
  assert(cbor_array_size(built) == 4);
  cbor_item_t **turns = cbor_array_handle(built);
  char *role = wd_text_copy(wd_get(turns[0], "role"));
  char *text = wd_text_copy(wd_get(turns[0], "text"));
  assert(role != NULL && strcmp(role, "user") == 0);
  assert(text != NULL && strcmp(text, "first question") == 0);
  free(role);
  free(text);
  role = wd_text_copy(wd_get(turns[3], "role"));
  text = wd_text_copy(wd_get(turns[3], "text"));
  assert(role != NULL && strcmp(role, "assistant") == 0);
  assert(text != NULL && strcmp(text, "second answer") == 0);
  free(role);
  free(text);
  wd_free(built);

  assert(hs_history(hist, 30, &built) == 0);
  assert(cbor_array_size(built) == 2);
  wd_free(built);

  cbor_item_t *compacted = wd_map(2);
  wd_put(compacted, "through", wd_uint(hist->last_seq));
  wd_put(compacted, "summary", wd_text("they talked twice"));
  assert(hs_emit(hist, "session.compacted", compacted, NULL) == 0);
  assert(hs_history(hist, 0, &built) == 0);
  assert(cbor_array_size(built) == 1);
  turns = cbor_array_handle(built);
  role = wd_text_copy(wd_get(turns[0], "role"));
  text = wd_text_copy(wd_get(turns[0], "text"));
  assert(role != NULL && strcmp(role, "system") == 0);
  assert(text != NULL && strcmp(text, "they talked twice") == 0);
  free(role);
  free(text);
  wd_free(built);

  hs_close(&store);

  printf("session tests passed\n");
  return 0;
}
