/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_NET_QUIC_H
#define WARD_NET_QUIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/frame.h"

#define WD_QUIC_MAX_STREAM (1u << 20)

typedef struct wd_pki wd_pki;
typedef struct wd_quic wd_quic;
typedef struct wd_quic_stream wd_quic_stream;

typedef struct {
  void (*stream_open)(void *ctx, wd_quic_stream *stream);
  void (*stream_frame)(void *ctx, wd_quic_stream *stream, const wd_frame *frame);
  void (*stream_close)(void *ctx, wd_quic_stream *stream);
} wd_quic_events;

typedef struct {
  const char *bind;
  wd_pki *pki;
} wd_quic_config;

wd_quic *wd_quic_new(const wd_quic_config *cfg, const wd_quic_events *events,
                     void *ctx, char **err);
void wd_quic_free(wd_quic *quic);

int wd_quic_fd(wd_quic *quic);
int wd_quic_port(wd_quic *quic);

int wd_quic_service(wd_quic *quic, uint64_t now_ms);
int wd_quic_flush(wd_quic *quic);
int wd_quic_next_timeout(wd_quic *quic, uint64_t now_ms);

int wd_quic_stream_write(wd_quic_stream *stream, const void *buf, size_t len);
void wd_quic_stream_close(wd_quic_stream *stream);

bool wd_quic_stream_authenticated(wd_quic_stream *stream);
const char *wd_quic_stream_scope(wd_quic_stream *stream);
const char *wd_quic_stream_serial(wd_quic_stream *stream);

enum wd_quic_state { WD_QUIC_CONNECTING = 0, WD_QUIC_ESTABLISHED = 1,
                     WD_QUIC_CLOSED = 2 };

typedef struct wd_quic_client wd_quic_client;

typedef struct {
  const char *cert_pem;
  const char *key_pem;
  const char *ca_pem;
  const char *pin_sha256;
  const char *server_name;
  void (*frame)(void *ctx, const wd_frame *frame);
  void *ctx;
} wd_quic_client_config;

wd_quic_client *wd_quic_client_new(const wd_quic_client_config *cfg, char **err);
void wd_quic_client_free(wd_quic_client *client);

int wd_quic_client_fd(wd_quic_client *client);
int wd_quic_client_start(wd_quic_client *client, const char *host, uint16_t port,
                         uint64_t now_ms);
int wd_quic_client_service(wd_quic_client *client, uint64_t now_ms);
int wd_quic_client_next_timeout(wd_quic_client *client, uint64_t now_ms);
int wd_quic_client_write(wd_quic_client *client, const void *buf, size_t len);
int wd_quic_client_flush(wd_quic_client *client);
enum wd_quic_state wd_quic_client_state(wd_quic_client *client);
int wd_quic_client_verified(wd_quic_client *client);

#endif
