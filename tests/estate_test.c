/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net/estate.h"

static const char busy[] =
    "{\"group\":\"gpu\",\"holders\":[\"sd\"],\"since_ms\":1793711314557,"
    "\"slots\":[{\"name\":\"llama\",\"state\":\"stopped\"},"
    "{\"name\":\"sd\",\"state\":\"running\"}]}";
static const char idle[] =
    "{\"group\":\"gpu\",\"holders\":[],\"since_ms\":0,"
    "\"slots\":[{\"name\":\"sd\",\"state\":\"loading\"}]}";

static void check_parse(void) {
  wd_estate_card card;
  assert(wd_estate_parse(busy, &card) == 0);
  assert(strcmp(card.group, "gpu") == 0);
  assert(card.holders_len == 1 && strcmp(card.holders[0], "sd") == 0);
  assert(card.since_ms == 1793711314557ull);
  assert(card.slots_len == 2 && strcmp(card.slots[1].state, "running") == 0);

  assert(wd_estate_parse(idle, &card) == 0);
  assert(card.holders_len == 0);
  assert(card.since_ms == 0);
  assert(card.slots_len == 1 && strcmp(card.slots[0].state, "loading") == 0);
}

static pid_t serve_once(const char *path, const char *body) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(1);
    if (listen(fd, 1) != 0) _exit(1);
    int conn = accept(fd, NULL, NULL);
    if (conn < 0) _exit(1);
    char reply[1024];
    int made = snprintf(reply, sizeof(reply),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                        strlen(body), body);
    if (made > 0) (void)!write(conn, reply, (size_t)made);
    close(conn);
    close(fd);
    _exit(0);
  }
  return pid;
}

int main(void) {
  alarm(30);
  check_parse();

  char dir[] = "/tmp/ward-estate-XXXXXX";
  assert(mkdtemp(dir) != NULL);
  char sock[sizeof(((struct sockaddr_un *)0)->sun_path)];
  int made = snprintf(sock, sizeof(sock), "%s/s", dir);
  assert(made > 0 && (size_t)made < sizeof(sock));

  pid_t server = serve_once(sock, busy);
  for (int tries = 0; tries < 200 && access(sock, F_OK) != 0; tries++)
    usleep(10000);
  wd_estate_card live;
  char *err = NULL;
  if (wd_estate_card_read(sock, "gpu", &live, &err) != 0) {
    fprintf(stderr, "read failed: %s\n", err != NULL ? err : "(none)");
    assert(0);
  }
  assert(live.holders_len == 1 && strcmp(live.holders[0], "sd") == 0);
  waitpid(server, NULL, 0);
  unlink(sock);

  pid_t idle_server = serve_once(sock, idle);
  for (int tries = 0; tries < 200 && access(sock, F_OK) != 0; tries++)
    usleep(10000);
  assert(wd_estate_card_read(sock, "gpu", &live, &err) == 0);
  assert(live.holders_len == 0);
  assert(live.slots_len == 1 && strcmp(live.slots[0].state, "loading") == 0);
  waitpid(idle_server, NULL, 0);

  printf("estate tests passed\n");
  return 0;
}
