/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char *text) {
  size_t len = strlen(text);
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, text + off, len - off);
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int read_line(int fd, char *out, size_t cap) {
  size_t used = 0;
  while (used + 1 < cap) {
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, 5000) <= 0) return -1;
    char c = 0;
    ssize_t n = read(fd, &c, 1);
    if (n <= 0) return -1;
    if (c == '\n') break;
    out[used++] = c;
  }
  out[used] = '\0';
  return 0;
}

static void session_id(const char *line, char *out, size_t cap) {
  const char *at = strstr(line, "\"sessionId\":\"");
  assert(at != NULL);
  at += strlen("\"sessionId\":\"");
  size_t n = 0;
  while (*at != '"' && n + 1 < cap) out[n++] = *at++;
  out[n] = '\0';
}

int main(void) {
  const char *modules = getenv("WARD_MODULE_DIR");
  const char *acp = getenv("WARD_ACP");
  const char *daemon = getenv("WARD_BIN");
  assert(modules != NULL && acp != NULL && daemon != NULL);

  char root[] = "/tmp/ward-acp-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    char cgroup[128];
    const char *override = getenv("WARD_GATE_CGROUP");
    if (override != NULL)
      snprintf(cgroup, sizeof(cgroup), "%s", override);
    else
      snprintf(cgroup, sizeof(cgroup), "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
    setenv("WARD_CGROUP", cgroup, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(daemon, "ward", "up", "--socket", sock, "--state", state, (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[512];
  bool up = false;
  size_t used = 0;
  while (!up) {
    ssize_t n = read(ready[0], line + used, 1);
    if (n <= 0) break;
    if (line[used++] != '\n') continue;
    line[used] = '\0';
    if (strncmp(line, "ready ", 6) == 0) up = true;
    used = 0;
  }
  close(ready[0]);
  assert(up);

  int to_acp[2];
  int from_acp[2];
  assert(pipe(to_acp) == 0);
  assert(pipe(from_acp) == 0);
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    alarm(30);
    setenv("WARD_SOCKET", sock, 1);
    dup2(to_acp[0], 0);
    dup2(from_acp[1], 1);
    close(to_acp[0]);
    close(to_acp[1]);
    close(from_acp[0]);
    close(from_acp[1]);
    execl(acp, "ward-acp", (char *)NULL);
    _exit(127);
  }
  close(to_acp[0]);
  close(from_acp[1]);

  char reply[4096];
  assert(write_all(to_acp[1],
                   "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                   "\"params\":{\"protocolVersion\":1}}\n") == 0);
  assert(read_line(from_acp[0], reply, sizeof(reply)) == 0);
  assert(strstr(reply, "\"protocolVersion\":1") != NULL);

  assert(write_all(to_acp[1],
                   "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/new\","
                   "\"params\":{\"cwd\":\"/tmp\",\"mcpServers\":[]}}\n") == 0);
  assert(read_line(from_acp[0], reply, sizeof(reply)) == 0);
  char sid[128];
  session_id(reply, sid, sizeof(sid));

  char prompt[512];
  snprintf(prompt, sizeof(prompt),
           "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"session/prompt\","
           "\"params\":{\"sessionId\":\"%s\",\"prompt\":[{\"type\":\"text\","
           "\"text\":\"/help\"}]}}\n",
           sid);
  assert(write_all(to_acp[1], prompt) == 0);

  bool update = false;
  bool stop = false;
  for (int i = 0; i < 20; i++) {
    assert(read_line(from_acp[0], reply, sizeof(reply)) == 0);
    if (strstr(reply, "agent_message_chunk") != NULL) update = true;
    if (strstr(reply, "stopReason") != NULL) {
      stop = true;
      break;
    }
  }
  assert(update);
  assert(stop);

  close(to_acp[1]);
  kill(child, SIGTERM);
  int status = 0;
  waitpid(child, &status, 0);
  kill(pid, SIGTERM);
  waitpid(pid, &status, 0);
  printf("acp gate passed: initialize, session/new and one session/update\n");
  return 0;
}
