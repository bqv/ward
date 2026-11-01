/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "net/http.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  wd_http_response *response;
  size_t max;
} sink;

static size_t write_cb(char *data, size_t size, size_t nmemb, void *userdata) {
  sink *s = userdata;
  size_t n = size * nmemb;
  if (s->response->len + n > s->max) return 0;
  uint8_t *grown = realloc(s->response->body, s->response->len + n + 1);
  if (grown == NULL) return 0;
  s->response->body = grown;
  memcpy(s->response->body + s->response->len, data, n);
  s->response->len += n;
  s->response->body[s->response->len] = '\0';
  return n;
}

bool wd_http_loopback(const char *url) {
  if (url == NULL) return false;
  const char *prefix = "http://127.0.0.1";
  size_t n = strlen(prefix);
  if (strncmp(url, prefix, n) == 0 && (url[n] == ':' || url[n] == '/'))
    return true;
  prefix = "http://localhost";
  n = strlen(prefix);
  return strncmp(url, prefix, n) == 0 && (url[n] == ':' || url[n] == '/');
}

int wd_http_request(const char *url, const char *method, const char *body,
                    uint64_t timeout_ms, size_t max_bytes,
                    wd_http_response *out, char **err) {
  static bool started = false;
  if (!started) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
      *err = strdup("curl_global_init failed");
      return -1;
    }
    started = true;
  }
  CURL *handle = curl_easy_init();
  if (handle == NULL) {
    *err = strdup("curl_easy_init failed");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  sink s = {out, max_bytes};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  curl_easy_setopt(handle, CURLOPT_URL, url);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &s);
  if (strcmp(method, "POST") == 0) {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body != NULL ? body : "");
  } else {
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
  }
  CURLcode rc = curl_easy_perform(handle);
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  if (rc != CURLE_OK) {
    *err = strdup(curl_easy_strerror(rc));
    wd_http_response_free(out);
    return -1;
  }
  out->status = status;
  if (out->body == NULL) {
    out->body = calloc(1, 1);
    if (out->body == NULL) {
      *err = strdup("out of memory");
      return -1;
    }
  }
  return 0;
}

void wd_http_response_free(wd_http_response *response) {
  if (response == NULL) return;
  free(response->body);
  response->body = NULL;
  response->len = 0;
}
