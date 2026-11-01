/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char **clean_environment(void) {
  static char *const env[] = {"PATH=/usr/local/bin:/usr/bin:/bin",
                              "LANG=C.UTF-8", "LC_ALL=C.UTF-8", NULL};
  return (char **)env;
}

void wd_proc_result_free(wd_proc_result *result) {
  if (result == NULL) return;
  free(result->out);
  result->out = NULL;
  result->len = 0;
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

static int spawn(const char *const argv[], const char *cwd, int *pipe_out) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    (void)setsid();
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
      _exit(126);
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO) close(fds[1]);
    if (cwd != NULL && chdir(cwd) != 0) _exit(126);
    exec_command(argv);
    _exit(127);
  }
  close(fds[1]);
  *pipe_out = fds[0];
  return (int)pid;
}

static int drain(int pid, int fd, uint64_t timeout_ms, size_t max_bytes,
                wd_proc_result *result) {
  size_t cap = max_bytes < 4096 ? 4096 : max_bytes + 1;
  uint8_t *buf = malloc(cap);
  if (buf == NULL) {
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
    close(fd);
    return -1;
  }
  result->len = 0;
  uint64_t deadline = 0;
  if (timeout_ms > 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u +
               timeout_ms;
  }
  bool killed = false;
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  for (;;) {
    if (deadline != 0 && !killed) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t now_ms =
          (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
      if (now_ms >= deadline) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        killed = true;
        result->timed_out = true;
      }
    }
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int ready = poll(&pfd, 1, 20);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      if (killed) break;
      continue;
    }
    ssize_t n = read(fd, buf + result->len, cap - result->len - 1);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      break;
    }
    if (n == 0) break;
    result->len += (size_t)n;
    if (result->len + 1 >= cap) {
      result->truncated = true;
      (void)kill(-pid, SIGKILL);
      (void)kill(pid, SIGKILL);
      killed = true;
      break;
    }
  }
  close(fd);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) break;
  }
  result->status = status;
  buf[result->len] = '\0';
  result->out = buf;
  return 0;
}

static int spawn_with_input(const char *const argv[], const char *cwd,
                          int stdin_fd, int *pipe_out) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    (void)setsid();
    close(fds[0]);
    if (dup2(stdin_fd, STDIN_FILENO) < 0 ||
        dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
      _exit(126);
    if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO) close(fds[1]);
    if (cwd != NULL && chdir(cwd) != 0) _exit(126);
    exec_command(argv);
    _exit(127);
  }
  close(fds[1]);
  *pipe_out = fds[0];
  return (int)pid;
}

int wd_proc_run(const char *const argv[], const char *cwd, uint64_t timeout_ms,
                size_t max_bytes, wd_proc_result *result, char **err) {
  if (argv == NULL || argv[0] == NULL) {
    if (err != NULL) *err = strdup("no command");
    return -1;
  }
  memset(result, 0, sizeof(*result));
  int fd = -1;
  int pid = spawn(argv, cwd, &fd);
  if (pid < 0) {
    if (err != NULL) *err = strdup("cannot start the command");
    return -1;
  }
  return drain(pid, fd, timeout_ms, max_bytes, result);
}

int wd_proc_run_input(const char *const argv[], const char *cwd,
                      const void *input, size_t input_len, uint64_t timeout_ms,
                      size_t max_bytes, wd_proc_result *result, char **err) {
  if (argv == NULL || argv[0] == NULL) {
    if (err != NULL) *err = strdup("no command");
    return -1;
  }
  memset(result, 0, sizeof(*result));
  int in[2];
  if (pipe2(in, O_CLOEXEC) != 0) {
    if (err != NULL) *err = strdup("cannot create the input pipe");
    return -1;
  }
  int out = -1;
  int pid = spawn_with_input(argv, cwd, in[0], &out);
  close(in[0]);
  if (pid < 0) {
    close(in[1]);
    if (err != NULL) *err = strdup("cannot start the command");
    return -1;
  }
  size_t written = 0;
  while (written < input_len) {
    ssize_t n = write(in[1], (const uint8_t *)input + written, input_len - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    written += (size_t)n;
  }
  close(in[1]);
  return drain(pid, out, timeout_ms, max_bytes, result);
}
