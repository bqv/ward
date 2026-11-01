/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "net/quic.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "core/util.h"
#include "net/pki.h"

#define WD_QUIC_MAX_CONNS 16
#define WD_QUIC_SCIDLEN 18
#define WD_QUIC_IDLE_MS 30000u
#define WD_QUIC_TIMEOUT_CAP 1000
#define WD_QUIC_PKT_MAX 65536
#define WD_QUIC_ALPN "\4ward"
#define WD_QUIC_ALPN_LEN 5
#define WD_QUIC_ERR_AUTH 1u

struct wd_quic_conn;

struct wd_quic_stream {
  struct wd_quic_conn *conn;
  int64_t id;
  bool remote;
  bool opened;
  bool notified;
  bool closed;
  bool fin_pending;
  bool fin_sent;
  bool authed;
  int scope;
  char serial[WD_PKI_SERIAL_MAX];
  size_t rlen;
  size_t wlen;
  size_t wsent;
  uint8_t *rbuf;
  uint8_t *wbuf;
};

typedef struct wd_quic_conn {
  struct wd_quic *owner;
  ngtcp2_conn *conn;
  ngtcp2_crypto_ossl_ctx *ossl;
  SSL *ssl;
  ngtcp2_crypto_conn_ref ref;
  ngtcp2_cid scid;
  ngtcp2_cid odcid;
  struct sockaddr_storage local;
  socklen_t local_len;
  struct sockaddr_storage remote;
  socklen_t remote_len;
  wd_quic_stream *stream;
  bool handshake_done;
  bool authed;
  bool auth_failed;
  bool fatal;
  bool stream_closed;
  bool close_when_done;
  bool dead;
  int scope;
  char serial[WD_PKI_SERIAL_MAX];
} wd_quic_conn;

struct wd_quic {
  int fd;
  int port;
  uint64_t now_ms;
  struct sockaddr_storage local;
  socklen_t local_len;
  SSL_CTX *ssl_ctx;
  wd_pki *pki;
  wd_quic_events events;
  void *ctx;
  wd_quic_conn *conns[WD_QUIC_MAX_CONNS];
  uint8_t *txbuf;
};

struct wd_quic_client {
  int fd;
  uint64_t now_ms;
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  ngtcp2_crypto_ossl_ctx *ossl;
  ngtcp2_crypto_conn_ref ref;
  ngtcp2_conn *conn;
  struct sockaddr_storage local;
  socklen_t local_len;
  struct sockaddr_storage remote;
  socklen_t remote_len;
  char *pin;
  char *server_name;
  void (*frame)(void *ctx, const wd_frame *frame);
  void *ctx;
  uint8_t *txbuf;
  uint8_t *rbuf;
  uint8_t *wbuf;
  size_t rlen;
  size_t wlen;
  size_t wsent;
  int64_t stream_id;
  enum wd_quic_state state;
  int verified;
  bool started;
  bool handshake_done;
  bool auth_failed;
  bool fatal;
};

static int quic_err(char **err, const char *msg) {
  if (err != NULL) *err = strdup(msg);
  return -1;
}

static uint64_t quic_clock(uint64_t now_ms) {
  if (now_ms != 0) return now_ms;
  return wu_now_ms();
}

static ngtcp2_tstamp quic_ts(uint64_t now_ms) {
  return (ngtcp2_tstamp)now_ms * (ngtcp2_tstamp)NGTCP2_MILLISECONDS;
}

static int quic_global_init(void) {
  static bool ready = false;
  static int status = -1;
  if (!ready) {
    status = ngtcp2_crypto_ossl_init();
    ready = true;
  }
  return status;
}

static void quic_rand(uint8_t *dest, size_t destlen,
                      const ngtcp2_rand_ctx *rand_ctx) {
  (void)rand_ctx;
  if (wu_random_bytes(dest, destlen) != 0) memset(dest, 0, destlen);
}

static int quic_new_cid(ngtcp2_conn *conn, ngtcp2_cid *cid,
                        ngtcp2_stateless_reset_token *token, size_t cidlen,
                        void *user_data) {
  (void)conn;
  (void)user_data;
  if (cidlen > NGTCP2_MAX_CIDLEN) return NGTCP2_ERR_CALLBACK_FAILURE;
  if (wu_random_bytes(cid->data, cidlen) != 0)
    return NGTCP2_ERR_CALLBACK_FAILURE;
  cid->datalen = cidlen;
  if (wu_random_bytes(token->data, sizeof(token->data)) != 0)
    return NGTCP2_ERR_CALLBACK_FAILURE;
  return 0;
}

static int quic_alpn_select(SSL *ssl, const unsigned char **out,
                            unsigned char *outlen, const unsigned char *in,
                            unsigned int inlen, void *arg) {
  unsigned int off = 0;
  (void)ssl;
  (void)arg;
  while (off < inlen) {
    unsigned int len = in[off];
    if (len == 0 || off + 1 + len > inlen) break;
    if (len == 4 && memcmp(in + off + 1, "ward", 4) == 0) {
      *out = in + off + 1;
      *outlen = 4;
      return SSL_TLSEXT_ERR_OK;
    }
    off += len + 1;
  }
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static int quic_verify_allow(int preverify_ok, X509_STORE_CTX *store) {
  (void)preverify_ok;
  (void)store;
  return 1;
}

static int quic_cert_fingerprint(X509 *cert, char out[65]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &len) != 1 || len != 32)
    return -1;
  for (unsigned int i = 0; i < 32; i++) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 0x0f];
  }
  out[64] = '\0';
  return 0;
}

static int quic_pin_match(const char *pin, const char *fingerprint) {
  const char *p = pin;
  const char *f = fingerprint;
  for (;;) {
    while (*p == ':' || *p == '-' || *p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *f == '\0') break;
    if (tolower((unsigned char)*p) != tolower((unsigned char)*f)) return -1;
    p++;
    f++;
  }
  while (*p == ':' || *p == '-' || *p == ' ' || *p == '\t') p++;
  return (*p == '\0' && *f == '\0') ? 0 : -1;
}

typedef struct {
  void (*frame)(void *ctx, const wd_frame *frame, bool *stop);
  void *ctx;
} quic_sink;

static int quic_drain(uint8_t *buf, size_t *len, const quic_sink *sink,
                      size_t *consumed) {
  size_t off = 0;
  int rv = 0;
  bool stop = false;
  while (!stop && *len - off >= WD_FRAME_HEADER) {
    wd_frame frame;
    size_t used = 0;
    enum wd_frame_status fs =
      wd_frame_parse(buf + off, *len - off, &frame, &used);
    if (fs == WD_FRAME_PARTIAL) break;
    if (fs == WD_FRAME_INVALID) {
      rv = -1;
      break;
    }
    if (sink->frame != NULL) sink->frame(sink->ctx, &frame, &stop);
    off += used;
  }
  if (off > 0) {
    memmove(buf, buf + off, *len - off);
    *len -= off;
  }
  if (consumed != NULL) *consumed = off;
  return rv;
}

