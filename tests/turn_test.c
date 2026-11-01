/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/turn.h"
#include "fake_endpoint.h"
#include "proto/codec.h"

static uint8_t *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  size_t cap = 1 << 16, used = 0;
  uint8_t *bytes = malloc(cap);
  size_t n;
  while (bytes != NULL && (n = fread(bytes + used, 1, cap - used, f)) > 0) {
    used += n;
    if (used == cap) {
      uint8_t *grown = realloc(bytes, cap * 2);
      if (grown == NULL) {
        free(bytes);
        bytes = NULL;
        break;
      }
      bytes = grown;
      cap *= 2;
    }
  }
  fclose(f);
  *len = used;
  return bytes;
}

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
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  char *err = NULL;
  char root[] = "/tmp/ward-turn-XXXXXX";
  assert(mkdtemp(root) != NULL);
  wd_turn *turn = wd_turn_new(engine, dir, root, &err);
  if (turn == NULL) {
    fprintf(stderr, "wd_turn_new: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  events seen = {0, 0, {0}};
  int rc = run_turn(turn, "hello", emit, &seen, &err);
  if (rc != 0) {
    fprintf(stderr, "wd_turn_prompt: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  assert(seen.deltas == 1);
  assert(strstr(seen.text, "hello") != NULL);
  assert(seen.ended == 1);

  events second = {0, 0, {0}};
  rc = run_turn(turn, "again", emit, &second, &err);
  if (rc != 0) {
    fprintf(stderr, "wd_turn_prompt second: %s\n", err != NULL ? err : "failed");
    return 1;
  }
  assert(second.deltas == 1);
  assert(strstr(second.text, "again") != NULL);
  assert(second.ended == 1);

  uint8_t *blob = NULL;
  size_t blob_len = 0;
  char *state_err = NULL;
  assert(wd_turn_state_export(turn, "driver", &blob, &blob_len, &state_err) == 1);
  assert(blob == NULL && blob_len == 0);
  free(state_err);
  assert(wd_turn_state_export(turn, "nonesuch", &blob, &blob_len, &state_err) == -1);

  wd_turn_free(turn);

  char pinned_dir[] = "/tmp/ward-turn-XXXXXX";
  assert(mkdtemp(pinned_dir) != NULL);
  DIR *source = opendir(dir);
  assert(source != NULL);
  struct dirent *entry;
  while ((entry = readdir(source)) != NULL) {
    if (entry->d_name[0] == '.') continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
    size_t len = 0;
    uint8_t *bytes = read_all(path, &len);
    assert(bytes != NULL);
    char name[512];
    if (strcmp(entry->d_name, "driver.wasm") == 0)
      snprintf(name, sizeof(name), "driver@1.wasm");
    else
      snprintf(name, sizeof(name), "%s", entry->d_name);
    char out[1024];
    snprintf(out, sizeof(out), "%s/%s", pinned_dir, name);
    FILE *f = fopen(out, "wb");
    assert(f != NULL);
    assert(fwrite(bytes, 1, len, f) == len);
    fclose(f);
    free(bytes);
  }
  closedir(source);
  FILE *broken = fopen("/tmp/ward-turn-driver.wasm", "wb");
  assert(broken != NULL);
  fputs("not a wasm module", broken);
  fclose(broken);
  char corrupt[1024];
  snprintf(corrupt, sizeof(corrupt), "%s/driver.wasm", pinned_dir);
  rename("/tmp/ward-turn-driver.wasm", corrupt);

  assert(wd_turn_new(engine, pinned_dir, root, &err) == NULL);
  free(err);
  err = NULL;
  wd_pin pins[1];
  snprintf(pins[0].name, sizeof(pins[0].name), "%s", "driver");
  snprintf(pins[0].version, sizeof(pins[0].version), "%s", "1");
  wd_turn *pinned = wd_turn_new_pinned(engine, pinned_dir, root, pins, 1, &err);
  assert(pinned != NULL);
  wd_turn_free(pinned);

  wd_engine_free(engine);
  stop_server(server);
  printf("turn tests passed\n");
  return 0;
}
