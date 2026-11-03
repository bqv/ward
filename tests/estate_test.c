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
static const char spaced[] =
    "{ \"group\" : \"gpu\" , \"holders\" : [ \"sd\" , \"llama\" ] , "
    "\"since_ms\" : 42 , \"slots\" : [ { \"name\" : \"sd\" , "
    "\"state\" : \"running\" } ] }";
static const char null_group[] =
    "{\"group\":null,\"holders\":[\"sd\"],\"slots\":[]}";

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

  assert(wd_estate_parse(spaced, &card) == 0);
  assert(strcmp(card.group, "gpu") == 0);
  assert(card.holders_len == 2 && strcmp(card.holders[1], "llama") == 0);
  assert(card.since_ms == 42);
  assert(card.slots_len == 1 && strcmp(card.slots[0].state, "running") == 0);

  assert(wd_estate_parse(null_group, &card) != 0);
}

static pid_t serve_once(const char *path, const char *body) {
  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) _exit(1);
    if (listen(fd, 1) != 0) _exit(1);
    char one = '1';
    (void)!write(ready[1], &one, 1);
    close(ready[1]);
    int conn = accept(fd, NULL, NULL);
    if (conn < 0) _exit(1);
    char request[4096];
    (void)!read(conn, request, sizeof(request));
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
  close(ready[1]);
  char one = 0;
  (void)!read(ready[0], &one, 1);
  close(ready[0]);
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
  assert(wd_estate_card_read(sock, "gpu", &live, &err) == 0);
  assert(live.holders_len == 0);
  assert(live.slots_len == 1 && strcmp(live.slots[0].state, "loading") == 0);
  waitpid(idle_server, NULL, 0);

  const char *live_group = getenv("WARD_ESTATE_LIVE");
  if (live_group != NULL && live_group[0] != '\0') {
    const char *path = getenv("WARD_DEMESNE_SOCKET");
    if (path == NULL || path[0] == '\0')
      path = "/run/user/1000/demesne.sock";
    wd_estate_card real;
    if (wd_estate_card_read(path, live_group, &real, &err) != 0) {
      fprintf(stderr, "live estate read failed: %s\n",
              err != NULL ? err : "(none)");
      assert(0);
    }
    assert(strcmp(real.group, live_group) == 0);
    printf("live estate: group=%s holders=%zu since_ms=%llu slots=%zu\n",
           real.group, real.holders_len, (unsigned long long)real.since_ms,
           real.slots_len);
  }

  printf("estate tests passed\n");
  return 0;
}