static ngtcp2_conn *quic_server_ref_get(ngtcp2_crypto_conn_ref *ref) {
  wd_quic_conn *cc = ref->user_data;
  return cc->conn;
}

static int quic_server_handshake_completed(ngtcp2_conn *conn, void *user_data) {
  wd_quic_conn *cc = user_data;
  char err[256];
  char serial[WD_PKI_SERIAL_MAX];
  int scope = WD_SCOPE_NONE;
  SSL *ssl;
  X509 *peer;
  (void)conn;
  cc->handshake_done = true;
  ssl = ngtcp2_crypto_ossl_ctx_get_ssl(cc->ossl);
  peer = ssl != NULL ? SSL_get0_peer_certificate(ssl) : NULL;
  if (peer == NULL) return 0;
  memset(serial, 0, sizeof(serial));
  err[0] = '\0';
  if (wd_pki_peer_check(cc->owner->pki, peer, &scope, serial, err, sizeof(err)) !=
      0) {
    cc->auth_failed = true;
    return 0;
  }
  cc->authed = true;
  cc->scope = scope;
  memcpy(cc->serial, serial, sizeof(cc->serial));
  return 0;
}

static int quic_server_recv_stream_data(ngtcp2_conn *conn, uint32_t flags,
                                        int64_t stream_id, uint64_t offset,
                                        const uint8_t *data, size_t datalen,
                                        void *user_data,
                                        void *stream_user_data) {
  wd_quic_conn *cc = user_data;
  wd_quic_stream *st = cc->stream;
  (void)flags;
  (void)offset;
  (void)stream_user_data;
  if (st == NULL) return 0;
  if (!st->remote) {
    if (!ngtcp2_is_bidi_stream(stream_id) ||
        ngtcp2_conn_is_local_stream2(conn, stream_id) || st->id >= 0)
      return 0;
    st->id = stream_id;
    st->remote = true;
  } else if (st->id != stream_id) {
    return 0;
  }
  if (datalen == 0) return 0;
  if (datalen > WD_QUIC_MAX_STREAM - st->rlen) {
    cc->fatal = true;
    return 0;
  }
  memcpy(st->rbuf + st->rlen, data, datalen);
  st->rlen += datalen;
  return 0;
}

static int quic_server_stream_close(ngtcp2_conn *conn, uint32_t flags,
                                    int64_t stream_id, uint64_t rx_app_error,
                                    uint64_t tx_app_error, void *user_data,
                                    void *stream_user_data) {
  wd_quic_conn *cc = user_data;
  (void)conn;
  (void)flags;
  (void)rx_app_error;
  (void)tx_app_error;
  (void)stream_user_data;
  if (cc->stream != NULL && cc->stream->id == stream_id)
    cc->stream_closed = true;
  return 0;
}

static ngtcp2_callbacks quic_server_callbacks(void) {
  ngtcp2_callbacks cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
  cbs.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
  cbs.handshake_completed = quic_server_handshake_completed;
  cbs.encrypt = ngtcp2_crypto_encrypt_cb;
  cbs.decrypt = ngtcp2_crypto_decrypt_cb;
  cbs.hp_mask = ngtcp2_crypto_hp_mask_cb;
  cbs.recv_stream_data = quic_server_recv_stream_data;
  cbs.stream_close2 = quic_server_stream_close;
  cbs.rand = quic_rand;
  cbs.update_key = ngtcp2_crypto_update_key_cb;
  cbs.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
  cbs.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
  cbs.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
  cbs.get_new_connection_id2 = quic_new_cid;
  cbs.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
  return cbs;
}

static wd_quic_stream *quic_stream_new(wd_quic_conn *cc) {
  wd_quic_stream *st = calloc(1, sizeof(*st));
  if (st == NULL) return NULL;
  st->rbuf = malloc(WD_QUIC_MAX_STREAM);
  st->wbuf = malloc(WD_QUIC_MAX_STREAM);
  if (st->rbuf == NULL || st->wbuf == NULL) {
    free(st->rbuf);
    free(st->wbuf);
    free(st);
    return NULL;
  }
  st->conn = cc;
  st->id = -1;
  st->serial[0] = '\0';
  return st;
}

static void quic_stream_free(wd_quic_stream *st) {
  if (st == NULL) return;
  free(st->rbuf);
  free(st->wbuf);
  free(st);
}

static void quic_conn_destroy(wd_quic *q, wd_quic_conn *cc) {
  if (cc == NULL) return;
  cc->dead = true;
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++)
    if (q->conns[i] == cc) q->conns[i] = NULL;
  if (cc->stream != NULL && cc->stream->opened && !cc->stream->notified) {
    cc->stream->notified = true;
    if (q->events.stream_close != NULL)
      q->events.stream_close(q->ctx, cc->stream);
  }
  quic_stream_free(cc->stream);
  cc->stream = NULL;
  if (cc->conn != NULL) {
    ngtcp2_conn_del(cc->conn);
    cc->conn = NULL;
  }
  if (cc->ssl != NULL) {
    SSL_set_app_data(cc->ssl, NULL);
    SSL_free(cc->ssl);
    cc->ssl = NULL;
  }
  if (cc->ossl != NULL) {
    ngtcp2_crypto_ossl_ctx_del(cc->ossl);
    cc->ossl = NULL;
  }
  free(cc);
}

static wd_quic_conn *quic_conn_find(wd_quic *q, const uint8_t *dcid,
                                    size_t dcidlen) {
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++) {
    wd_quic_conn *cc = q->conns[i];
    if (cc == NULL || cc->dead) continue;
    if (cc->scid.datalen == dcidlen &&
        memcmp(cc->scid.data, dcid, dcidlen) == 0)
      return cc;
    if (cc->odcid.datalen == dcidlen &&
        memcmp(cc->odcid.data, dcid, dcidlen) == 0)
      return cc;
  }
  return NULL;
}

static bool quic_conn_install(wd_quic *q, wd_quic_conn *cc) {
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++) {
    if (q->conns[i] == NULL) {
      q->conns[i] = cc;
      return true;
    }
  }
  return false;
}

