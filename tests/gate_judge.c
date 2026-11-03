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

int main(void) {
  alarm(90);
  const char *daemon = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  assert(daemon != NULL && modules != NULL);

  char root[] = "/tmp/ward-judge-gate-XXXXXX";
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

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    char cgroup_base[128];
    const char *cgroup_override = getenv("WARD_GATE_CGROUP");
    if (cgroup_override != NULL)
      snprintf(cgroup_base, sizeof(cgroup_base), "%s", cgroup_override);
    else
      snprintf(cgroup_base, sizeof(cgroup_base),
               "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
    setenv("WARD_CGROUP", cgroup_base, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    setenv("WARD_JUDGE_ENDPOINT", endpoint_url, 1);
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

  wd_client client;
  assert(wd_client_connect(&client, sock) == 0);
  assert(wd_client_handshake(&client) == 0);

  cbor_item_t *pick = wd_map(2);
  wd_put(pick, "id", wd_text("full-access"));
  wd_put(pick, "confirm", cbor_build_bool(true));
  cbor_item_t *picked = NULL;
  assert(wd_client_call(&client, "policy.select", pick, &picked) == 0);
  wd_free(picked);

  cbor_item_t *create = wd_map(1);
  wd_put(create, "cwd", wd_text(root));
  cbor_item_t *created = NULL;
  assert(wd_client_call(&client, "session.create", create, &created) == 0);
  char *session = wd_text_copy(wd_get(wd_get(created, "session"), "id"));
  assert(session != NULL);
  wd_free(created);

  cbor_item_t *prompt = wd_map(2);
  wd_put(prompt, "id", wd_text(session));
  wd_put(prompt, "text", wd_text("/write judged.txt risky"));
  cbor_item_t *answered = NULL;
  assert(wd_client_call(&client, "session.prompt", prompt, &answered) == 0);
  assert(wd_get(answered, "approval") != NULL);
  wd_free(answered);
  free(session);
  close(client.fd);

  stop_server(endpoint);
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);

  printf("judge gate passed: a scored write parked for approval\n");
  return 0;
}
