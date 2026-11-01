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

  wd_store store;
  assert(wd_store_open(&store, dir) == 0);
  assert(store.len == 0);

  wd_session *first = NULL;
  wd_session *second = NULL;
  assert(wd_session_create(&store, "/tmp", "deepseek", &first) == 0);
  usleep(2000);
  assert(wd_session_create(&store, "/tmp", NULL, &second) == 0);
  assert(store.len == 2);
  assert(strcmp(store.items[0]->id, second->id) == 0);
  assert(strcmp(second->model, "unset") == 0);
  assert(wd_session_find(&store, first->id) == first);
  assert(wd_session_find(&store, "nope") == NULL);

  uint64_t seq = 0;
  cbor_item_t *payload = wd_map(1);
  wd_put(payload, "session", wd_session_to_cbor(first));
  assert(wd_session_emit(first, "session.updated", payload, &seq) == 0);
  assert(seq == 1);
  assert(first->last_seq == 1);

  seen s = {0};
  assert(wd_session_replay(first, 0, note, &s) == 1);
  assert(s.count == 1);
  assert(s.seqs[0] == 1);
  assert(strcmp(s.kinds[0], "session.updated") == 0);

  seen from_one = {0};
  assert(wd_session_replay(first, 1, note, &from_one) == 0);
  assert(from_one.count == 0);

  char first_id[WD_ID_LEN];
  snprintf(first_id, sizeof(first_id), "%s", first->id);
  wd_store_close(&store);

  assert(wd_store_open(&store, dir) == 0);
  assert(store.len == 2);
  wd_session *reloaded = wd_session_find(&store, first_id);
  assert(reloaded != NULL);
  assert(reloaded->last_seq == 1);
  assert(strcmp(reloaded->cwd, "/tmp") == 0);
  assert(strcmp(reloaded->model, "deepseek") == 0);
  uint64_t checkpoint = 0;
  assert(wd_session_checkpoint(reloaded, "before the branch", &checkpoint) == 0);
  assert(checkpoint == 2);
  uint64_t last = 0;
  assert(wd_session_last_checkpoint(reloaded, &last) == 0);
  assert(last == checkpoint);
  assert(wd_session_checkpoint(reloaded, "", &checkpoint) == 0);
  assert(checkpoint == 3);
  assert(wd_session_last_checkpoint(reloaded, &last) == 0);
  assert(last == 3);

  wd_session *child = NULL;
  assert(wd_session_fork(&store, reloaded, 0, &child) == 0);
  assert(child != NULL);
  assert(strcmp(child->id, reloaded->id) != 0);
  assert(strcmp(child->cwd, reloaded->cwd) == 0);
  assert(strcmp(child->model, reloaded->model) == 0);
  assert(child->last_seq == 4);
  seen fork_seen = {0};
  assert(wd_session_replay(child, 0, note, &fork_seen) == 4);
  assert(strcmp(fork_seen.kinds[0], "session.updated") == 0);
  assert(strcmp(fork_seen.kinds[1], "session.checkpoint") == 0);
  assert(strcmp(fork_seen.kinds[2], "session.checkpoint") == 0);
  assert(strcmp(fork_seen.kinds[3], "session.forked") == 0);

  wd_session *cut = NULL;
  assert(wd_session_fork(&store, reloaded, 1, &cut) == 0);
  assert(cut->last_seq == 2);
  wd_session *hist = NULL;
  assert(wd_session_create(&store, "/tmp", NULL, &hist) == 0);
  cbor_item_t *asked = wd_map(1);
  wd_put(asked, "text", wd_text("first question"));
  assert(wd_session_emit(hist, "session.prompted", asked, NULL) == 0);
  cbor_item_t *delta = wd_map(1);
  wd_put(delta, "text", wd_text("first answer"));
  assert(wd_session_emit(hist, "text.delta", delta, NULL) == 0);
  assert(wd_session_emit(hist, "turn.end", wd_map(0), NULL) == 0);
  cbor_item_t *asked2 = wd_map(1);
  wd_put(asked2, "text", wd_text("second question"));
  assert(wd_session_emit(hist, "session.prompted", asked2, NULL) == 0);
  cbor_item_t *delta2 = wd_map(1);
  wd_put(delta2, "text", wd_text("second answer"));
  assert(wd_session_emit(hist, "text.delta", delta2, NULL) == 0);
  assert(wd_session_emit(hist, "turn.end", wd_map(0), NULL) == 0);

  cbor_item_t *built = NULL;
  assert(wd_session_history(hist, 0, &built) == 0);
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

  assert(wd_session_history(hist, 30, &built) == 0);
  assert(cbor_array_size(built) == 4);
  wd_free(built);

  cbor_item_t *compacted = wd_map(2);
  wd_put(compacted, "through", wd_uint(hist->last_seq));
  wd_put(compacted, "summary", wd_text("they talked twice"));
  assert(wd_session_emit(hist, "session.compacted", compacted, NULL) == 0);
  assert(wd_session_history(hist, 0, &built) == 0);
  assert(cbor_array_size(built) == 1);
  turns = cbor_array_handle(built);
  role = wd_text_copy(wd_get(turns[0], "role"));
  text = wd_text_copy(wd_get(turns[0], "text"));
  assert(role != NULL && strcmp(role, "system") == 0);
  assert(text != NULL && strcmp(text, "they talked twice") == 0);
  free(role);
  free(text);
  wd_free(built);

  wd_session *pinned = NULL;
  assert(wd_session_create(&store, "/tmp", NULL, &pinned) == 0);
  wd_pin pins[2];
  snprintf(pins[0].name, sizeof(pins[0].name), "driver");
  snprintf(pins[0].version, sizeof(pins[0].version), "3");
  snprintf(pins[1].name, sizeof(pins[1].name), "provider");
  snprintf(pins[1].version, sizeof(pins[1].version), "1");
  uint64_t pins_seq = 0;
  assert(wd_session_set_pins(pinned, pins, 2, &pins_seq) == 0);
  assert(pins_seq > 0);
  uint64_t upgrade_seq = 0;
  assert(wd_session_upgrade(pinned, "driver", "4", &upgrade_seq) == 0);
  assert(upgrade_seq > pins_seq);
  assert(strcmp(pinned->pins[0].version, "4") == 0);
  uint64_t same_seq = 0;
  assert(wd_session_upgrade(pinned, "driver", "4", &same_seq) == 0);
  assert(same_seq == 0);
  char pinned_id[WD_ID_LEN];
  snprintf(pinned_id, sizeof(pinned_id), "%s", pinned->id);

  wd_store_close(&store);
  assert(wd_store_open(&store, dir) == 0);
  wd_session *found = wd_session_find(&store, pinned_id);
  assert(found != NULL);
  assert(found->pins_len == 2);
  assert(strcmp(found->pins[0].name, "driver") == 0);
  assert(strcmp(found->pins[0].version, "4") == 0);
  assert(strcmp(found->pins[1].name, "provider") == 0);
  assert(strcmp(found->pins[1].version, "1") == 0);
  cbor_item_t *object = wd_session_to_cbor(found);
  cbor_item_t *pinned_array = wd_get(object, "pins");
  assert(pinned_array != NULL && cbor_array_size(pinned_array) == 2);
  cbor_item_t *first_pin = cbor_array_get(pinned_array, 0);
  char *pin_name = wd_text_copy(wd_get(first_pin, "name"));
  assert(pin_name != NULL && strcmp(pin_name, "driver") == 0);
  free(pin_name);
  cbor_decref(&first_pin);
  wd_free(object);

  snprintf(pinned_id, sizeof(pinned_id), "%s", found->id);
  uint64_t failed_seq = 0;
  assert(wd_session_note_migration(found, "driver", "3", "4",
                                   "the guest refused", &failed_seq) == 0);
  assert(failed_seq > 0);

  wd_store_close(&store);
  assert(wd_store_open(&store, dir) == 0);
  wd_session *replayed = wd_session_find(&store, pinned_id);
  assert(replayed != NULL);
  assert(strcmp(replayed->pins[0].name, "driver") == 0);
  assert(strcmp(replayed->pins[0].version, "3") == 0);

  {
    wd_session *stable = NULL;
    assert(wd_session_create(&store, "/tmp", NULL, &stable) == 0);
    cbor_item_t *first = NULL;
    cbor_item_t *second = NULL;
    for (int round = 1; round <= 2; round++) {
      cbor_item_t *when = wd_map(1);
      wd_put(when, "text", wd_text("2026-11-02 01:00:00 EET"));
      assert(wd_session_emit(stable, "session.time", when, NULL) == 0);
      cbor_item_t *ask = wd_map(1);
      char question[32];
      snprintf(question, sizeof(question), "question %d", round);
      wd_put(ask, "text", wd_text(question));
      assert(wd_session_emit(stable, "session.prompted", ask, NULL) == 0);
      cbor_item_t *piece = wd_map(1);
      char reply[32];
      snprintf(reply, sizeof(reply), "answer %d", round);
      wd_put(piece, "text", wd_text(reply));
      assert(wd_session_emit(stable, "text.delta", piece, NULL) == 0);
      assert(wd_session_emit(stable, "turn.end", wd_map(0), NULL) == 0);
      cbor_item_t *built_now = NULL;
      assert(wd_session_history(stable, 0, &built_now) == 0);
      if (round == 1) {
        first = built_now;
      } else {
        second = built_now;
      }
    }
    assert(cbor_array_size(first) == 3);
    assert(cbor_array_size(second) == 6);
    cbor_item_t **a = cbor_array_handle(first);
    cbor_item_t **b = cbor_array_handle(second);
    for (size_t i = 0; i < cbor_array_size(first); i++) {
      unsigned char *a_bytes = NULL;
      unsigned char *b_bytes = NULL;
      size_t a_len = cbor_serialize_alloc(a[i], &a_bytes, NULL);
      size_t b_len = cbor_serialize_alloc(b[i], &b_bytes, NULL);
      assert(a_bytes != NULL && b_bytes != NULL);
      assert(a_len == b_len);
      assert(memcmp(a_bytes, b_bytes, a_len) == 0);
      free(a_bytes);
      free(b_bytes);
    }
    wd_free(first);
    wd_free(second);

    wd_session *branch = NULL;
    assert(wd_session_fork(&store, stable, stable->last_seq, &branch) == 0);
    assert(branch != NULL);
    cbor_item_t *compacted = wd_map(2);
    wd_put(compacted, "through", wd_uint(branch->last_seq));
    wd_put(compacted, "summary", wd_text("two questions, two answers"));
    assert(wd_session_emit(branch, "session.compacted", compacted, NULL) == 0);
    cbor_item_t *branch_history = NULL;
    assert(wd_session_history(branch, 0, &branch_history) == 0);
    assert(cbor_array_size(branch_history) == 1);
    cbor_item_t **branch_items = cbor_array_handle(branch_history);
    char *branch_role = wd_text_copy(wd_get(branch_items[0], "role"));
    assert(branch_role != NULL && strcmp(branch_role, "system") == 0);
    free(branch_role);
    wd_free(branch_history);

    cbor_item_t *parent_history = NULL;
    assert(wd_session_history(stable, 0, &parent_history) == 0);
    assert(cbor_array_size(parent_history) == 6);
    wd_free(parent_history);
  }

  wd_store_close(&store);

  printf("session tests passed\n");
  return 0;
}