static int quic_send_to(int fd, const struct sockaddr *sa, socklen_t salen,
                        const uint8_t *data, size_t len) {
  struct iovec iov;
  struct msghdr msg;
  ssize_t n;
  memset(&msg, 0, sizeof(msg));
  iov.iov_base = (void *)data;
  iov.iov_len = len;
  msg.msg_name = (void *)sa;
  msg.msg_namelen = sa != NULL ? salen : 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  do {
    n = sendmsg(fd, &msg, MSG_DONTWAIT);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
  return 0;
}

static int quic_conn_send(wd_quic_conn *cc, const uint8_t *data, size_t len) {
  return quic_send_to(cc->owner->fd, (const struct sockaddr *)&cc->remote,
                      cc->remote_len, data, len);
}

static void quic_conn_send_close(wd_quic *q, wd_quic_conn *cc,
                                 uint64_t app_error) {
  ngtcp2_ccerr ccerr;
  ngtcp2_path_storage ps;
  ngtcp2_pkt_info pi;
  ngtcp2_ssize nwrite;
  if (cc == NULL || cc->conn == NULL) return;
  if (ngtcp2_conn_in_closing_period2(cc->conn) ||
      ngtcp2_conn_in_draining_period2(cc->conn))
    return;
  ngtcp2_ccerr_default(&ccerr);
  ngtcp2_ccerr_set_application_error(&ccerr, app_error, NULL, 0);
  ngtcp2_path_storage_zero(&ps);
  memset(&pi, 0, sizeof(pi));
  nwrite = ngtcp2_conn_write_connection_close(
    cc->conn, &ps.path, &pi, q->txbuf, WD_QUIC_PKT_MAX, &ccerr,
    quic_ts(q->now_ms));
  if (nwrite > 0) (void)quic_conn_send(cc, q->txbuf, (size_t)nwrite);
}

static void quic_serve_frame(void *ctx, const wd_frame *frame, bool *stop) {
  wd_quic_stream *st = ctx;
  wd_quic *q = st->conn->owner;
  if (q->events.stream_frame != NULL)
    q->events.stream_frame(q->ctx, st, frame);
  if (st->closed) *stop = true;
}

static void quic_server_open_stream(wd_quic *q, wd_quic_conn *cc) {
  wd_quic_stream *st = cc->stream;
  if (st == NULL || st->opened || !st->remote) return;
  if (!cc->handshake_done || cc->auth_failed) return;
  st->opened = true;
  st->authed = cc->authed;
  st->scope = cc->scope;
  memcpy(st->serial, cc->serial, sizeof(st->serial));
  if (q->events.stream_open != NULL) q->events.stream_open(q->ctx, st);
}

static void quic_server_parse(wd_quic *q, wd_quic_conn *cc) {
  wd_quic_stream *st = cc->stream;
  quic_sink sink;
  size_t consumed = 0;
  (void)q;
  if (st == NULL || !st->opened || st->closed || st->rlen == 0) return;
  sink.frame = quic_serve_frame;
  sink.ctx = st;
  if (quic_drain(st->rbuf, &st->rlen, &sink, &consumed) != 0) {
    cc->fatal = true;
    return;
  }
  if (consumed > 0) {
    (void)ngtcp2_conn_extend_max_stream_offset(cc->conn, st->id, consumed);
    ngtcp2_conn_extend_max_offset(cc->conn, consumed);
  }
}

static int quic_conn_flush(wd_quic *q, wd_quic_conn *cc, ngtcp2_tstamp ts) {
  unsigned iter = 0;
  bool blocked = false;
  bool wrote = false;
  if (cc->conn == NULL) return -1;
  if (ngtcp2_conn_in_closing_period2(cc->conn) ||
      ngtcp2_conn_in_draining_period2(cc->conn))
    return 0;
  while (iter++ < 128) {
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_vec vec;
    ngtcp2_ssize ndatalen = -1;
    ngtcp2_ssize nwrite;
    wd_quic_stream *st = cc->stream;
    int64_t stream_id = -1;
    size_t vcnt = 0;
    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
    bool fin_only = false;
    bool have = false;
    ngtcp2_path_storage_zero(&ps);
    memset(&pi, 0, sizeof(pi));
    memset(&vec, 0, sizeof(vec));
    if (!blocked && st != NULL && st->id >= 0) {
      if (st->wlen > st->wsent) {
        have = true;
        vec.base = st->wbuf + st->wsent;
        vec.len = st->wlen - st->wsent;
        vcnt = 1;
      } else if (st->fin_pending && !st->fin_sent) {
        have = true;
        fin_only = true;
        vcnt = 1;
      }
    }
    if (have) {
      stream_id = st->id;
      flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
      if (st->fin_pending) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
    }
    nwrite = ngtcp2_conn_writev_stream(cc->conn, &ps.path, &pi, q->txbuf,
                                       WD_QUIC_PKT_MAX, &ndatalen, flags,
                                       stream_id, vcnt > 0 ? &vec : NULL, vcnt,
                                       ts);
    if (nwrite < 0) {
      if (nwrite == NGTCP2_ERR_WRITE_MORE) {
        if (have && ndatalen > 0) {
          st->wsent += (size_t)ndatalen;
          if (st->wlen == st->wsent && st->fin_pending) st->fin_sent = true;
        }
        continue;
      }
      if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
          nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
          nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
        blocked = true;
        continue;
      }
      return -1;
    }
    if (nwrite == 0) break;
    wrote = true;
    if (have && ndatalen > 0) {
      st->wsent += (size_t)ndatalen;
      if (st->wlen == st->wsent && st->fin_pending) st->fin_sent = true;
    } else if (have && fin_only && ndatalen >= 0) {
      st->fin_sent = true;
    }
    if (quic_conn_send(cc, q->txbuf, (size_t)nwrite) != 0) return -1;
  }
  if (wrote) ngtcp2_conn_update_pkt_tx_time(cc->conn, ts);
  if (cc->close_when_done && cc->stream != NULL && cc->stream->fin_sent) {
    quic_conn_send_close(q, cc, NGTCP2_NO_ERROR);
    cc->dead = true;
    return 1;
  }
  return 0;
}

static int quic_pump_conn(wd_quic *q, wd_quic_conn *cc, uint64_t now_ms) {
  ngtcp2_tstamp ts = quic_ts(now_ms);
  if (cc->dead) {
    quic_conn_destroy(q, cc);
    return -1;
  }
  if (cc->auth_failed) {
    quic_conn_send_close(q, cc, WD_QUIC_ERR_AUTH);
    quic_conn_destroy(q, cc);
    return -1;
  }
  if (cc->conn == NULL) {
    quic_conn_destroy(q, cc);
    return -1;
  }
  if (ngtcp2_conn_get_expiry2(cc->conn) <= ts) {
    int rv = ngtcp2_conn_handle_expiry(cc->conn, ts);
    if (rv != 0) {
      if (rv != NGTCP2_ERR_IDLE_CLOSE) quic_conn_send_close(q, cc, 0);
      quic_conn_destroy(q, cc);
      return -1;
    }
    if (ngtcp2_conn_in_draining_period2(cc->conn)) {
      quic_conn_destroy(q, cc);
      return -1;
    }
  }
  if (cc->handshake_done) quic_server_open_stream(q, cc);
  quic_server_parse(q, cc);
  if (cc->fatal) {
    quic_conn_send_close(q, cc, 0);
    quic_conn_destroy(q, cc);
    return -1;
  }
  if (cc->stream_closed) {
    quic_conn_destroy(q, cc);
    return -1;
  }
  if (quic_conn_flush(q, cc, ts) != 0) {
    quic_conn_destroy(q, cc);
    return -1;
  }
  return 0;
}

