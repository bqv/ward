/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HARNESS_CORE_UTIL_H
#define HARNESS_CORE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int wu_mkdir_p(const char *path, mode_t mode);
int wu_write_all(int fd, const void *buf, size_t len);
int wu_fsync_dir(const char *path);
char *wu_path_join(const char *a, const char *b);
char *wu_default_state_dir(void);
char *wu_default_socket_path(void);
char *wu_boot_id(void);
uint64_t wu_now_ms(void);
bool wu_pid_alive(long pid);
int wu_random_bytes(void *buf, size_t len);

#endif
