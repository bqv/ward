/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/files.h"

static void write_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  assert(fd >= 0);
  size_t len = strlen(text);
  assert(write(fd, text, len) == (ssize_t)len);
  close(fd);
}

int main(void) {
  char root[] = "/tmp/ward-files-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char inside[256];
  char outside[256];
  snprintf(inside, sizeof(inside), "%s/note.txt", root);
  snprintf(outside, sizeof(outside), "%s-escape.txt", root);
  write_file(inside, "hello from the session directory");
  write_file(outside, "outside");

  char *text = NULL;
  size_t len = 0;
  char *err = NULL;
  assert(wd_file_read(root, "note.txt", 1024, &text, &len, &err) == 0);
  assert(len == strlen("hello from the session directory"));
  assert(strcmp(text, "hello from the session directory") == 0);
  free(text);

  err = NULL;
  assert(wd_file_read(root, "../escape.txt", 1024, &text, &len, &err) != 0);
  assert(err != NULL && strstr(err, "outside the session directory") != NULL);
  free(err);

  err = NULL;
  assert(wd_file_read(root, outside, 1024, &text, &len, &err) != 0);
  assert(err != NULL && strstr(err, "outside the session directory") != NULL);
  free(err);

  err = NULL;
  assert(wd_file_read(root, "absent.txt", 1024, &text, &len, &err) != 0);
  assert(err != NULL);
  free(err);

  err = NULL;
  assert(wd_file_read(root, "note.txt", 8, &text, &len, &err) != 0);
  assert(err != NULL && strstr(err, "exceeds the 8 byte limit") != NULL);
  free(err);

  printf("files tests passed\n");
  return 0;
}
