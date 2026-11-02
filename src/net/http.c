/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "net/http.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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

char *wd_http_auth_header(const char *url, const char *api_key) {
  if (api_key == NULL || api_key[0] == '\0') return NULL;
  if (url == NULL || wd_http_loopback(url)) return NULL;
  size_t need = strlen(api_key) + 32;
  char *line = malloc(need);
  if (line == NULL) return NULL;
  snprintf(line, need, "Authorization: Bearer %s", api_key);
  return line;
}

bool wd_http_loopback(const char *url) {
  if (url == NULL) return false;
  const char *prefix = "http://127.0.0.1";
  size_t n = strlen(prefix);
  if (strncmp(url, prefix, n) == 0 && (url[n] == ':' || url[n] == '/'))
    return true;
  prefix = "http://localhost";
  n = strlen(prefix);
  if (strncmp(url, prefix, n) == 0 && (url[n] == ':' || url[n] == '/'))
    return true;
  prefix = "http://[::1]";
  n = strlen(prefix);
  if (strncmp(url, prefix, n) == 0 && (url[n] == ':' || url[n] == '/'))
    return true;
  return false;
}

int wd_http_request(const char *url, const char *method, const char *body,
                    const char *api_key, uint64_t timeout_ms, size_t max_bytes,
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
  char *auth = wd_http_auth_header(url, api_key);
  if (auth != NULL) {
    headers = curl_slist_append(headers, auth);
    free(auth);
  }
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

struct wd_http_job {
  pid_t pid;
  int fd;
};

typedef struct {
  int fd;
  int status_sent;
  long status;
  size_t max;
  size_t len;
} stream_ctx;

static size_t stream_write_cb(char *data, size_t size, size_t nmemb,
                              void *userdata) {
  stream_ctx *s = userdata;
  size_t n = size * nmemb;
  if (s->len + n > s->max) return 0;
  if (!s->status_sent) {
    uint32_t status = 0;
    uint8_t head[4] = {(uint8_t)(status & 0xff), (uint8_t)((status >> 8) & 0xff),
                       (uint8_t)((status >> 16) & 0xff),
                       (uint8_t)((status >> 24) & 0xff)};
    if (write(s->fd, head, sizeof(head)) != (ssize_t)sizeof(head)) return 0;
    s->status_sent = 1;
  }
  while (n > 0) {
    ssize_t sent = write(s->fd, data, n);
    if (sent <= 0) return 0;
    data += sent;
    n -= (size_t)sent;
  }
  s->len += size * nmemb;
  return size * nmemb;
}

static size_t stream_header_cb(char *data, size_t size, size_t nmemb,
                               void *userdata) {
  stream_ctx *s = userdata;
  size_t n = size * nmemb;
  if (!s->status_sent && n > 9 && strncmp(data, "HTTP/", 5) == 0) {
    long status = strtol(data + 9, NULL, 10);
    uint32_t value = (uint32_t)status;
    uint8_t head[4] = {(uint8_t)(value & 0xff), (uint8_t)((value >> 8) & 0xff),
                       (uint8_t)((value >> 16) & 0xff),
                       (uint8_t)((value >> 24) & 0xff)};
    if (write(s->fd, head, sizeof(head)) == (ssize_t)sizeof(head)) {
      s->status_sent = 1;
      s->status = status;
    }
  }
  return n;
}

wd_http_job *wd_http_start(const char *url, const char *method, const char *body,
                           const char *api_key, uint64_t timeout_ms,
                           size_t max_bytes, char **err) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    if (err != NULL) *err = strdup("cannot create the http pipe");
    return NULL;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    if (err != NULL) *err = strdup("cannot fork for the http request");
    return NULL;
  }
  if (pid == 0) {
    close(fds[0]);
    static bool started = false;
    if (!started) {
      curl_global_init(CURL_GLOBAL_DEFAULT);
      started = true;
    }
    CURL *handle = curl_easy_init();
    int failed = handle == NULL;
    long status = 0;
    if (handle != NULL) {
      stream_ctx ctx;
      memset(&ctx, 0, sizeof(ctx));
      ctx.fd = fds[1];
      ctx.max = max_bytes;
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Accept: text/event-stream");
      char *auth = wd_http_auth_header(url, api_key);
      if (auth != NULL) {
        headers = curl_slist_append(headers, auth);
        free(auth);
      }
      curl_easy_setopt(handle, CURLOPT_URL, url);
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
      curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, stream_write_cb);
      curl_easy_setopt(handle, CURLOPT_WRITEDATA, &ctx);
      curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, stream_header_cb);
      curl_easy_setopt(handle, CURLOPT_HEADERDATA, &ctx);
      if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body != NULL ? body : "");
      } else {
        curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
      }
      CURLcode rc = curl_easy_perform(handle);
      curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(handle);
      if (rc != CURLE_OK && !ctx.status_sent) {
        uint32_t zero = 0;
        uint8_t head[4] = {0, 0, 0, 0};
        (void)write(fds[1], head, sizeof(head));
        (void)zero;
      }
      failed = rc != CURLE_OK;
    }
    _exit(!failed && status != 0 ? 0 : 1);
  }
  close(fds[1]);
  wd_http_job *job = calloc(1, sizeof(*job));
  if (job == NULL) {
    (void)kill(pid, SIGKILL);
    close(fds[0]);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  job->pid = pid;
  job->fd = fds[0];
  return job;
}

