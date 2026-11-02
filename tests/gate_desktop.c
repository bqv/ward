/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fake_endpoint.h"

static int run_capture(const char *socket, const char *bin, char *const argv[],
                       char *out, size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    alarm(20);
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

static int count_text(const char *haystack, const char *needle) {
  int count = 0;
  const char *at = haystack;
  size_t len = strlen(needle);
  while ((at = strstr(at, needle)) != NULL) {
    count++;
    at += len;
  }
  return count;
}

int main(void) {
  const char *modules = getenv("WARD_MODULE_DIR");
  const char *probe = getenv("WARD_GUI_PROBE");
  const char *daemon = getenv("WARD_BIN");
  assert(modules != NULL && probe != NULL && daemon != NULL);

  char root[] = "/tmp/ward-desktop-gate-XXXXXX";
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

  char cgroup_base[128];
  const char *override = getenv("WARD_GATE_CGROUP");
  if (override != NULL) {
    snprintf(cgroup_base, sizeof(cgroup_base), "%s", override);
  } else {
    snprintf(cgroup_base, sizeof(cgroup_base),
             "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
  }

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    setenv("WARD_CGROUP", cgroup_base, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
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
      while (len > 0 && tcp_url[len - 1] == '\n') tcp_url[--len] = '\0';
    }
    if (strncmp(line, "ready ", 6) == 0) ready_seen = true;
    used = 0;
  }
  close(ready[0]);
  assert(ready_seen);
  assert(tcp_url[0] != '\0');

  char out[8192];
  char *prompt_argv[] = {(char *)probe, (char *)"--prompt", (char *)"hello desktop", NULL};
  assert(run_capture(sock, probe, prompt_argv, out, sizeof(out)) == 0);
  assert(count_text(out, "hello desktop") == 1);

  char tcp_out[8192];
  char *tcp_argv[] = {(char *)probe, (char *)"--prompt", (char *)"hello tcp", NULL};
  assert(run_capture(tcp_url, probe, tcp_argv, tcp_out, sizeof(tcp_out)) == 0);
  assert(count_text(tcp_out, "hello tcp") == 1);

  char terminal_out[8192];
  char *terminal_argv[] = {(char *)probe, (char *)"--terminal", NULL};
  assert(run_capture(sock, probe, terminal_argv, terminal_out, sizeof(terminal_out)) == 0);
  assert(count_text(terminal_out, "ward-terminal-ok") >= 1);

  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  stop_server(endpoint);
  if (strcmp(cgroup_base, "off") != 0) drop_test_cgroup(cgroup_base);

  printf("desktop gate passed: ward-gui drove a session over UDS, TCP and a terminal\n");
  return 0;
}