static int quic_split_bind(const char *bind, char *host, size_t hostcap,
                           char *port, size_t portcap) {
  const char *h = bind;
  const char *p;
  size_t hlen;
  if (bind[0] == '[') {
    const char *close = strchr(bind, ']');
    if (close == NULL || close[1] != ':') return -1;
    h = bind + 1;
    hlen = (size_t)(close - h);
    p = close + 2;
  } else {
    const char *colon = strrchr(bind, ':');
    if (colon == NULL) return -1;
    hlen = (size_t)(colon - bind);
    p = colon + 1;
  }
  if (hlen >= hostcap) return -1;
  if (strlen(p) >= portcap) return -1;
  memcpy(host, h, hlen);
  host[hlen] = '\0';
  memcpy(port, p, strlen(p) + 1);
  return 0;
}

static int quic_bind_socket(wd_quic *q, const char *spec) {
  char host[256];
  char port[16];
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *rp;
  int fd = -1;
  if (quic_split_bind(spec, host, sizeof(host), port, sizeof(port)) != 0)
    return -1;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  hints.ai_flags = AI_PASSIVE;
  if (getaddrinfo(host[0] != '\0' ? host : NULL, port, &hints, &res) != 0)
    return -1;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    int on = 1;
    fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                rp->ai_protocol);
    if (fd < 0) continue;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (rp->ai_family == AF_INET6)
      (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  q->fd = fd;
  q->local_len = sizeof(q->local);
  if (getsockname(fd, (struct sockaddr *)&q->local, &q->local_len) != 0) {
    close(fd);
    q->fd = -1;
    return -1;
  }
  if (q->local.ss_family == AF_INET)
    q->port = (int)ntohs(((struct sockaddr_in *)&q->local)->sin_port);
  else if (q->local.ss_family == AF_INET6)
    q->port = (int)ntohs(((struct sockaddr_in6 *)&q->local)->sin6_port);
  return 0;
}

static SSL_CTX *quic_server_ssl_ctx(wd_pki *pki, char **err) {
  const char *cert = wd_pki_server_cert(pki);
  const char *key = wd_pki_server_key(pki);
  const char *ca = wd_pki_ca_cert(pki);
  SSL_CTX *ctx;
  if (cert == NULL || key == NULL || ca == NULL) {
    quic_err(err, "pki has no server material");
    return NULL;
  }
  ctx = SSL_CTX_new(TLS_server_method());
  if (ctx == NULL) {
    quic_err(err, "could not create tls context");
    return NULL;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, quic_verify_allow);
  SSL_CTX_set_alpn_select_cb(ctx, quic_alpn_select, NULL);
  if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
      SSL_CTX_check_private_key(ctx) != 1) {
    SSL_CTX_free(ctx);
    quic_err(err, "could not load server credentials");
    return NULL;
  }
  return ctx;
}

wd_quic *wd_quic_new(const wd_quic_config *cfg, const wd_quic_events *events,
                     void *ctx, char **err) {
  wd_quic *q;
  if (err != NULL) *err = NULL;
  if (cfg == NULL || cfg->bind == NULL) {
    quic_err(err, "missing bind address");
    return NULL;
  }
  if (cfg->pki == NULL) {
    quic_err(err, "quic needs pki");
    return NULL;
  }
  if (quic_global_init() != 0) {
    quic_err(err, "could not initialise tls");
    return NULL;
  }
  q = calloc(1, sizeof(*q));
  if (q == NULL) {
    quic_err(err, "out of memory");
    return NULL;
  }
  q->fd = -1;
  q->pki = cfg->pki;
  q->ctx = ctx;
  if (events != NULL) q->events = *events;
  q->now_ms = quic_clock(0);
  q->txbuf = malloc(WD_QUIC_PKT_MAX);
  if (q->txbuf == NULL) {
    free(q);
    quic_err(err, "out of memory");
    return NULL;
  }
  if (quic_bind_socket(q, cfg->bind) != 0) {
    free(q->txbuf);
    free(q);
    quic_err(err, "could not bind quic socket");
    return NULL;
  }
  q->ssl_ctx = quic_server_ssl_ctx(cfg->pki, err);
  if (q->ssl_ctx == NULL) {
    close(q->fd);
    free(q->txbuf);
    free(q);
    return NULL;
  }
  return q;
}

void wd_quic_free(wd_quic *q) {
  if (q == NULL) return;
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++)
    if (q->conns[i] != NULL) quic_conn_destroy(q, q->conns[i]);
  if (q->fd >= 0) close(q->fd);
  if (q->ssl_ctx != NULL) SSL_CTX_free(q->ssl_ctx);
  free(q->txbuf);
  free(q);
}

int wd_quic_fd(wd_quic *q) { return q != NULL ? q->fd : -1; }

int wd_quic_port(wd_quic *q) { return q != NULL ? q->port : 0; }

