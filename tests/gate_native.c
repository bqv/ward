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
#include "proto/codec.h"
#include "fake_endpoint.h"

static int run_capture(const char *socket, const char *bin, char *const argv[],
                       char *out, size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    alarm(45);
    setenv("WARD_SOCKET", socket, 1);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    execv(bin, argv);
    _exit(127);
  }
  close(fds[1]);
  size_t used = 0;
  while (used + 1 < cap) {
    ssize_t got = read(fds[0], out + used, cap - 1 - used);
    if (got <= 0) break;
    used += got;
  }
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void) {
  const char *modules = getenv("WARD_MODULE_DIR");
  const char *bin = getenv("WARD_NATIVE");
  const char *daemon = getenv("WARD_BIN");
  assert(modules != NULL && bin != NULL && daemon != NULL);

  char root[] = "/tmp/ward-native-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);

  sweep_test_cgroups();
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
    snprintf(cgroup_base, sizeof(cgroup_base),
             "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
    setenv("WARD_CGROUP", cgroup_base, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    setenv("WARD_FAKE_DELAY_MS", "1500", 1);
    setenv("WARD_TCP_BIND", "127.0.0.1:0", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(daemon, "ward", "up", "--socket", sock, "--state", state,
          (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[256];
  char tcp_url[64];
  tcp_url[0] = '\0';
  bool ready_seen = false;
  size_t used = 0;
  while (!ready_seen) {
    size_t got = read(ready[0], line + used, 1);
    if (got <= 0) break;
    if (line[used++] != '\n') continue;
    line[used] = '\0';
    if (strncmp(line, "tcp ", 4) == 0) {
      snprintf(tcp_url, sizeof(tcp_url), "tcp://%s", line + 4);
      size_t len = strlen(tcp_url);
      while (len > 0 && (tcp_url[len - 1] == '\n')) tcp_url[--len] = '\0';
    }
    if (strncmp(line, "ready ", 6) == 0) ready_seen = true;
    used = 0;
  }
  close(ready[0]);
  assert(ready_seen);
  assert(tcp_url[0] != '\0');

  char out[8192];
  char *create_argv[] = {(char *)bin, "create", root, NULL};
  assert(run_capture(sock, bin, create_argv, out, sizeof(out)) == 0);
  char session[64];
  size_t at = 0;
  while (out[at] != '\0' && out[at] != '\n' && at + 1 < sizeof(session)) {
    session[at] = out[at];
    at++;
  }
  session[at] = '\0';
  assert(at == 16);

  wd_client queuer;
  assert(wd_client_connect(&queuer, sock) == 0);
  assert(wd_client_handshake(&queuer) == 0);
  cbor_item_t *created = wd_map(1);
  wd_put(created, "cwd", wd_text(root));
  cbor_item_t *created_result = NULL;
  assert(wd_client_call(&queuer, "session.create", created, &created_result) == 0);
  cbor_item_t *created_session = wd_get(created_result, "session");
  char *queued_session = wd_text_copy(wd_get(created_session, "id"));
  assert(queued_session != NULL);
  wd_free(created_result);
  cbor_item_t *first = wd_map(2);
  wd_put(first, "id", wd_text(queued_session));
  wd_put(first, "text", wd_text("first prompt"));
  cbor_item_t *first_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", first, &first_result) == 0);
  wd_free(first_result);
  cbor_item_t *second = wd_map(2);
  wd_put(second, "id", wd_text(queued_session));
  wd_put(second, "text", wd_text("second prompt"));
  cbor_item_t *second_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", second, &second_result) == 0);
  bool queued = false;
  uint64_t ahead = 99;
  assert(wd_bool_of(wd_get(second_result, "queued"), &queued));
  assert(queued);
  assert(wd_uint_of(wd_get(second_result, "ahead"), &ahead));
  assert(ahead == 0);
  wd_free(second_result);
  free(queued_session);
  close(queuer.fd);

  char *prompt_argv[] = {(char *)bin, "prompt", session, "native hello", NULL};
  int prompt_rc = run_capture(sock, bin, prompt_argv, out, sizeof(out));
  if (prompt_rc != 0)
    fprintf(stderr, "prompt rc=%d daemon_alive=%d\n", prompt_rc,
            kill(pid, 0) == 0 ? 1 : 0);
  assert(prompt_rc == 0);

  char note_path[256];
  snprintf(note_path, sizeof(note_path), "%s/note.txt", root);
  FILE *note = fopen(note_path, "w");
  assert(note != NULL);
  assert(fputs("desktop fixture\n", note) >= 0);
  fclose(note);
  wd_client reader;
  assert(wd_client_connect(&reader, sock) == 0);
  assert(wd_client_handshake(&reader) == 0);
  cbor_item_t *read_params = wd_map(2);
  wd_put(read_params, "id", wd_text(session));
  wd_put(read_params, "path", wd_text("note.txt"));
  cbor_item_t *read_result = NULL;
  assert(wd_client_call(&reader, "session.file_read", read_params,
                        &read_result) == 0);
  char *read_text = wd_text_copy(wd_get(read_result, "text"));
  assert(read_text != NULL && strstr(read_text, "desktop fixture") != NULL);
  free(read_text);
  wd_free(read_result);
  cbor_item_t *escape_params = wd_map(2);
  wd_put(escape_params, "id", wd_text(session));
  wd_put(escape_params, "path", wd_text("../outside.txt"));
  cbor_item_t *escape_result = NULL;
  assert(wd_client_call(&reader, "session.file_read", escape_params,
                        &escape_result) != 0);
  close(reader.fd);

  char *resume_argv[] = {(char *)bin, "resume", session, NULL};
  int resume_rc = run_capture(sock, bin, resume_argv, out, sizeof(out));
  if (resume_rc != 0) fprintf(stderr, "resume rc=%d out=%s\n", resume_rc, out);
  assert(resume_rc == 0);
  assert(strstr(out, "native hello") != NULL);

  char tcp_out[8192];
  char *tcp_create[] = {(char *)bin, "create", root, NULL};
  assert(run_capture(tcp_url, bin, tcp_create, tcp_out, sizeof(tcp_out)) == 0);
  assert(strlen(tcp_out) > 16);

  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  stop_server(endpoint);
  drop_test_cgroup(getenv("WARD_CGROUP"));

  printf("native gate passed: ward-native drove a session over the socket\n");
  return 0;
}
