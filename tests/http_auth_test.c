/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/http.h"

int main(void) {
  unsetenv("WARD_API_KEY");
  assert(wd_http_auth_header("http://api.deepseek.com/v1/chat/completions") ==
         NULL);

  setenv("WARD_API_KEY", "sk-test-123", 1);
  char *header =
      wd_http_auth_header("http://api.deepseek.com/v1/chat/completions");
  assert(header != NULL);
  assert(strcmp(header, "Authorization: Bearer sk-test-123") == 0);
  free(header);

  assert(wd_http_auth_header("http://127.0.0.1:55556/v1/chat/completions") ==
         NULL);
  assert(wd_http_auth_header("http://localhost:55556/v1/chat/completions") ==
         NULL);
  assert(wd_http_auth_header("http://[::1]:55556/v1/chat/completions") == NULL);

  setenv("WARD_API_KEY", "", 1);
  assert(wd_http_auth_header("http://api.deepseek.com/v1/chat/completions") ==
         NULL);

  printf("http auth tests passed\n");
  return 0;
}