static void quic_accept(wd_quic *q, struct sockaddr_storage *remote,
                        socklen_t remote_len, const uint8_t *data, size_t len,
                        uint64_t now_ms) {
  ngtcp2_pkt_hd hd;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  ngtcp2_path path;
  ngtcp2_callbacks cbs;
  wd_quic_conn *cc;
  int rv;
  if (ngtcp2_accept(&hd, data, len) != 0) return;
  if (hd.type != NGTCP2_PKT_INITIAL) return;
  cc = calloc(1, sizeof(*cc));
  if (cc == NULL) return;
  cc->owner = q;
  cc->ref.get_conn = quic_server_ref_get;
  cc->ref.user_data = cc;
  cc->local = q->local;
  cc->local_len = q->local_len;
  cc->remote = *remote;
  cc->remote_len = remote_len;
  cc->odcid = hd.dcid;
  cc->serial[0] = '\0';
  cc->stream = quic_stream_new(cc);
  if (cc->stream == NULL) {
    free(cc);
    return;
  }
  if (wu_random_bytes(cc->scid.data, WD_QUIC_SCIDLEN) != 0) {
    quic_stream_free(cc->stream);
    free(cc);
    return;
  }
  cc->scid.datalen = WD_QUIC_SCIDLEN;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = quic_ts(now_ms);
  settings.no_pmtud = 1;
  ngtcp2_transport_params_default(&params);
  params.initial_max_stream_data_bidi_local = WD_QUIC_MAX_STREAM;
  params.initial_max_stream_data_bidi_remote = WD_QUIC_MAX_STREAM;
  params.initial_max_data = WD_QUIC_MAX_STREAM;
  params.initial_max_streams_bidi = 1;
  params.initial_max_streams_uni = 0;
  params.max_idle_timeout =
    (ngtcp2_duration)WD_QUIC_IDLE_MS * NGTCP2_MILLISECONDS;
  params.max_udp_payload_size = 65527;
  params.active_connection_id_limit = 2;
  params.original_dcid = hd.dcid;
  params.original_dcid_present = 1;
  params.grease_quic_bit = 1;
  memset(&path, 0, sizeof(path));
  path.local.addr = (ngtcp2_sockaddr *)&cc->local;
  path.local.addrlen = cc->local_len;
  path.remote.addr = (ngtcp2_sockaddr *)&cc->remote;
  path.remote.addrlen = cc->remote_len;
  path.user_data = cc;
  cbs = quic_server_callbacks();
  rv = ngtcp2_conn_server_new(&cc->conn, &hd.scid, &cc->scid, &path, hd.version,
                              &cbs, &settings, &params, NULL, cc);
  if (rv != 0) {
    quic_stream_free(cc->stream);
    free(cc);
    return;
  }
  cc->ssl = SSL_new(q->ssl_ctx);
  if (cc->ssl == NULL) goto fail;
  if (ngtcp2_crypto_ossl_ctx_new(&cc->ossl, cc->ssl) != 0) goto fail;
  if (ngtcp2_crypto_ossl_configure_server_session(cc->ssl) != 0) goto fail;
  SSL_set_app_data(cc->ssl, &cc->ref);
  SSL_set_accept_state(cc->ssl);
  ngtcp2_conn_set_tls_native_handle(cc->conn, cc->ossl);
  {
    ngtcp2_path rpath;
    ngtcp2_pkt_info pi;
    memset(&rpath, 0, sizeof(rpath));
    rpath.local.addr = (ngtcp2_sockaddr *)&cc->local;
    rpath.local.addrlen = cc->local_len;
    rpath.remote.addr = (ngtcp2_sockaddr *)&cc->remote;
    rpath.remote.addrlen = cc->remote_len;
    rpath.user_data = cc;
    memset(&pi, 0, sizeof(pi));
    rv = ngtcp2_conn_read_pkt(cc->conn, &rpath, &pi, data, len,
                              quic_ts(now_ms));
  }
  if (rv != 0) goto fail;
  if (!quic_conn_install(q, cc)) goto fail;
  return;
fail:
  quic_conn_destroy(q, cc);
}

static void quic_handle_pkt(wd_quic *q, struct sockaddr_storage *remote,
                            socklen_t remote_len, const uint8_t *data,
                            size_t len, uint64_t now_ms) {
  ngtcp2_version_cid vc;
  ngtcp2_path path;
  ngtcp2_pkt_info pi;
  wd_quic_conn *cc;
  int rv;
  if (len < 21) return;
  if (ngtcp2_pkt_decode_version_cid(&vc, data, len, WD_QUIC_SCIDLEN) != 0)
    return;
  cc = quic_conn_find(q, vc.dcid, vc.dcidlen);
  if (cc == NULL) {
    quic_accept(q, remote, remote_len, data, len, now_ms);
    return;
  }
  if (ngtcp2_conn_in_closing_period2(cc->conn) ||
      ngtcp2_conn_in_draining_period2(cc->conn))
    return;
  memset(&path, 0, sizeof(path));
  path.local.addr = (ngtcp2_sockaddr *)&cc->local;
  path.local.addrlen = cc->local_len;
  path.remote.addr = (ngtcp2_sockaddr *)&cc->remote;
  path.remote.addrlen = cc->remote_len;
  path.user_data = cc;
  memset(&pi, 0, sizeof(pi));
  rv = ngtcp2_conn_read_pkt(cc->conn, &path, &pi, data, len, quic_ts(now_ms));
  if (rv != 0) {
    if (rv != NGTCP2_ERR_DRAINING && rv != NGTCP2_ERR_DROP_CONN)
      quic_conn_send_close(q, cc, 0);
    quic_conn_destroy(q, cc);
    return;
  }
  if (cc->handshake_done) quic_server_open_stream(q, cc);
  quic_server_parse(q, cc);
  if (cc->fatal) {
    quic_conn_send_close(q, cc, 0);
    quic_conn_destroy(q, cc);
  }
}

int wd_quic_service(wd_quic *q, uint64_t now_ms) {
  if (q == NULL) return -1;
  q->now_ms = quic_clock(now_ms);
  for (;;) {
    struct sockaddr_storage remote;
    socklen_t remote_len = sizeof(remote);
    ssize_t nread;
    memset(&remote, 0, sizeof(remote));
    nread = recvfrom(q->fd, q->txbuf, WD_QUIC_PKT_MAX, 0,
                     (struct sockaddr *)&remote, &remote_len);
    if (nread < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return -1;
    }
    if (nread == 0) continue;
    quic_handle_pkt(q, &remote, remote_len, q->txbuf, (size_t)nread,
                    q->now_ms);
  }
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++) {
    wd_quic_conn *cc = q->conns[i];
    if (cc == NULL) continue;
    (void)quic_pump_conn(q, cc, q->now_ms);
  }
  return 0;
}

int wd_quic_flush(wd_quic *q) {
  if (q == NULL) return -1;
  if (q->now_ms == 0) q->now_ms = quic_clock(0);
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++) {
    wd_quic_conn *cc = q->conns[i];
    int rv;
    if (cc == NULL || cc->dead) continue;
    rv = quic_conn_flush(q, cc, quic_ts(q->now_ms));
    if (rv != 0) cc->dead = true;
  }
  return 0;
}

int wd_quic_next_timeout(wd_quic *q, uint64_t now_ms) {
  ngtcp2_tstamp ts;
  ngtcp2_tstamp best = 0;
  bool found = false;
  if (q == NULL) return -1;
  ts = quic_ts(quic_clock(now_ms));
  for (unsigned i = 0; i < WD_QUIC_MAX_CONNS; i++) {
    ngtcp2_tstamp expiry;
    wd_quic_conn *cc = q->conns[i];
    if (cc == NULL || cc->dead || cc->conn == NULL) continue;
    expiry = ngtcp2_conn_get_expiry2(cc->conn);
    if (!found || expiry < best) {
      best = expiry;
      found = true;
    }
  }
  if (!found) return -1;
  if (best <= ts) return 0;
  {
    ngtcp2_tstamp diff = best - ts;
    uint64_t ms = (uint64_t)(diff / NGTCP2_MILLISECONDS);
    if (diff % NGTCP2_MILLISECONDS != 0) ms++;
    if (ms == 0) ms = 1;
    if (ms > WD_QUIC_TIMEOUT_CAP) ms = WD_QUIC_TIMEOUT_CAP;
    return (int)ms;
  }
}

int wd_quic_stream_write(wd_quic_stream *stream, const void *buf, size_t len) {
  if (stream == NULL || stream->closed) return -1;
  if (len == 0) return 0;
  if (len > WD_QUIC_MAX_STREAM - stream->wlen) return -1;
  memcpy(stream->wbuf + stream->wlen, buf, len);
  stream->wlen += len;
  return 0;
}

