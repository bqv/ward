/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct wd_worker {
  pid_t pid;
  int fd;
  bool dead;
};

static int write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static int read_all(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int write_u32(int fd, uint32_t value) {
  uint8_t bytes[4] = {(uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff),
                      (uint8_t)((value >> 16) & 0xff),
                      (uint8_t)((value >> 24) & 0xff)};
  return write_all(fd, bytes, sizeof(bytes));
}

static int read_u32(int fd, uint32_t *value) {
  uint8_t bytes[4];
  if (read_all(fd, bytes, sizeof(bytes)) != 0) return -1;
  *value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
  return 0;
}

static char **clean_environment(void) {
  static char *const env[] = {"PATH=/usr/local/bin:/usr/bin:/bin",
                              "LANG=C.UTF-8", "LC_ALL=C.UTF-8", NULL};
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

static int run_child(const char *const argv[], uint64_t timeout_ms,
                     size_t max_bytes, wd_proc_result *result) {
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
    exec_command(argv);
    _exit(127);
  }
  close(fds[1]);
  memset(result, 0, sizeof(*result));
  size_t cap = max_bytes < 4096 ? 4096 : max_bytes + 1;
  uint8_t *buf = malloc(cap);
  if (buf == NULL) {
    (void)kill(-pid, SIGKILL);
    (void)kill(pid, SIGKILL);
    close(fds[0]);
    waitpid(pid, NULL, 0);
    return -1;
  }
  uint64_t deadline = 0;
  if (timeout_ms > 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u +
               timeout_ms;
  }
  int flags = fcntl(fds[0], F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  bool killed = false;
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
    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    int ready = poll(&pfd, 1, 20);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0) {
      if (killed) break;
      continue;
    }
    ssize_t n = read(fds[0], buf + result->len, cap - result->len - 1);
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
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) break;
  }
  result->status = status;
  buf[result->len] = '\0';
  result->out = buf;
  return 0;
}

static void worker_serve(int fd) {
  for (;;) {
    uint32_t argc = 0;
    if (read_u32(fd, &argc) != 0) break;
    if (argc == 0 || argc > 64) break;
    char *argv[65];
    char *storage[65];
    uint32_t i = 0;
    int bad = 0;
    for (i = 0; i < argc; i++) {
      uint32_t len = 0;
      if (read_u32(fd, &len) != 0 || len > 65536) {
        bad = 1;
        break;
      }
      storage[i] = malloc(len + 1);
      if (storage[i] == NULL || read_all(fd, storage[i], len) != 0) {
        bad = 1;
        break;
      }
      storage[i][len] = '\0';
      argv[i] = storage[i];
    }
    argv[argc] = NULL;
    uint32_t timeout = 0;
    uint32_t limit = 0;
    if (!bad && (read_u32(fd, &timeout) != 0 || read_u32(fd, &limit) != 0))
      bad = 1;
    if (bad) {
      for (uint32_t j = 0; j < i; j++) free(storage[j]);
      break;
    }
    wd_proc_result result;
    int rc = run_child((const char *const *)argv, timeout, limit, &result);
    for (uint32_t j = 0; j < argc; j++) free(storage[j]);
    if (rc != 0) {
      uint32_t marker = 0xffffffffu;
      if (write_u32(fd, marker) != 0) break;
      continue;
    }
    uint32_t flags_out =
        (result.timed_out ? 1u : 0u) | (result.truncated ? 2u : 0u);
    if (write_u32(fd, (uint32_t)result.status) != 0 ||
        write_u32(fd, flags_out) != 0 ||
        write_u32(fd, (uint32_t)result.len) != 0 ||
        (result.len > 0 && write_all(fd, result.out, result.len) != 0)) {
      wd_proc_result_free(&result);
      break;
    }
    wd_proc_result_free(&result);
  }
}

