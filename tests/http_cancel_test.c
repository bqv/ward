/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net/http.h"

int main(void) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(listen_fd >= 0);
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(listen_fd, 4) == 0);
  socklen_t alen = sizeof(addr);
  assert(getsockname(listen_fd, (struct sockaddr *)&addr, &alen) == 0);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%u/slow",
           (unsigned)ntohs(addr.sin_port));

  char *err = NULL;
  wd_http_job *job = wd_http_start(url, "POST", "{}", NULL, 60000, 1 << 20, &err);
  assert(job != NULL);

  struct timespec before;
  clock_gettime(CLOCK_MONOTONIC, &before);
  wd_http_job_cancel(job);
  wd_http_job_free(job);
  struct timespec after;
  clock_gettime(CLOCK_MONOTONIC, &after);
  double elapsed = (double)(after.tv_sec - before.tv_sec) +
                   (double)(after.tv_nsec - before.tv_nsec) / 1e9;
  assert(elapsed < 2.0);

  close(listen_fd);
  printf("http cancel tests passed (%.2fs)\n", elapsed);
  return 0;
}
