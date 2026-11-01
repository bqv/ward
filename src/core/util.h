/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_UTIL_H
#define WARD_CORE_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int wd_mkdir_p(const char *path, mode_t mode);
int wd_write_all(int fd, const void *buf, size_t len);
int wd_fsync_dir(const char *path);
char *wd_path_join(const char *a, const char *b);
char *wd_default_state_dir(void);
char *wd_default_socket_path(void);
char *wd_boot_id(void);
uint64_t wd_now_ms(void);
void wd_now_text(char *buf, size_t cap);
bool wd_pid_alive(long pid);
int wd_random_bytes(void *buf, size_t len);

#endif
