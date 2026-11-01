/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/worker.h"

int main(void) {
  char root[] = "/tmp/ward-worker-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char *err = NULL;
  wd_worker *worker = wd_worker_new(root, &err);
  assert(worker != NULL);
  assert(wd_worker_pid(worker) > 0);
  assert(wd_worker_alive(worker));

  const char *echo_argv[] = {"echo", "hello worker", NULL};
  wd_proc_result result;
  assert(wd_worker_run(worker, echo_argv, 5000, 4096, &result, &err) == 0);
  assert(WIFEXITED(result.status));
  assert(WEXITSTATUS(result.status) == 0);
  assert(result.len == 13);
  assert(strcmp((char *)result.out, "hello worker\n") == 0);
  wd_proc_result_free(&result);

  const char *pwd_argv[] = {"pwd", NULL};
  assert(wd_worker_run(worker, pwd_argv, 5000, 4096, &result, &err) == 0);
  assert(WEXITSTATUS(result.status) == 0);
  assert(strncmp((char *)result.out, root, strlen(root)) == 0);
  wd_proc_result_free(&result);

  const char *missing_argv[] = {"/nonexistent/program", NULL};
  assert(wd_worker_run(worker, missing_argv, 5000, 4096, &result, &err) == 0);
  assert(WIFEXITED(result.status));
  assert(WEXITSTATUS(result.status) == 127);
  wd_proc_result_free(&result);

  const char *slow_argv[] = {"sleep", "5", NULL};
  assert(wd_worker_run(worker, slow_argv, 200, 4096, &result, &err) == 0);
  assert(result.timed_out);
  wd_proc_result_free(&result);

  assert(wd_worker_alive(worker));
  pid_t pid = wd_worker_pid(worker);
  assert(kill(pid, SIGKILL) == 0);
  while (waitpid(pid, NULL, 0) < 0) {
  }
  assert(!wd_worker_alive(worker));
  assert(wd_worker_run(worker, echo_argv, 5000, 4096, &result, &err) != 0);
  assert(err != NULL);
  free(err);
  wd_worker_free(worker);

  wd_worker *second = wd_worker_new(root, NULL);
  assert(second != NULL);
  assert(wd_worker_run(second, echo_argv, 5000, 4096, &result, NULL) == 0);
  assert(strcmp((char *)result.out, "hello worker\n") == 0);
  wd_proc_result_free(&result);
  wd_worker_free(second);

  rmdir(root);
  printf("worker tests passed\n");
  return 0;
}