void wd_quic_stream_close(wd_quic_stream *stream) {
  wd_quic *q;
  if (stream == NULL || stream->closed) return;
  stream->closed = true;
  stream->fin_pending = true;
  q = stream->conn->owner;
  stream->conn->close_when_done = true;
  if (stream->opened && !stream->notified) {
    stream->notified = true;
    if (q->events.stream_close != NULL)
      q->events.stream_close(q->ctx, stream);
  }
}

bool wd_quic_stream_authenticated(wd_quic_stream *stream) {
  return stream != NULL && stream->authed;
}

const char *wd_quic_stream_scope(wd_quic_stream *stream) {
  if (stream == NULL || !stream->authed) return NULL;
  return wd_scope_name(stream->scope);
}

const char *wd_quic_stream_serial(wd_quic_stream *stream) {
  if (stream == NULL) return "";
  return stream->serial;
}

static ngtcp2_conn *quic_client_ref_get(ngtcp2_crypto_conn_ref *ref) {
  wd_quic_client *c = ref->user_data;
  return c->conn;
}

static int quic_client_handshake_completed(ngtcp2_conn *conn, void *user_data) {
  wd_quic_client *c = user_data;
  (void)conn;
  c->handshake_done = true;
  if (c->pin != NULL) {
    char fingerprint[65];
    SSL *ssl = ngtcp2_crypto_ossl_ctx_get_ssl(c->ossl);
    X509 *peer = ssl != NULL ? SSL_get0_peer_certificate(ssl) : NULL;
    if (peer == NULL || quic_cert_fingerprint(peer, fingerprint) != 0 ||
        quic_pin_match(c->pin, fingerprint) != 0) {
      c->auth_failed = true;
      return 0;
    }
  }
  c->verified = 1;
  c->state = WD_QUIC_ESTABLISHED;
  return 0;
}

static int quic_client_recv_stream_data(ngtcp2_conn *conn, uint32_t flags,
                                        int64_t stream_id, uint64_t offset,
                                        const uint8_t *data, size_t datalen,
                                        void *user_data,
                                        void *stream_user_data) {
  wd_quic_client *c = user_data;
  (void)conn;
  (void)flags;
  (void)offset;
  (void)stream_user_data;
  if (c->stream_id < 0 || stream_id != c->stream_id) return 0;
  if (datalen == 0) return 0;
  if (datalen > WD_QUIC_MAX_STREAM - c->rlen) {
    c->fatal = true;
    return 0;
  }
  memcpy(c->rbuf + c->rlen, data, datalen);
  c->rlen += datalen;
  return 0;
}

static int quic_client_stream_close(ngtcp2_conn *conn, uint32_t flags,
                                    int64_t stream_id, uint64_t rx_app_error,
                                    uint64_t tx_app_error, void *user_data,
                                    void *stream_user_data) {
  wd_quic_client *c = user_data;
  (void)conn;
  (void)flags;
  (void)rx_app_error;
  (void)tx_app_error;
  (void)stream_user_data;
  if (c->stream_id >= 0 && stream_id == c->stream_id) c->state = WD_QUIC_CLOSED;
  return 0;
}

static ngtcp2_callbacks quic_client_callbacks(void) {
  ngtcp2_callbacks cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.client_initial = ngtcp2_crypto_client_initial_cb;
  cbs.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
  cbs.handshake_completed = quic_client_handshake_completed;
  cbs.encrypt = ngtcp2_crypto_encrypt_cb;
  cbs.decrypt = ngtcp2_crypto_decrypt_cb;
  cbs.hp_mask = ngtcp2_crypto_hp_mask_cb;
  cbs.recv_stream_data = quic_client_recv_stream_data;
  cbs.stream_close2 = quic_client_stream_close;
  cbs.recv_retry = ngtcp2_crypto_recv_retry_cb;
  cbs.rand = quic_rand;
  cbs.update_key = ngtcp2_crypto_update_key_cb;
  cbs.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
  cbs.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
  cbs.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
  cbs.get_new_connection_id2 = quic_new_cid;
  cbs.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
  return cbs;
}

static void quic_client_frame(void *ctx, const wd_frame *frame, bool *stop) {
  wd_quic_client *c = ctx;
  (void)stop;
  if (c->frame != NULL) c->frame(c->ctx, frame);
}

static void quic_client_teardown(wd_quic_client *c) {
  if (c->conn != NULL) {
    ngtcp2_conn_del(c->conn);
    c->conn = NULL;
  }
  if (c->ssl != NULL) {
    SSL_set_app_data(c->ssl, NULL);
    SSL_free(c->ssl);
    c->ssl = NULL;
  }
  if (c->ossl != NULL) {
    ngtcp2_crypto_ossl_ctx_del(c->ossl);
    c->ossl = NULL;
  }
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  c->state = WD_QUIC_CLOSED;
}

static int quic_client_socket(wd_quic_client *c, const char *host,
                              uint16_t port) {
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  struct addrinfo *rp;
  char service[8];
  int fd = -1;
  snprintf(service, sizeof(service), "%u", (unsigned)port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  if (getaddrinfo(host, service, &hints, &res) != 0) return -1;
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                rp->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
      close(fd);
      fd = -1;
      continue;
    }
    memcpy(&c->remote, rp->ai_addr, rp->ai_addrlen);
    c->remote_len = rp->ai_addrlen;
    break;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  c->fd = fd;
  c->local_len = sizeof(c->local);
  if (getsockname(fd, (struct sockaddr *)&c->local, &c->local_len) != 0) {
    close(fd);
    c->fd = -1;
    return -1;
  }
  return 0;
}

static int quic_client_flush_at(wd_quic_client *c, ngtcp2_tstamp ts) {
  unsigned iter = 0;
  bool blocked = false;
  bool wrote = false;
  if (c->conn == NULL) return -1;
  if (ngtcp2_conn_in_closing_period2(c->conn) ||
      ngtcp2_conn_in_draining_period2(c->conn))
    return 0;
  while (iter++ < 128) {
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_vec vec;
    ngtcp2_ssize ndatalen = -1;
    ngtcp2_ssize nwrite;
    int64_t stream_id = -1;
    size_t vcnt = 0;
    uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
    bool have = false;
    ngtcp2_path_storage_zero(&ps);
    memset(&pi, 0, sizeof(pi));
    memset(&vec, 0, sizeof(vec));
    if (!blocked && c->stream_id >= 0 && c->wlen > c->wsent) {
      have = true;
      stream_id = c->stream_id;
      vec.base = c->wbuf + c->wsent;
      vec.len = c->wlen - c->wsent;
      vcnt = 1;
      flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
    }
    nwrite = ngtcp2_conn_writev_stream(c->conn, &ps.path, &pi, c->txbuf,
                                       WD_QUIC_PKT_MAX, &ndatalen, flags,
                                       stream_id, vcnt > 0 ? &vec : NULL, vcnt,
                                       ts);
    if (nwrite < 0) {
      if (nwrite == NGTCP2_ERR_WRITE_MORE) {
        if (have && ndatalen > 0) c->wsent += (size_t)ndatalen;
        continue;
      }
      if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED ||
          nwrite == NGTCP2_ERR_STREAM_NOT_FOUND ||
          nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
        blocked = true;
        continue;
      }
      return -1;
    }
    if (nwrite == 0) break;
    wrote = true;
    if (have && ndatalen > 0) c->wsent += (size_t)ndatalen;
    if (quic_send_to(c->fd, NULL, 0, c->txbuf, (size_t)nwrite) != 0) return -1;
  }
  if (wrote) ngtcp2_conn_update_pkt_tx_time(c->conn, ts);
  return 0;
}

