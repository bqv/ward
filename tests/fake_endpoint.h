/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_TEST_FAKE_ENDPOINT_H
#define WARD_TEST_FAKE_ENDPOINT_H

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_REQUEST 16384

static int find_int(const char *buf, const char *key) {
  const char *at = strstr(buf, key);
  if (at == NULL) return -1;
  at += strlen(key);
  int value = 0;
  while (*at >= '0' && *at <= '9') {
    value = value * 10 + (*at - '0');
    at++;
  }
  return value;
}

static int extract_content(const char *buf, char *out, size_t cap) {
  const char *at = strstr(buf, "\"content\":\"");
  if (at == NULL) return -1;
  for (const char *next = strstr(at + 1, "\"content\":\"");
       next != NULL; next = strstr(next + 1, "\"content\":\""))
    at = next;
  at += strlen("\"content\":\"");
  size_t n = 0;
  while (*at != '\0' && *at != '"' && n + 1 < cap) out[n++] = *at++;
  out[n] = '\0';
  return (int)n;
}

static void serve(int fd) {
  const char *delay = getenv("WARD_FAKE_DELAY_MS");
  if (delay != NULL) usleep((useconds_t)(atoi(delay) * 1000));
  char request[MAX_REQUEST];
  size_t len = 0;
  int content_length = -1;
  for (;;) {
    ssize_t got = read(fd, request + len, sizeof(request) - 1 - len);
    if (got <= 0) break;
    len += (size_t)got;
    request[len] = '\0';
    if (content_length < 0) content_length = find_int(request, "Content-Length: ");
    const char *end = strstr(request, "\r\n\r\n");
    if (end != NULL) {
      size_t header = (size_t)(end - request) + 4;
      if (content_length < 0 || len >= header + (size_t)content_length) break;
    }
  }
  request[len] = '\0';
  char prompt[256];
  if (extract_content(request, prompt, sizeof(prompt)) < 0) prompt[0] = '\0';
  if (strstr(request, "be kind to the tools") != NULL) {
    size_t used = strlen(prompt);
    snprintf(prompt + used, sizeof(prompt) - used, " [rules]");
  }
  if (strstr(request, "note.txt") != NULL) {
    size_t used = strlen(prompt);
    snprintf(prompt + used, sizeof(prompt) - used, " [attach]");
  }
  size_t half = strlen(prompt) / 2;
  char response[8192];
  int n = snprintf(response, sizeof(response),
                   "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                   "Connection: close\r\n\r\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"%.*s\"}}]}\n\n"
                   "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n"
                   "data: [DONE]\n\n",
                   (int)half, prompt, prompt + half);
  if (n > 0) {
    ssize_t written = write(fd, response, (size_t)n);
    (void)written;
  }
  close(fd);
}

static pid_t start_server(int *port_out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  assert(listen(fd, 1) == 0);
  socklen_t size = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &size) == 0);
  *port_out = ntohs(addr.sin_port);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    for (int i = 0; i < 8; i++) {
      int conn = accept(fd, NULL, NULL);
      if (conn < 0) break;
      pid_t worker = fork();
      if (worker == 0) {
        close(fd);
        serve(conn);
        _exit(0);
      }
      close(conn);
      if (worker < 0) serve(conn);
    }
    int status = 0;
    while (waitpid(-1, &status, 0) > 0) {
    }
    close(fd);
    _exit(0);
  }
  return pid;
}


static void stop_server(pid_t pid) {
  kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
}

static char *fake_endpoint(int *port_out, pid_t *pid_out) {
  int port = 0;
  *pid_out = start_server(&port);
  *port_out = port;
  static char url[64];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
  setenv("WARD_ENDPOINT", url, 1);
  return url;
}

#endif