wd_worker *wd_worker_new(const char *root, char **err) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
    if (err != NULL) *err = strdup("cannot create a worker socket");
    return NULL;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    if (err != NULL) *err = strdup("cannot fork a worker");
    return NULL;
  }
  if (pid == 0) {
    close(fds[0]);
    (void)setsid();
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (root != NULL && chdir(root) != 0) _exit(126);
    worker_serve(fds[1]);
    _exit(0);
  }
  close(fds[1]);
  wd_worker *worker = calloc(1, sizeof(*worker));
  if (worker == NULL) {
    (void)kill(pid, SIGKILL);
    close(fds[0]);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  worker->pid = pid;
  worker->fd = fds[0];
  return worker;
}

void wd_worker_free(wd_worker *worker) {
  if (worker == NULL) return;
  if (worker->fd >= 0) close(worker->fd);
  if (worker->pid > 0) {
    (void)kill(worker->pid, SIGKILL);
    while (waitpid(worker->pid, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  free(worker);
}

pid_t wd_worker_pid(const wd_worker *worker) {
  return worker != NULL ? worker->pid : -1;
}

bool wd_worker_alive(const wd_worker *worker) {
  if (worker == NULL || worker->dead) return false;
  return kill(worker->pid, 0) == 0;
}

int wd_worker_submit(wd_worker *worker, const char *const argv[],
                     uint64_t timeout_ms, size_t max_bytes, char **err) {
  if (worker == NULL || worker->dead) {
    if (err != NULL) *err = strdup("the worker is gone");
    return -1;
  }
  size_t argc = 0;
  while (argv[argc] != NULL) argc++;
  if (argc == 0 || argc > 64) {
    if (err != NULL) *err = strdup("the command has no arguments");
    return -1;
  }
  int failed = write_u32(worker->fd, (uint32_t)argc) != 0;
  for (size_t i = 0; i < argc && !failed; i++) {
    size_t len = strlen(argv[i]);
    if (write_u32(worker->fd, (uint32_t)len) != 0 ||
        write_all(worker->fd, argv[i], len) != 0)
      failed = 1;
  }
  if (!failed && (write_u32(worker->fd, (uint32_t)timeout_ms) != 0 ||
                  write_u32(worker->fd, (uint32_t)max_bytes) != 0))
    failed = 1;
  if (!failed) return 0;
  worker->dead = true;
  if (err != NULL) *err = strdup("the worker is gone");
  return -1;
}

int wd_worker_fd(const wd_worker *worker) {
  return worker != NULL ? worker->fd : -1;
}

void wd_worker_kill(wd_worker *worker) {
  if (worker == NULL || worker->dead) return;
  worker->dead = true;
  (void)kill(worker->pid, SIGKILL);
}

int wd_worker_complete(wd_worker *worker, size_t max_bytes,
                       wd_proc_result *out, char **err) {
  if (worker == NULL || worker->dead) {
    if (err != NULL) *err = strdup("the worker is gone");
    return -1;
  }
  uint32_t status = 0;
  if (read_u32(worker->fd, &status) != 0) goto gone;
  if (status == 0xffffffffu) {
    if (err != NULL) *err = strdup("the worker could not run the command");
    return -1;
  }
  uint32_t flags = 0;
  uint32_t len = 0;
  if (read_u32(worker->fd, &flags) != 0 || read_u32(worker->fd, &len) != 0)
    goto gone;
  if (len > max_bytes + 1) goto gone;
  memset(out, 0, sizeof(*out));
  out->status = (int)status;
  out->timed_out = (flags & 1u) != 0;
  out->truncated = (flags & 2u) != 0;
  if (len > 0) {
    out->out = malloc(len + 1);
    if (out->out == NULL) goto gone;
    if (read_all(worker->fd, out->out, len) != 0) {
      wd_proc_result_free(out);
      goto gone;
    }
    out->out[len] = '\0';
    out->len = len;
  }
  return 0;
gone:
  worker->dead = true;
  if (err != NULL) *err = strdup("the worker is gone");
  return -1;
}

int wd_worker_run(wd_worker *worker, const char *const argv[],
                  uint64_t timeout_ms, size_t max_bytes, wd_proc_result *out,
                  char **err) {
  if (wd_worker_submit(worker, argv, timeout_ms, max_bytes, err) != 0) return -1;
  return wd_worker_complete(worker, max_bytes, out, err);
}
