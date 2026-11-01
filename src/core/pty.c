/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

struct wd_pty {
  int master;
  pid_t pid;
};

static char **clean_environment(void) {
  static char *const env[] = {"PATH=/usr/local/bin:/usr/bin:/bin",
                              "TERM=xterm-256color", "LANG=C.UTF-8",
                              "LC_ALL=C.UTF-8", NULL};
  return (char **)env;
}

static void exec_command(const char *const argv[]) {
  if (strchr(argv[0], '/') != NULL) {
    execve(argv[0], (char *const *)argv, clean_environment());
    return;
  }
  static const char *const dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
  char path[4096];
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    if (snprintf(path, sizeof(path), "%s/%s", dirs[i], argv[0]) >=
        (int)sizeof(path))
      continue;
    execve(path, (char *const *)argv, clean_environment());
  }
}

wd_pty *wd_pty_open(const char *const argv[], const char *cwd, uint16_t rows,
                    uint16_t cols, char **err) {
  if (argv == NULL || argv[0] == NULL) {
    if (err != NULL) *err = strdup("no command");
    return NULL;
  }
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = rows == 0 ? 24 : rows;
  ws.ws_col = cols == 0 ? 80 : cols;
  int master = -1;
  pid_t pid = forkpty(&master, NULL, NULL, &ws);
  if (pid < 0) {
    if (err != NULL) *err = strdup("cannot open a pty");
    return NULL;
  }
  if (pid == 0) {
    (void)setsid();
    if (cwd != NULL && chdir(cwd) != 0) _exit(126);
    exec_command(argv);
    _exit(127);
  }
  int flags = fcntl(master, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(master, F_SETFL, flags | O_NONBLOCK);
  wd_pty *pty = calloc(1, sizeof(*pty));
  if (pty == NULL) {
    (void)kill(pid, SIGKILL);
    close(master);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  pty->master = master;
  pty->pid = pid;
  return pty;
}

int wd_pty_fd(const wd_pty *pty) { return pty != NULL ? pty->master : -1; }

pid_t wd_pty_pid(const wd_pty *pty) { return pty != NULL ? pty->pid : 0; }

bool wd_pty_alive(const wd_pty *pty) {
  if (pty == NULL) return false;
  int status = 0;
  pid_t rc = waitpid(pty->pid, &status, WNOHANG);
  if (rc == 0) return true;
  return false;
}

ssize_t wd_pty_write(wd_pty *pty, const void *data, size_t len) {
  if (pty == NULL) return -1;
  return write(pty->master, data, len);
}

int wd_pty_resize(wd_pty *pty, uint16_t rows, uint16_t cols) {
  if (pty == NULL) return -1;
  struct winsize ws;
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = rows == 0 ? 24 : rows;
  ws.ws_col = cols == 0 ? 80 : cols;
  return ioctl(pty->master, TIOCSWINSZ, &ws) == 0 ? 0 : -1;
}

ssize_t wd_pty_read(wd_pty *pty, char *out, size_t cap) {
  if (pty == NULL || cap == 0) return -1;
  for (;;) {
    ssize_t n = read(pty->master, out, cap);
    if (n < 0 && errno == EINTR) continue;
    return n;
  }
}

void wd_pty_close(wd_pty *pty) {
  if (pty == NULL) return;
  (void)kill(-pty->pid, SIGHUP);
  (void)kill(pty->pid, SIGHUP);
  close(pty->master);
  int status = 0;
  for (int i = 0; i < 100; i++) {
    pid_t rc = waitpid(pty->pid, &status, WNOHANG);
    if (rc != 0) break;
    usleep(1000);
  }
  (void)kill(-pty->pid, SIGKILL);
  (void)kill(pty->pid, SIGKILL);
  (void)waitpid(pty->pid, &status, 0);
  free(pty);
}