int wd_http_job_fd(wd_http_job *job) { return job != NULL ? job->fd : -1; }

static int read_exact(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

int wd_http_job_finish(wd_http_job *job, wd_http_response *out, char **err) {
  memset(out, 0, sizeof(*out));
  uint8_t header[4];
  if (read_exact(job->fd, header, sizeof(header)) != 0) {
    if (err != NULL) *err = strdup("the http helper died");
    return -1;
  }
  uint32_t status = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                    ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
  out->status = (long)status;
  size_t cap = 8192;
  size_t len = 0;
  uint8_t *buffer = malloc(cap);
  if (buffer == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return -1;
  }
  for (;;) {
    if (len + 4096 > cap) {
      size_t grown = cap * 2;
      uint8_t *next = realloc(buffer, grown);
      if (next == NULL) {
        free(buffer);
        if (err != NULL) *err = strdup("out of memory");
        return -1;
      }
      buffer = next;
      cap = grown;
    }
    ssize_t got = read(job->fd, buffer + len, cap - len);
    if (got < 0) {
      if (errno == EINTR) continue;
      free(buffer);
      if (err != NULL) *err = strdup("the http helper died");
      return -1;
    }
    if (got == 0) break;
    len += (size_t)got;
  }
  if (len == cap) {
    uint8_t *next = realloc(buffer, cap + 1);
    if (next == NULL) {
      free(buffer);
      if (err != NULL) *err = strdup("out of memory");
      return -1;
    }
    buffer = next;
  }
  buffer[len] = '\0';
  out->body = buffer;
  out->len = len;
  return 0;
}

void wd_http_job_cancel(wd_http_job *job) {
  if (job == NULL || job->pid <= 0) return;
  kill(job->pid, SIGTERM);
  for (int i = 0; i < 20; i++) {
    pid_t done = waitpid(job->pid, NULL, WNOHANG);
    if (done == job->pid) {
      job->pid = -1;
      return;
    }
    struct timespec pause = {0, 10 * 1000 * 1000};
    nanosleep(&pause, NULL);
  }
  kill(job->pid, SIGKILL);
  while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {
  }
  job->pid = -1;
}

void wd_http_job_free(wd_http_job *job) {
  if (job == NULL) return;
  if (job->fd >= 0) close(job->fd);
  if (job->pid > 0) {
    while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {
    }
  }
  free(job);
}
