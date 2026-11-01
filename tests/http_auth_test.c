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
#include <sys/wait.h>
#include <unistd.h>

#include "net/http.h"

static void serve_once(int listen_fd, int report_fd) {
  int fd = accept(listen_fd, NULL, NULL);
  assert(fd >= 0);
  char captured[4096];
  size_t used = 0;
  captured[0] = '\0';
  while (used + 1 < sizeof(captured)) {
    ssize_t n = read(fd, captured + used, sizeof(captured) - 1 - used);
    if (n <= 0) break;
    used += (size_t)n;
    captured[used] = '\0';
    if (strstr(captured, "\r\n\r\n") != NULL) break;
  }
  static const char reply[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
  assert(write(fd, reply, sizeof(reply) - 1) > 0);
  close(fd);
  ssize_t ignored = write(report_fd, captured, used);
  (void)ignored;
}

static void capture_request(const char *url, char *out, size_t cap) {
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  int listen_fd = dup(3);
  assert(listen_fd >= 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(pipefd[0]);
    serve_once(listen_fd, pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);
  wd_http_response response = {0};
  char *err = NULL;
  assert(wd_http_request(url, "POST", "{}", 5000, 1 << 20, &response, &err) == 0);
  assert(response.status == 200);
  wd_http_response_free(&response);
  ssize_t n = read(pipefd[0], out, cap - 1);
  assert(n >= 0);
  out[n > 0 ? (size_t)n : 0] = '\0';
  close(pipefd[0]);
  close(listen_fd);
  int status = 0;
  waitpid(pid, &status, 0);
}

int main(void) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(listen_fd >= 0);
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(listen_fd, 8) == 0);
  socklen_t alen = sizeof(addr);
  assert(getsockname(listen_fd, (struct sockaddr *)&addr, &alen) == 0);
  assert(dup2(listen_fd, 3) == 3);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1/chat/completions",
           (unsigned)ntohs(addr.sin_port));

  char request[4096];
  setenv("WARD_API_KEY", "sk-test-123", 1);
  capture_request(url, request, sizeof(request));
  assert(strstr(request, "Authorization: Bearer sk-test-123") != NULL);

  unsetenv("WARD_API_KEY");
  capture_request(url, request, sizeof(request));
  assert(strstr(request, "Authorization:") == NULL);

  close(listen_fd);
  printf("http auth tests passed\n");
  return 0;
}
