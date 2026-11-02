/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/codec.h"

static size_t from_hex(const char *hex, uint8_t *out, size_t cap) {
  size_t used = 0;
  for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2) {
    char pair[3] = {hex[i], hex[i + 1], '\0'};
    if (used + 1 >= cap) break;
    out[used++] = (uint8_t)strtoul(pair, NULL, 16);
  }
  return used;
}

int main(void) {
  FILE *fixture = fopen("tests/transcripts/create.txt", "r");
  if (fixture == NULL) {
    printf("conformance tests skipped: no fixture\n");
    return 0;
  }
  char line[8192];
  size_t frames = 0;
  while (fgets(line, sizeof(line), fixture) != NULL) {
    char *space = strchr(line, ' ');
    if (space == NULL) continue;
    char *hex = space + 1;
    size_t len = strlen(hex);
    while (len > 0 && (hex[len - 1] == '\n' || hex[len - 1] == '\r')) hex[--len] = '\0';
    if (strncmp(line, "note", 4) == 0) continue;
    static uint8_t bytes[4096];
    size_t size = from_hex(hex, bytes, sizeof(bytes));
    assert(size > 0);
    cbor_item_t *item = wd_decode(bytes, size);
    assert(item != NULL);
    size_t again = 0;
    uint8_t *round = wd_encode(item, &again);
    assert(round != NULL);
    assert(again == size);
    assert(memcmp(round, bytes, size) == 0);
    free(round);
    frames++;
  }
  fclose(fixture);
  assert(frames >= 3);
  printf("conformance tests passed: %zu frames round-trip byte for byte\n",
         frames);
  return 0;
}