static void quic_client_open_stream(wd_quic_client *c) {
  int64_t stream_id = -1;
  if (c->conn == NULL || c->stream_id >= 0) return;
  if (ngtcp2_conn_open_bidi_stream(c->conn, &stream_id, NULL) == 0)
    c->stream_id = stream_id;
}

static void quic_client_parse(wd_quic_client *c) {
  quic_sink sink;
  size_t consumed = 0;
  if (c->rlen == 0) return;
  sink.frame = quic_client_frame;
  sink.ctx = c;
  if (quic_drain(c->rbuf, &c->rlen, &sink, &consumed) != 0) {
    c->fatal = true;
    return;
  }
  if (consumed > 0 && c->conn != NULL && c->stream_id >= 0) {
    (void)ngtcp2_conn_extend_max_stream_offset(c->conn, c->stream_id, consumed);
    ngtcp2_conn_extend_max_offset(c->conn, consumed);
  }
}

wd_quic_client *wd_quic_client_new(const wd_quic_client_config *cfg,
                                   char **err) {
  wd_quic_client *c;
  if (err != NULL) *err = NULL;
  if (cfg == NULL) {
    quic_err(err, "missing configuration");
    return NULL;
  }
  if (quic_global_init() != 0) {
    quic_err(err, "could not initialise tls");
    return NULL;
  }
  c = calloc(1, sizeof(*c));
  if (c == NULL) {
    quic_err(err, "out of memory");
    return NULL;
  }
  c->fd = -1;
  c->stream_id = -1;
  c->state = WD_QUIC_CONNECTING;
  c->frame = cfg->frame;
  c->ctx = cfg->ctx;
  c->now_ms = quic_clock(0);
  c->txbuf = malloc(WD_QUIC_PKT_MAX);
  c->rbuf = malloc(WD_QUIC_MAX_STREAM);
  c->wbuf = malloc(WD_QUIC_MAX_STREAM);
  if (c->txbuf == NULL || c->rbuf == NULL || c->wbuf == NULL) {
    free(c->txbuf);
    free(c->rbuf);
    free(c->wbuf);
    free(c);
    quic_err(err, "out of memory");
    return NULL;
  }
  if (cfg->pin_sha256 != NULL) {
    c->pin = strdup(cfg->pin_sha256);
    if (c->pin == NULL) {
      quic_err(err, "out of memory");
      goto fail;
    }
  }
  if (cfg->server_name != NULL) {
    c->server_name = strdup(cfg->server_name);
    if (c->server_name == NULL) {
      quic_err(err, "out of memory");
      goto fail;
    }
  }
  c->ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (c->ssl_ctx == NULL) {
    quic_err(err, "could not create tls context");
    goto fail;
  }
  SSL_CTX_set_min_proto_version(c->ssl_ctx, TLS1_3_VERSION);
  if (SSL_CTX_set_alpn_protos(c->ssl_ctx, (const unsigned char *)WD_QUIC_ALPN,
                              WD_QUIC_ALPN_LEN) != 0) {
    quic_err(err, "could not offer alpn");
    goto fail;
  }
  if (cfg->ca_pem != NULL) {
    if (SSL_CTX_load_verify_locations(c->ssl_ctx, cfg->ca_pem, NULL) != 1) {
      quic_err(err, "could not load ca certificate");
      goto fail;
    }
    SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_PEER, NULL);
  } else {
    SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_NONE, NULL);
  }
  if (cfg->cert_pem != NULL || cfg->key_pem != NULL) {
    if (cfg->cert_pem == NULL || cfg->key_pem == NULL) {
      quic_err(err, "client certificate needs a key");
      goto fail;
    }
    if (SSL_CTX_use_certificate_chain_file(c->ssl_ctx, cfg->cert_pem) != 1 ||
        SSL_CTX_use_PrivateKey_file(c->ssl_ctx, cfg->key_pem,
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(c->ssl_ctx) != 1) {
      quic_err(err, "could not load client credentials");
      goto fail;
    }
  }
  return c;
fail:
  if (c->ssl_ctx != NULL) SSL_CTX_free(c->ssl_ctx);
  free(c->pin);
  free(c->server_name);
  free(c->txbuf);
  free(c->rbuf);
  free(c->wbuf);
  free(c);
  return NULL;
}

void wd_quic_client_free(wd_quic_client *client) {
  if (client == NULL) return;
  quic_client_teardown(client);
  if (client->ssl_ctx != NULL) SSL_CTX_free(client->ssl_ctx);
  free(client->pin);
  free(client->server_name);
  free(client->txbuf);
  free(client->rbuf);
  free(client->wbuf);
  free(client);
}

int wd_quic_client_fd(wd_quic_client *client) {
  return client != NULL ? client->fd : -1;
}

