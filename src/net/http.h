/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_NET_HTTP_H
#define WARD_NET_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  long status;
  uint8_t *body;
  size_t len;
} wd_http_response;

int wd_http_request(const char *url, const char *method, const char *body,
                    const char *api_key, uint64_t timeout_ms, size_t max_bytes,
                    wd_http_response *out, char **err);
void wd_http_response_free(wd_http_response *response);

typedef struct wd_http_job wd_http_job;

wd_http_job *wd_http_start(const char *url, const char *method, const char *body,
                           const char *api_key, uint64_t timeout_ms,
                           size_t max_bytes, char **err);
int wd_http_job_fd(wd_http_job *job);
int wd_http_job_finish(wd_http_job *job, wd_http_response *out, char **err);
void wd_http_job_free(wd_http_job *job);
bool wd_http_loopback(const char *url);
char *wd_http_auth_header(const char *url, const char *api_key);

#endif
