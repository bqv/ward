/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CORE_PTY_H
#define WARD_CORE_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct wd_pty wd_pty;

wd_pty *wd_pty_open(const char *const argv[], const char *cwd, uint16_t rows,
                    uint16_t cols, char **err);
int wd_pty_fd(const wd_pty *pty);
pid_t wd_pty_pid(const wd_pty *pty);
bool wd_pty_alive(const wd_pty *pty);
ssize_t wd_pty_write(wd_pty *pty, const void *data, size_t len);
int wd_pty_resize(wd_pty *pty, uint16_t rows, uint16_t cols);
ssize_t wd_pty_read(wd_pty *pty, char *out, size_t cap);
void wd_pty_close(wd_pty *pty);

#endif