int wd_quic_client_start(wd_quic_client *client, const char *host,
                         uint16_t port, uint64_t now_ms) {
  ngtcp2_cid dcid;
  ngtcp2_cid scid;
  ngtcp2_settings settings;
  ngtcp2_transport_params params;
  ngtcp2_path path;
  ngtcp2_callbacks cbs;
  wd_quic_client *c = client;
  int rv;
  if (c == NULL || host == NULL || c->started || c->state == WD_QUIC_CLOSED)
    return -1;
  c->now_ms = quic_clock(now_ms);
  if (quic_client_socket(c, host, port) != 0) {
    c->state = WD_QUIC_CLOSED;
    return -1;
  }
  memset(&dcid, 0, sizeof(dcid));
  memset(&scid, 0, sizeof(scid));
  dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
  if (wu_random_bytes(dcid.data, dcid.datalen) != 0) goto fail;
  scid.datalen = WD_QUIC_SCIDLEN;
  if (wu_random_bytes(scid.data, scid.datalen) != 0) goto fail;
  ngtcp2_settings_default(&settings);
  settings.initial_ts = quic_ts(c->now_ms);
  settings.no_pmtud = 1;
  ngtcp2_transport_params_default(&params);
  params.initial_max_stream_data_bidi_local = WD_QUIC_MAX_STREAM;
  params.initial_max_stream_data_bidi_remote = WD_QUIC_MAX_STREAM;
  params.initial_max_data = WD_QUIC_MAX_STREAM;
  params.initial_max_streams_bidi = 1;
  params.initial_max_streams_uni = 0;
  params.max_idle_timeout =
    (ngtcp2_duration)WD_QUIC_IDLE_MS * NGTCP2_MILLISECONDS;
  params.max_udp_payload_size = 65527;
  params.active_connection_id_limit = 2;
  params.grease_quic_bit = 1;
  memset(&path, 0, sizeof(path));
  path.local.addr = (ngtcp2_sockaddr *)&c->local;
  path.local.addrlen = c->local_len;
  path.remote.addr = (ngtcp2_sockaddr *)&c->remote;
  path.remote.addrlen = c->remote_len;
  path.user_data = c;
  c->ref.get_conn = quic_client_ref_get;
  c->ref.user_data = c;
  cbs = quic_client_callbacks();
  rv = ngtcp2_conn_client_new(&c->conn, &dcid, &scid, &path,
                              NGTCP2_PROTO_VER_V1, &cbs, &settings, &params,
                              NULL, c);
  if (rv != 0) goto fail;
  c->ssl = SSL_new(c->ssl_ctx);
  if (c->ssl == NULL) goto fail;
  if (ngtcp2_crypto_ossl_ctx_new(&c->ossl, c->ssl) != 0) goto fail;
  if (ngtcp2_crypto_ossl_configure_client_session(c->ssl) != 0) goto fail;
  SSL_set_app_data(c->ssl, &c->ref);
  SSL_set_connect_state(c->ssl);

  {
    const char *sni = c->server_name != NULL ? c->server_name : host;
    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, sni, &a4) == 1 || inet_pton(AF_INET6, sni, &a6) == 1)
      sni = "localhost";
    (void)SSL_set_tlsext_host_name(c->ssl, sni);
  }
  ngtcp2_conn_set_tls_native_handle(c->conn, c->ossl);
  c->started = true;
  if (quic_client_flush_at(c, quic_ts(c->now_ms)) != 0) {
    quic_client_teardown(c);
    return -1;
  }
  return 0;
fail:
  quic_client_teardown(c);
  return -1;
}

int wd_quic_client_service(wd_quic_client *client, uint64_t now_ms) {
  wd_quic_client *c = client;
  ngtcp2_tstamp ts;
  bool closed = false;
  if (c == NULL) return -1;
  if (c->fd < 0) return 0;
  if (c->state == WD_QUIC_CLOSED) return 0;
  c->now_ms = quic_clock(now_ms);
  ts = quic_ts(c->now_ms);
  for (;;) {
    ssize_t nread;
    ngtcp2_path path;
    ngtcp2_pkt_info pi;
    int rv;
    nread = recvfrom(c->fd, c->txbuf, WD_QUIC_PKT_MAX, 0, NULL, NULL);
    if (nread < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      quic_client_teardown(c);
      return -1;
    }
    if (nread == 0) continue;
    memset(&path, 0, sizeof(path));
    path.local.addr = (ngtcp2_sockaddr *)&c->local;
    path.local.addrlen = c->local_len;
    path.remote.addr = (ngtcp2_sockaddr *)&c->remote;
    path.remote.addrlen = c->remote_len;
    path.user_data = c;
    memset(&pi, 0, sizeof(pi));
    rv = ngtcp2_conn_read_pkt(c->conn, &path, &pi, c->txbuf, (size_t)nread, ts);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_DROP_CONN) {
        closed = true;
        break;
      }
      quic_client_teardown(c);
      return -1;
    }
    if (c->handshake_done) quic_client_open_stream(c);
    quic_client_parse(c);
    if (c->auth_failed || c->fatal) {
      quic_client_teardown(c);
      return -1;
    }
  }
  if (closed) {
    c->state = WD_QUIC_CLOSED;
    return 0;
  }
  if (c->conn == NULL) return -1;
  if (ngtcp2_conn_get_expiry2(c->conn) <= ts) {
    int rv = ngtcp2_conn_handle_expiry(c->conn, ts);
    if (rv != 0) {
      if (rv == NGTCP2_ERR_IDLE_CLOSE) {
        c->state = WD_QUIC_CLOSED;
        return 0;
      }
      quic_client_teardown(c);
      return -1;
    }
    if (ngtcp2_conn_in_draining_period2(c->conn)) {
      c->state = WD_QUIC_CLOSED;
      return 0;
    }
  }
  if (c->handshake_done) quic_client_open_stream(c);
  quic_client_parse(c);
  if (c->auth_failed || c->fatal) {
    quic_client_teardown(c);
    return -1;
  }
  if (c->state == WD_QUIC_CLOSED) return 0;
  if (quic_client_flush_at(c, ts) != 0) {
    quic_client_teardown(c);
    return -1;
  }
  return 0;
}

int wd_quic_client_next_timeout(wd_quic_client *client, uint64_t now_ms) {
  ngtcp2_tstamp ts;
  ngtcp2_tstamp expiry;
  uint64_t ms;
  if (client == NULL || client->conn == NULL) return -1;
  if (client->state == WD_QUIC_CLOSED) return -1;
  ts = quic_ts(quic_clock(now_ms));
  expiry = ngtcp2_conn_get_expiry2(client->conn);
  if (expiry <= ts) return 0;
  ms = (uint64_t)((expiry - ts) / NGTCP2_MILLISECONDS);
  if ((expiry - ts) % NGTCP2_MILLISECONDS != 0) ms++;
  if (ms == 0) ms = 1;
  if (ms > WD_QUIC_TIMEOUT_CAP) ms = WD_QUIC_TIMEOUT_CAP;
  return (int)ms;
}

int wd_quic_client_write(wd_quic_client *client, const void *buf, size_t len) {
  if (client == NULL || client->state == WD_QUIC_CLOSED) return -1;
  if (len == 0) return 0;
  if (len > WD_QUIC_MAX_STREAM - client->wlen) return -1;
  memcpy(client->wbuf + client->wlen, buf, len);
  client->wlen += len;
  return 0;
}

int wd_quic_client_flush(wd_quic_client *client) {
  if (client == NULL || client->conn == NULL) return -1;
  if (client->now_ms == 0) client->now_ms = quic_clock(0);
  if (quic_client_flush_at(client, quic_ts(client->now_ms)) != 0) {
    quic_client_teardown(client);
    return -1;
  }
  return 0;
}

enum wd_quic_state wd_quic_client_state(wd_quic_client *client) {
  if (client == NULL) return WD_QUIC_CLOSED;
  return client->state;
}

int wd_quic_client_verified(wd_quic_client *client) {
  if (client == NULL) return 0;
  return client->verified;
}
