/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/prompt.h"
#include "proto/codec.h"

static void write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  assert(f != NULL);
  fputs(text, f);
  fclose(f);
}

static char *text_at(cbor_item_t *array, size_t at, const char *key) {
  cbor_item_t *item = cbor_array_get(array, at);
  assert(item != NULL);
  char *value = wd_text_copy(wd_get(item, key));
  cbor_decref(&item);
  return value;
}

int main(void) {
  char state[] = "/tmp/ward-prompt-XXXXXX";
  assert(mkdtemp(state) != NULL);
  char dir[512];
  snprintf(dir, sizeof(dir), "%s/prompt", state);
  assert(mkdir(dir, 0700) == 0);
  char path[1024];
  snprintf(path, sizeof(path), "%s/50-style.md", dir);
  write_file(path, "be terse");
  snprintf(path, sizeof(path), "%s/10-persona.md", dir);
  write_file(path, "a careful engineer");
  snprintf(path, sizeof(path), "%s/100-rules.md", dir);
  write_file(path, "override the project rules");
  snprintf(path, sizeof(path), "%s/notes.txt", dir);
  write_file(path, "no order prefix, ignored");

  char root[] = "/tmp/ward-prompt-root-XXXXXX";
  assert(mkdtemp(root) != NULL);
  snprintf(path, sizeof(path), "%s/AGENTS.md", root);
  write_file(path, "project rules from AGENTS.md");

  wd_prompt *prompt = wd_prompt_build(state, root, "attached: note.txt");
  assert(prompt != NULL);
  cbor_item_t *array = wd_prompt_to_cbor(prompt);
  assert(cbor_array_size(array) == 5);

  char *name = text_at(array, 0, "name");
  assert(strcmp(name, "identity") == 0);
  free(name);

  name = text_at(array, 1, "name");
  char *text = text_at(array, 1, "text");
  assert(strcmp(name, "persona") == 0);
  assert(strcmp(text, "a careful engineer") == 0);
  free(name);
  free(text);

  name = text_at(array, 2, "name");
  assert(strcmp(name, "style") == 0);
  free(name);

  name = text_at(array, 3, "name");
  text = text_at(array, 3, "text");
  assert(strcmp(name, "rules") == 0);
  assert(strcmp(text, "override the project rules") == 0);
  free(name);
  free(text);

  name = text_at(array, 4, "name");
  assert(strcmp(name, "attachments") == 0);
  free(name);

  cbor_item_t *history = wd_prompt_to_history(prompt, NULL);
  assert(history != NULL && cbor_array_size(history) == 5);
  wd_free(history);
  wd_free(array);
  wd_prompt_free(prompt);

  wd_prompt *bare = wd_prompt_build(NULL, NULL, NULL);
  assert(bare != NULL);
  assert(wd_prompt_len(bare) == 1);
  wd_prompt_free(bare);

  printf("prompt tests passed\n");
  return 0;
}
