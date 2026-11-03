/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "client/client.h"
#include "fake_endpoint.h"
#include "proto/codec.h"

static pid_t start_daemon(const char *daemon, const char *modules,
                          const char *sock, const char *state) {
  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    char cgroup_base[128];
    const char *override = getenv("WARD_GATE_CGROUP");
    if (override != NULL)
      snprintf(cgroup_base, sizeof(cgroup_base), "%s", override);
    else
      snprintf(cgroup_base, sizeof(cgroup_base),
               "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
    setenv("WARD_CGROUP", cgroup_base, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(daemon, "ward", "up", "--socket", sock, "--state", state,
          (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[256];
  bool ready_seen = false;
  size_t used = 0;
  while (!ready_seen) {
    size_t got = read(ready[0], line + used, 1);
    if (got <= 0) break;
    if (line[used++] != '\n') continue;
    line[used] = '\0';
    if (strncmp(line, "ready ", 6) == 0) ready_seen = true;
    used = 0;
  }
  close(ready[0]);
  assert(ready_seen);
  return pid;
}

static void stop_daemon(pid_t pid) {
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
}

int main(void) {
  alarm(120);
  const char *daemon = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  assert(daemon != NULL && modules != NULL);

  char root[] = "/tmp/ward-reindex-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);

  int endpoint_port = 0;
  pid_t endpoint = 0;
  char *endpoint_url = fake_endpoint(&endpoint_port, &endpoint);
  char models_path[512];
  snprintf(models_path, sizeof(models_path), "%s/models.toml", state);
  FILE *models = fopen(models_path, "w");
  assert(models != NULL);
  assert(fprintf(models,
                 "[[model]]\nid = \"local:qwen\"\nprovider = \"stadi\"\n"
                 "model = \"qwen3\"\ncontext = \"32768\"\n"
                 "endpoint = \"%s\"\n",
                 endpoint_url) > 0);
  fclose(models);

  pid_t first = start_daemon(daemon, modules, sock, state);
  wd_client client;
  assert(wd_client_connect(&client, sock) == 0);
  assert(wd_client_handshake(&client) == 0);
  cbor_item_t *create = wd_map(1);
  wd_put(create, "cwd", wd_text(root));
  cbor_item_t *created = NULL;
  assert(wd_client_call(&client, "session.create", create, &created) == 0);
  char *session = wd_text_copy(wd_get(wd_get(created, "session"), "id"));
  assert(session != NULL);
  wd_free(created);
  cbor_item_t *prompt = wd_map(2);
  wd_put(prompt, "id", wd_text(session));
  wd_put(prompt, "text", wd_text("reindexable phrase"));
  cbor_item_t *answered = NULL;
  assert(wd_client_call(&client, "session.prompt", prompt, &answered) == 0);
  wd_free(answered);
  close(client.fd);
  free(session);
  stop_daemon(first);

  char search_db[512];
  snprintf(search_db, sizeof(search_db), "%s/search.db", state);
  unlink(search_db);
  char search_wal[520];
  snprintf(search_wal, sizeof(search_wal), "%s-wal", search_db);
  unlink(search_wal);

  pid_t second = start_daemon(daemon, modules, sock, state);
  wd_client reader;
  assert(wd_client_connect(&reader, sock) == 0);
  assert(wd_client_handshake(&reader) == 0);
  cbor_item_t *query = wd_map(1);
  wd_put(query, "query", wd_text("reindexable"));
  cbor_item_t *found = NULL;
  assert(wd_client_call(&reader, "session.search_text", query, &found) == 0);
  cbor_item_t *hits = wd_get(found, "hits");
  assert(hits != NULL && cbor_array_size(hits) >= 1);
  wd_free(found);
  close(reader.fd);
  stop_daemon(second);
  stop_server(endpoint);

  printf("reindex gate passed: an old session was searchable after a restart\n");
  return 0;
}
