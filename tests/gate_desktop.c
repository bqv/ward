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

  const char *gui = getenv("WARD_GUI");
  if (gui != NULL) {
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    char gui_out[8192];
    char *gui_argv[] = {(char *)gui, (char *)"--transport", (char *)"ward",
                        (char *)"--prompt", (char *)"hello gui", NULL};
    assert(run_capture(sock, gui, gui_argv, gui_out, sizeof(gui_out)) == 0);
    assert(count_text(gui_out, "hello gui") == 1);
  }

  char terminal_out[8192];
  char *terminal_argv[] = {(char *)probe, (char *)"--terminal", NULL};
  assert(run_capture(sock, probe, terminal_argv, terminal_out, sizeof(terminal_out)) == 0);
  assert(count_text(terminal_out, "ward-terminal-ok") >= 1);

  char note[512];
  snprintf(note, sizeof(note), "%s/note.txt", root);
  FILE *note_file = fopen(note, "w");
  assert(note_file != NULL);
  assert(fputs("ward file read\n", note_file) >= 0);
  fclose(note_file);
  char subdir[512];
  snprintf(subdir, sizeof(subdir), "%s/subdir", root);
  assert(mkdir(subdir, 0755) == 0);
  assert(chdir(root) == 0);

  char read_out[8192];
  char *read_argv[] = {(char *)probe, (char *)"--read", (char *)"note.txt", NULL};
  assert(run_capture(sock, probe, read_argv, read_out, sizeof(read_out)) == 0);
  assert(strstr(read_out, "ward file read") != NULL);

  char escape_out[8192];
  char *escape_argv[] = {(char *)probe, (char *)"--read", (char *)"../escape.txt", NULL};
  assert(run_capture(sock, probe, escape_argv, escape_out, sizeof(escape_out)) == 0);
  assert(strstr(escape_out, "outside the session directory") != NULL);

  char blob_path[512];
  snprintf(blob_path, sizeof(blob_path), "%s/note.bin", root);
  FILE *blob_file = fopen(blob_path, "wb");
  assert(blob_file != NULL);
  assert(fwrite("ward-blob-ok\n", 1, 13, blob_file) == 13);
  fclose(blob_file);

  char blob_out[8192];
  char *blob_argv[] = {(char *)probe, (char *)"--blob", (char *)"note.bin",
                       (char *)"--blob-len", (char *)"64", NULL};
  assert(run_capture(sock, probe, blob_argv, blob_out, sizeof(blob_out)) == 0);
  assert(strstr(blob_out, "ward-blob-ok") != NULL);

  char blob_escape_out[8192];
  char *blob_escape_argv[] = {(char *)probe, (char *)"--blob", (char *)"../escape.bin",
                              (char *)"--blob-len", (char *)"64", NULL};
  assert(run_capture(sock, probe, blob_escape_argv, blob_escape_out,
                     sizeof(blob_escape_out)) == 0);
  assert(strstr(blob_escape_out, "outside the session directory") != NULL);

  char list_out[8192];
  char *list_argv[] = {(char *)probe, (char *)"--list", (char *)".", NULL};
  assert(run_capture(sock, probe, list_argv, list_out, sizeof(list_out)) == 0);
  assert(strstr(list_out, "note.txt") != NULL);
  assert(strstr(list_out, "subdir") != NULL);

  char list_escape_out[8192];
  char *list_escape_argv[] = {(char *)probe, (char *)"--list", (char *)"..", NULL};
  assert(run_capture(sock, probe, list_escape_argv, list_escape_out,
                     sizeof(list_escape_out)) == 0);
  assert(strstr(list_escape_out, "outside the session directory") != NULL);

  char preset_out[8192];
  char *preset_argv[] = {(char *)probe, (char *)"--preset", (char *)"reads-only", NULL};
  assert(run_capture(sock, probe, preset_argv, preset_out, sizeof(preset_out)) == 0);
  assert(strstr(preset_out, "reads-only") != NULL);

  char full_out[8192];
  char *full_argv[] = {(char *)probe, (char *)"--preset", (char *)"full-access",
                       (char *)"--confirm-danger", NULL};
  assert(run_capture(sock, probe, full_argv, full_out, sizeof(full_out)) == 0);
  assert(strstr(full_out, "full-access") != NULL);

  char write_out[8192];
  char *write_argv[] = {(char *)probe, (char *)"--prompt",
                        (char *)"/write free.txt hi", NULL};
  assert(run_capture(sock, probe, write_argv, write_out, sizeof(write_out)) == 0);
  char free_path[512];
  snprintf(free_path, sizeof(free_path), "%s/free.txt", root);
  assert(access(free_path, F_OK) == 0);

  char reads_out[8192];
  char *reads_argv[] = {(char *)probe, (char *)"--preset", (char *)"reads-only",
                        (char *)"--confirm-danger", NULL};
  assert(run_capture(sock, probe, reads_argv, reads_out, sizeof(reads_out)) == 0);
  assert(strstr(reads_out, "reads-only") != NULL);

  char blocked_out[8192];
  char *blocked_argv[] = {(char *)probe, (char *)"--prompt",
                          (char *)"/write blocked.txt hi", NULL};
  assert(run_capture(sock, probe, blocked_argv, blocked_out, sizeof(blocked_out)) == 0);
  char blocked_path[512];
  snprintf(blocked_path, sizeof(blocked_path), "%s/blocked.txt", root);
  assert(access(blocked_path, F_OK) != 0);

  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  stop_server(endpoint);
  if (strcmp(cgroup_base, "off") != 0) drop_test_cgroup(cgroup_base);

  printf("desktop gate passed: ward-gui drove a session over UDS, TCP, a terminal, reads, a directory list, presets and a headless prompt\n");
  return 0;
}
