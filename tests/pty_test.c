/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/pty.h"

static int wait_for(int fd, int ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  return poll(&pfd, 1, ms) > 0;
}

int main(void) {
  char *err = NULL;
  const char *argv[] = {"cat", NULL};
  wd_pty *pty = wd_pty_open(argv, "/tmp", 24, 80, &err);
  assert(pty != NULL);
  assert(wd_pty_fd(pty) >= 0);
  assert(wd_pty_pid(pty) > 0);
  assert(wd_pty_resize(pty, 40, 100) == 0);
  const char *line = "hello from the pty\n";
  assert(wd_pty_write(pty, line, strlen(line)) == (ssize_t)strlen(line));
  char buf[256];
  size_t got = 0;
  for (int i = 0; i < 100 && got < 12; i++) {
    if (!wait_for(wd_pty_fd(pty), 100)) continue;
    ssize_t n = wd_pty_read(pty, buf + got, sizeof(buf) - got - 1);
    if (n > 0) got += (size_t)n;
  }
  buf[got] = '\0';
  assert(strstr(buf, "hello from the pty") != NULL);
  assert(wd_pty_alive(pty));
  wd_pty_close(pty);
  printf("pty tests passed\n");
  return 0;
}
