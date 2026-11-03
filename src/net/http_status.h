/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_NET_HTTP_STATUS_H
#define WARD_NET_HTTP_STATUS_H

#include <stddef.h>
#include <string.h>

static inline long wd_http_status_line(const char *data, size_t n) {
  if (n <= 5 || strncmp(data, "HTTP/", 5) != 0) return 0;
  const char *space = memchr(data, ' ', n);
  if (space == NULL) return 0;
  long status = 0;
  for (const char *p = space + 1; p < data + n && *p >= '0' && *p <= '9'; p++)
    status = status * 10 + (*p - '0');
  return status;
}

#endif
