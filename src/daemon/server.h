/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef HARNESS_DAEMON_SERVER_H
#define HARNESS_DAEMON_SERVER_H

typedef struct {
  const char *socket_path;
  const char *state_dir;
} hx_up_config;

int hx_up_run(const hx_up_config *cfg);

#endif
