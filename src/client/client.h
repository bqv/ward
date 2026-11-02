/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_CLIENT_CLIENT_H
#define WARD_CLIENT_CLIENT_H

#include <stdint.h>

#include <cbor.h>

#include "proto/frame.h"

typedef struct {
  int fd;
  uint64_t next_id;
} wd_client;

int wd_client_connect(wd_client *client, const char *path);
int wd_client_handshake(wd_client *client);
int wd_client_send_item(wd_client *client, cbor_item_t *item);
uint8_t *wd_client_read_frame(int fd, wd_frame *frame);
int wd_client_call(wd_client *client, const char *method, cbor_item_t *params,
                   cbor_item_t **result);

#endif
