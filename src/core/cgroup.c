/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/cgroup.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_text(const char *path, const char *text, char **err) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err != NULL) {
      char buf[512];
      snprintf(buf, sizeof(buf), "cannot open %s: %s", path, strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  size_t len = strlen(text);
  ssize_t written = write(fd, text, len);
  close(fd);
  if (written != (ssize_t)len) {
    if (err != NULL) {
      char buf[512];
      snprintf(buf, sizeof(buf), "cannot write %s: %s", path, strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  return 0;
}

static int read_small(const char *path, char *out, size_t cap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t got = read(fd, out, cap - 1);
  close(fd);
  if (got < 0) return -1;
  out[got] = '\0';
  return 0;
}

static char saved_origin[1024];
static char saved_leaf[1024];
static char saved_base[1024];

static int ensure_dir(const char *path) {
  char partial[1024];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(partial)) return -1;
  memcpy(partial, path, len + 1);
  for (size_t i = 1; i <= len; i++) {
    if (partial[i] != '/' && partial[i] != '\0') continue;
    char saved = partial[i];
    partial[i] = '\0';
    if (mkdir(partial, 0755) != 0 && errno != EEXIST) return -1;
    char control[1200];
    snprintf(control, sizeof(control), "%s/cgroup.subtree_control", partial);
    if (write_text(control, "+memory +pids\n", NULL) != 0)
      (void)write_text(control, "+memory\n", NULL);
    partial[i] = saved;
  }
  return 0;
}

static int write_through_root(const char *path, const char *text) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[1]);
    dup2(fds[0], 0);
    close(fds[0]);
    int quiet = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (quiet >= 0) {
      dup2(quiet, 1);
      close(quiet);
    }
    execl("/usr/bin/sudo", "sudo", "-n", "tee", path, (char *)NULL);
    _exit(127);
  }
  close(fds[0]);
  size_t len = strlen(text);
  ssize_t written = write(fds[1], text, len);
  close(fds[1]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  if (written != (ssize_t)len) return -1;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int move_pid(const char *procs_path, const char *value, char **err) {
  if (write_text(procs_path, value, NULL) == 0) return 0;
  const char *allow = getenv("WARD_CGROUP_SUDO");
  if (allow != NULL && strcmp(allow, "0") == 0) {
    if (err != NULL) *err = strdup("the cgroup move was refused by the kernel");
    return -1;
  }
  if (write_through_root(procs_path, value) == 0) return 0;
  if (err != NULL)
    *err = strdup(
        "cannot move into the ward cgroup: the common ancestor with the "
        "current cgroup is not writable, and the privileged fallback failed");
  return -1;
}

static int fence_has_processes(const char *path) {
  char text[4096];
  if (read_small(path, text, sizeof(text)) != 0) return 0;
  for (size_t i = 0; text[i] != '\0'; i++) {
    if (text[i] >= '0' && text[i] <= '9') return 1;
  }
  return 0;
}

static int supervisor_inside(const char *base) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)getppid());
  char text[1024];
  if (read_small(path, text, sizeof(text)) != 0) return 0;
  char *colon = strchr(text, ':');
  if (colon == NULL) return 0;
  char *slash = strchr(colon + 1, '/');
  if (slash == NULL) return 0;
  size_t base_len = strlen(base);
  if (base_len == 0) return 0;
  return strncmp(slash, base, base_len) == 0 &&
         (slash[base_len] == '/' || slash[base_len] == '\n' ||
          slash[base_len] == '\0');
}

void wd_cgroup_leave(void) {
  if (saved_leaf[0] == '\0') return;
  char origin_procs[1200];
  snprintf(origin_procs, sizeof(origin_procs), "/sys/fs/cgroup%s/cgroup.procs",
           saved_origin);
  char value[32];
  snprintf(value, sizeof(value), "%d\n", (int)getpid());
  if (move_pid(origin_procs, value, NULL) == 0) {
    rmdir(saved_leaf);
    rmdir(saved_base);
  }
  saved_leaf[0] = '\0';
  saved_base[0] = '\0';
}

