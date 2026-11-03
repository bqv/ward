/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "net/http_status.h"

static long status_of(const char *line) {
  return wd_http_status_line(line, strlen(line));
}

int main(void) {
  assert(status_of("HTTP/1.1 200 OK\r\n") == 200);
  assert(status_of("HTTP/2 200\r\n") == 200);
  assert(status_of("HTTP/2 404\r\n") == 404);
  assert(status_of("HTTP/1.1 100 Continue\r\n") == 100);
  assert(status_of("HTTP/3 204\r\n") == 204);
  assert(status_of("HTTP/2 200") == 200);
  assert(status_of("NOTHTTP 200\r\n") == 0);
  assert(status_of("HTTP/2 \r\n") == 0);
  assert(status_of("") == 0);
  printf("http status tests passed\n");
  return 0;
}
