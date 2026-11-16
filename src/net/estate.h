/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_NET_ESTATE_H
#define WARD_NET_ESTATE_H

#include <stddef.h>
#include <stdint.h>

#define WD_ESTATE_HOLDERS 4
#define WD_ESTATE_SLOTS 8
#define WD_ESTATE_NAME 32
#define WD_ESTATE_GROUP 16

typedef struct {
  char name[WD_ESTATE_NAME];
  char state[WD_ESTATE_NAME];
} wd_estate_slot;

typedef struct {
  char group[WD_ESTATE_GROUP];
  char holders[WD_ESTATE_HOLDERS][WD_ESTATE_NAME];
  size_t holders_len;
  uint64_t since_ms;
  wd_estate_slot slots[WD_ESTATE_SLOTS];
  size_t slots_len;
} wd_estate_card;

int wd_estate_card_read(const char *socket_path, const char *group,
                        wd_estate_card *out, char **err);
int wd_estate_status_read(const char *url, char **json, size_t *len,
                          char **err);
int wd_estate_parse(const char *json, wd_estate_card *out);

#endif