int wd_cgroup_enter(const wd_cgroup_config *config, char **err) {
  if (config == NULL || config->base == NULL || config->base[0] == '\0')
    return 0;
  const char *leaf = config->leaf != NULL ? config->leaf : "harness";
  if (ensure_dir(config->base) != 0) {
    if (err != NULL) {
      char buf[1200];
      snprintf(buf, sizeof(buf), "cannot create %s: %s", config->base,
               strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  char origin_path[64];
  snprintf(origin_path, sizeof(origin_path), "/proc/%d/cgroup", (int)getpid());
  saved_origin[0] = '\0';
  if (read_small(origin_path, saved_origin, sizeof(saved_origin)) == 0) {
    char *slash = strchr(saved_origin, '/');
    if (slash != NULL) memmove(saved_origin, slash, strlen(slash) + 1);
    size_t len = strlen(saved_origin);
    while (len > 0 && (saved_origin[len - 1] == '\n' ||
                       saved_origin[len - 1] == '\r'))
      saved_origin[--len] = '\0';
  }
  snprintf(saved_base, sizeof(saved_base), "%s", config->base);
  char fence_procs[1024];
  snprintf(fence_procs, sizeof(fence_procs), "%s/cgroup.procs", config->base);
  if (supervisor_inside(config->base)) {
    if (err != NULL)
      *err = strdup(
          "the supervisor sits inside the fence; start supervise-daemon "
          "outside the ward cgroup");
    return -1;
  }
  if (fence_has_processes(fence_procs)) {
    if (err != NULL)
      *err = strdup(
          "the fence holds processes; a cgroup with children must be empty");
    return -1;
  }
  char leaf_path[1024];
  snprintf(leaf_path, sizeof(leaf_path), "%s/%s", config->base, leaf);
  snprintf(saved_leaf, sizeof(saved_leaf), "%s", leaf_path);
  if (mkdir(leaf_path, 0755) != 0 && errno != EEXIST) {
    if (err != NULL) {
      char buf[1200];
      snprintf(buf, sizeof(buf), "cannot create %s: %s", leaf_path,
               strerror(errno));
      *err = strdup(buf);
    }
    return -1;
  }
  char file[1200];
  char value[64];
  if (config->max_bytes > 0) {
    snprintf(file, sizeof(file), "%s/memory.max", leaf_path);
    snprintf(value, sizeof(value), "%lld\n", config->max_bytes);
    if (write_text(file, value, err) != 0) return -1;
    snprintf(file, sizeof(file), "%s/memory.swap.max", leaf_path);
    if (write_text(file, "0\n", NULL) != 0) {
      snprintf(file, sizeof(file), "%s/memory.swap.max", leaf_path);
      (void)write_text(file, "0\n", NULL);
    }
  }
  if (config->high_bytes > 0) {
    snprintf(file, sizeof(file), "%s/memory.high", leaf_path);
    snprintf(value, sizeof(value), "%lld\n", config->high_bytes);
    if (write_text(file, value, err) != 0) return -1;
  }
  if (config->pids_max > 0) {
    snprintf(file, sizeof(file), "%s/pids.max", leaf_path);
    snprintf(value, sizeof(value), "%lld\n", config->pids_max);
    if (write_text(file, value, NULL) != 0) {
      snprintf(file, sizeof(file), "%s/pids.max", leaf_path);
      (void)write_text(file, value, NULL);
    }
  }
  snprintf(file, sizeof(file), "%s/cgroup.procs", leaf_path);
  snprintf(value, sizeof(value), "%d\n", (int)getpid());
  if (move_pid(file, value, err) != 0) return -1;
  char self_path[64];
  snprintf(self_path, sizeof(self_path), "/proc/%d/cgroup", (int)getpid());
  char now[1024];
  if (read_small(self_path, now, sizeof(now)) != 0) return 0;
  const char *relative = leaf_path;
  const char *mount = "/sys/fs/cgroup";
  size_t mount_len = strlen(mount);
  if (strncmp(leaf_path, mount, mount_len) == 0) relative = leaf_path + mount_len;
  if (strstr(now, relative) == NULL) {
    if (err != NULL) *err = strdup("the process did not enter the leaf");
    return -1;
  }
  return 0;
}
