/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/turn.h"
#include "proto/codec.h"

typedef struct {
  char text[4096];
  int deltas;
  int approvals;
  uint64_t call;
  uint64_t epoch;
  char reason[256];
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
  } else if (strcmp(event, "approval.requested") == 0) {
    s->approvals++;
    assert(wd_uint_of(wd_get(payload, "call"), &s->call));
    assert(wd_uint_of(wd_get(payload, "epoch"), &s->epoch));
    char *reason = wd_text_copy(wd_get(payload, "reason"));
    assert(reason != NULL);
    snprintf(s->reason, sizeof(s->reason), "%s", reason);
    free(reason);
  }
  return 0;
}

static void write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file != NULL);
  assert(fputs(text, file) >= 0);
  fclose(file);
}

static char *read_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) return NULL;
  size_t cap = 4096;
  size_t len = 0;
  char *buf = malloc(cap);
  assert(buf != NULL);
  size_t n;
  while ((n = fread(buf + len, 1, cap - len - 1, file)) > 0) {
    len += n;
    if (len + 1 == cap) {
      cap *= 2;
      char *grown = realloc(buf, cap);
      assert(grown != NULL);
      buf = grown;
    }
  }
  fclose(file);
  buf[len] = '\0';
  return buf;
}

int main(void) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  char state[] = "/tmp/ward-tool-state-XXXXXX";
  assert(mkdtemp(state) != NULL);

  char root[] = "/tmp/ward-tools-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char path[512];
  snprintf(path, sizeof(path), "%s/hello.txt", root);
  write_file(path, "the file text");

  wd_engine *engine = wd_engine_new(state);
  assert(engine != NULL);
  char *err = NULL;
  wd_turn *turn = wd_turn_new(engine, dir, root, &err);
  if (turn == NULL) {
    fprintf(stderr, "wd_turn_new: %s\n", err != NULL ? err : "failed");
    return 1;
  }

  char source[512];
  snprintf(source, sizeof(source), "%s/sample.c", root);
  write_file(source, "int main(void) {\n  return 0;\n}\n\nvoid helper(int x) {\n}\n");

  seen outline_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/outline sample.c", emit, &outline_seen, &err) ==
         WD_TURN_OK);
  assert(outline_seen.deltas > 0);
  assert(strstr(outline_seen.text, "main:1") != NULL);
  assert(strstr(outline_seen.text, "helper:5") != NULL);

  seen defs_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/defs helper", emit, &defs_seen, &err) ==
         WD_TURN_OK);
  assert(defs_seen.deltas > 0);
  assert(strstr(defs_seen.text, "sample.c:5 helper") != NULL);

  seen read_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/read hello.txt", emit, &read_seen, &err) ==
         WD_TURN_OK);
  assert(read_seen.deltas == 1);
  assert(strcmp(read_seen.text, "the file text") == 0);

  seen escape = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/read ../../etc/passwd", emit, &escape, &err) ==
         WD_TURN_OK);
  assert(escape.deltas == 1);
  assert(strstr(escape.text, "outside the tool root") != NULL);

  seen write_seen = {{0}, 0, 0, 0, 0, {0}};
  int rc = wd_turn_prompt(turn, "/write out.txt written here", emit,
                          &write_seen, &err);
  assert(rc == WD_TURN_PENDING);
  uint64_t call = 0;
  uint64_t epoch = 0;
  char *reason = NULL;
  assert(wd_turn_pending(turn, &call, &epoch, &reason) == 0);
  assert(call != 0 && epoch != 0);
  assert(reason != NULL && strstr(reason, "write") != NULL);
  free(reason);
  assert(write_seen.approvals == 1);
  assert(write_seen.call == call && write_seen.epoch == epoch);
  assert(wd_turn_resume(turn, true, emit, &write_seen, &err) == WD_TURN_OK);
  assert(write_seen.deltas >= 1);
  assert(strstr(write_seen.text, "wrote") != NULL);
  snprintf(path, sizeof(path), "%s/out.txt", root);
  char *written = read_file(path);
  assert(written != NULL && strcmp(written, "written here") == 0);
  free(written);

  seen denied = {{0}, 0, 0, 0, 0, {0}};
  rc = wd_turn_prompt(turn, "/write blocked.txt nope", emit, &denied, &err);
  assert(rc == WD_TURN_PENDING);
  assert(denied.approvals == 1);
  assert(wd_turn_resume(turn, false, emit, &denied, &err) == WD_TURN_OK);
  assert(strstr(denied.text, "denied") != NULL);
  snprintf(path, sizeof(path), "%s/blocked.txt", root);
  assert(access(path, F_OK) != 0);

  seen grep_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/grep file text", emit, &grep_seen, &err) ==
         WD_TURN_OK);
  assert(grep_seen.approvals == 0);
  assert(strstr(grep_seen.text, "hello.txt") != NULL);
  assert(strstr(grep_seen.text, "the file text") != NULL);

  seen edit_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/edit hello.txt file FILE", emit, &edit_seen,
                        &err) == WD_TURN_PENDING);
  assert(edit_seen.approvals == 1);
  assert(wd_turn_resume(turn, true, emit, &edit_seen, &err) == WD_TURN_OK);
  snprintf(path, sizeof(path), "%s/hello.txt", root);
  char *edited = read_file(path);
  assert(edited != NULL && strcmp(edited, "the FILE text") == 0);
  free(edited);

  seen edit_missing = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/edit hello.txt nothing here", emit,
                        &edit_missing, &err) == WD_TURN_PENDING);
  assert(wd_turn_resume(turn, true, emit, &edit_missing, &err) == WD_TURN_OK);
  assert(strstr(edit_missing.text, "not in the file") != NULL);

  seen exec_seen = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/run echo tool exec works", emit, &exec_seen,
                        &err) == WD_TURN_PENDING);
  assert(exec_seen.approvals == 1);
  assert(strstr(exec_seen.reason, "exec") != NULL ||
         exec_seen.reason[0] != '\0');
  assert(wd_turn_resume(turn, true, emit, &exec_seen, &err) == WD_TURN_PENDING);
  assert(wd_turn_pending_kind(turn) == 2);
  assert(wd_turn_resume_tool(turn, emit, &exec_seen, &err) == WD_TURN_OK);
  assert(strstr(exec_seen.text, "tool exec works") != NULL);

  seen exec_denied = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/run echo nope", emit, &exec_denied, &err) ==
         WD_TURN_PENDING);
  assert(wd_turn_resume(turn, false, emit, &exec_denied, &err) == WD_TURN_OK);
  assert(strstr(exec_denied.text, "denied") != NULL);

  seen exec_fails = {{0}, 0, 0, 0, 0, {0}};
  assert(wd_turn_prompt(turn, "/run /nonexistent/command", emit, &exec_fails,
                        &err) == WD_TURN_PENDING);
  assert(wd_turn_resume(turn, true, emit, &exec_fails, &err) == WD_TURN_PENDING);
  assert(wd_turn_resume_tool(turn, emit, &exec_fails, &err) == WD_TURN_OK);
  assert(strstr(exec_fails.text, "cannot") != NULL ||
         strstr(exec_fails.text, "127") != NULL);

  wd_turn_free(turn);
  wd_engine_free(engine);
  unlink(path);
  printf("tool tests passed\n");
  return 0;
}
