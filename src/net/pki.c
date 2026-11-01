/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "net/pki.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/util.h"

#define WD_PKI_TOKENS 8
#define WD_PKI_TOKEN_TTL 300000u

typedef struct {
  uint8_t secret[16];
  uint64_t expires_ms;
  int scope;
  bool used;
} wd_pki_token;

struct wd_pki {
  char *dir;
  char *ca_cert_path;
  char *ca_key_path;
  char *server_cert_path;
  char *server_key_path;
  char *crl_path;
  char *devices_path;
  X509 *ca_cert;
  EVP_PKEY *ca_key;
  X509 *server_cert;
  EVP_PKEY *server_key;
  X509_CRL *crl;
  char fingerprint[65];
  wd_pki_token tokens[WD_PKI_TOKENS];
  size_t token_at;
};

static void fail(char *err, size_t cap, const char *fmt, ...) {
  if (err == NULL || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

static int write_text(const char *path, const char *data, size_t len,
                      mode_t mode) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return -1;
  if (fchmod(fd, mode) != 0) {
    close(fd);
    return -1;
  }
  int rc = wu_write_all(fd, data, len);
  close(fd);
  return rc;
}

static char *bio_text(BIO *bio) {
  char *data = NULL;
  long len = BIO_get_mem_data(bio, &data);
  if (len <= 0 || data == NULL) return NULL;
  char *copy = malloc((size_t)len + 1);
  if (copy == NULL) return NULL;
  memcpy(copy, data, (size_t)len);
  copy[len] = '\0';
  return copy;
}

static char *pem_cert(X509 *cert) {
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio == NULL) return NULL;
  char *out = NULL;
  if (PEM_write_bio_X509(bio, cert) == 1) out = bio_text(bio);
  BIO_free(bio);
  return out;
}

static char *pem_key(EVP_PKEY *key) {
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio == NULL) return NULL;
  char *out = NULL;
  if (PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) == 1)
    out = bio_text(bio);
  BIO_free(bio);
  return out;
}

static char *pem_crl_text(X509_CRL *crl) {
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio == NULL) return NULL;
  char *out = NULL;
  if (PEM_write_bio_X509_CRL(bio, crl) == 1) out = bio_text(bio);
  BIO_free(bio);
  return out;
}

static X509 *load_cert(const char *path) {
  BIO *bio = BIO_new_file(path, "r");
  if (bio == NULL) return NULL;
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return cert;
}

static EVP_PKEY *load_key(const char *path) {
  BIO *bio = BIO_new_file(path, "r");
  if (bio == NULL) return NULL;
  EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return key;
}

static X509_CRL *load_crl(const char *path) {
  BIO *bio = BIO_new_file(path, "r");
  if (bio == NULL) return NULL;
  X509_CRL *crl = PEM_read_bio_X509_CRL(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return crl;
}

static int add_ext(X509 *cert, X509V3_CTX *ctx, int nid, const char *value) {
  X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, ctx, nid, value);
  if (ext == NULL) return -1;
  int rc = X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
  return rc == 1 ? 0 : -1;
}

static int random_serial(ASN1_INTEGER *out) {
  BIGNUM *bn = BN_new();
  if (bn == NULL) return -1;
  int rc = -1;
  if (BN_rand(bn, 128, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) == 1 &&
      BN_to_ASN1_INTEGER(bn, out) != NULL)
    rc = 0;
  BN_free(bn);
  return rc;
}

static EVP_PKEY *gen_key(void) {
  return EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
}

static X509 *make_cert(EVP_PKEY *key, const char *cn, X509_NAME *subject,
                       X509 *issuer, EVP_PKEY *issuer_key, bool is_ca,
                       int scope, bool server, long days) {
  X509 *cert = X509_new();
  if (cert == NULL) return NULL;
  if (X509_set_version(cert, 2) != 1) goto bad;
  if (random_serial(X509_get_serialNumber(cert)) != 0) goto bad;
  if (X509_gmtime_adj(X509_getm_notBefore(cert), -60) == NULL) goto bad;
  if (X509_gmtime_adj(X509_getm_notAfter(cert), days * 24L * 3600L) == NULL)
    goto bad;
  if (X509_set_pubkey(cert, key) != 1) goto bad;

  X509_NAME *name = subject != NULL ? subject : X509_NAME_new();
  if (name == NULL) goto bad;
  bool own_name = subject == NULL;
  if (own_name &&
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8,
                                 (const unsigned char *)cn, -1, -1, 0) != 1) {
    X509_NAME_free(name);
    goto bad;
  }
  if (!is_ca && scope != WD_SCOPE_NONE &&
      X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_UTF8,
                                 (const unsigned char *)wd_scope_name(scope),
                                 -1, -1, 0) != 1) {
    if (own_name) X509_NAME_free(name);
    goto bad;
  }
  if (X509_set_subject_name(cert, name) != 1) {
    if (own_name) X509_NAME_free(name);
    goto bad;
  }
  if (own_name) X509_NAME_free(name);

  X509 *issuer_cert = issuer != NULL ? issuer : cert;
  EVP_PKEY *signer = issuer_key != NULL ? issuer_key : key;
  if (X509_set_issuer_name(cert, X509_get_subject_name(issuer_cert)) != 1)
    goto bad;

  X509V3_CTX ctx;
  X509V3_set_ctx(&ctx, issuer_cert, cert, NULL, NULL, 0);
  if (add_ext(cert, &ctx, NID_basic_constraints,
              is_ca ? "critical,CA:TRUE" : "critical,CA:FALSE") != 0)
    goto bad;
  if (add_ext(cert, &ctx, NID_key_usage,
              is_ca ? "critical,keyCertSign,cRLSign"
                    : "critical,digitalSignature,keyEncipherment") != 0)
    goto bad;
  if (is_ca && add_ext(cert, &ctx, NID_subject_key_identifier, "hash") != 0)
    goto bad;
  if (!is_ca && add_ext(cert, &ctx, NID_ext_key_usage,
                        server ? "serverAuth" : "clientAuth") != 0)
    goto bad;
  if (server &&
      add_ext(cert, &ctx, NID_subject_alt_name, "DNS:ward,IP:127.0.0.1") != 0)
    goto bad;
  if (X509_sign(cert, signer, EVP_sha256()) <= 0) goto bad;
  return cert;
bad:
  X509_free(cert);
  return NULL;
}

static char *serial_hex(const ASN1_INTEGER *serial) {
  if (serial == NULL) return NULL;
  BIGNUM *bn = ASN1_INTEGER_to_BN(serial, NULL);
  if (bn == NULL) return NULL;
  char *hex = BN_bn2hex(bn);
  BN_free(bn);
  if (hex == NULL) return NULL;
  size_t len = strlen(hex);
  char *out = malloc(len + 2);
  if (out == NULL) {
    OPENSSL_free(hex);
    return NULL;
  }
  if (len % 2 == 0) {
    memcpy(out, hex, len + 1);
  } else {
    out[0] = '0';
    memcpy(out + 1, hex, len + 1);
  }
  OPENSSL_free(hex);
  return out;
}

static char *serial_text(X509 *cert) {
  return serial_hex(X509_get_serialNumber(cert));
}

static bool crl_revoked(X509_CRL *crl, X509 *cert) {
  if (crl == NULL) return false;
  char *want = serial_text(cert);
  if (want == NULL) return false;
  bool found = false;
  STACK_OF(X509_REVOKED) *list = X509_CRL_get_REVOKED(crl);
  for (int i = 0; list != NULL && i < sk_X509_REVOKED_num(list); i++) {
    X509_REVOKED *entry = sk_X509_REVOKED_value(list, i);
    char *got = serial_hex(X509_REVOKED_get0_serialNumber(entry));
    if (got != NULL && strcmp(got, want) == 0) found = true;
    free(got);
    if (found) break;
  }
  free(want);
  return found;
}

static const char *scope_text(int scope) {
  const char *name = wd_scope_name(scope);
  return name != NULL ? name : "";
}

static int scope_from_name(X509_NAME *name) {
  unsigned char buf[64];
  int len = X509_NAME_get_text_by_NID(name, NID_organizationalUnitName,
                                      (char *)buf, sizeof(buf));
  if (len <= 0) return WD_SCOPE_NONE;
  for (int scope = WD_SCOPE_READONLY; scope <= WD_SCOPE_DESKTOP; scope++)
    if (strcmp((const char *)buf, scope_text(scope)) == 0) return scope;
  return WD_SCOPE_NONE;
}

static void sanitise_name(const char *in, char *out, size_t cap) {
  size_t at = 0;
  for (const char *p = in; *p != '\0' && at + 1 < cap; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20 || c == 0x7f) continue;
    out[at++] = c;
  }
  out[at] = '\0';
  if (at == 0) snprintf(out, cap, "device");
}

static int read_devices(wd_pki *pki, wd_pki_device **out, size_t *count) {
  *out = NULL;
  *count = 0;
  FILE *file = fopen(pki->devices_path, "r");
  if (file == NULL) return errno == ENOENT ? 0 : -1;
  size_t cap = 0;
  char line[256];
  while (fgets(line, sizeof(line), file) != NULL) {
    char *fields[5];
    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, "\t\n", &save); tok != NULL && n < 5;
         tok = strtok_r(NULL, "\t\n", &save))
      fields[n++] = tok;
    if (n < 5) continue;
    if (*count == cap) {
      size_t grown = cap == 0 ? 8 : cap * 2;
      wd_pki_device *next = realloc(*out, grown * sizeof(**out));
      if (next == NULL) {
        free(*out);
        *out = NULL;
        *count = 0;
        fclose(file);
        return -1;
      }
      *out = next;
      cap = grown;
    }
    wd_pki_device *slot = &(*out)[*count];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->serial, sizeof(slot->serial), "%s", fields[0]);
    snprintf(slot->name, sizeof(slot->name), "%s", fields[1]);
    slot->scope = atoi(fields[2]);
    slot->issued_ms = strtoull(fields[3], NULL, 10);
    slot->revoked = strcmp(fields[4], "revoked") == 0;
    (*count)++;
  }
  fclose(file);
  return 0;
}

static int write_devices(wd_pki_device *devices, size_t count, const char *path,
                         char *err, size_t cap) {
  size_t size = count * 96 + 64;
  char *text = malloc(size);
  if (text == NULL) {
    fail(err, cap, "out of memory");
    return -1;
  }
  size_t at = 0;
  for (size_t i = 0; i < count; i++)
    at += (size_t)snprintf(text + at, size - at, "%s\t%s\t%d\t%llu\t%s\n",
                           devices[i].serial, devices[i].name, devices[i].scope,
                           (unsigned long long)devices[i].issued_ms,
                           devices[i].revoked ? "revoked" : "valid");
  int rc = write_text(path, text, at, 0644);
  free(text);
  if (rc != 0) fail(err, cap, "cannot write the device list");
  return rc;
}

static X509_CRL *make_crl(wd_pki *pki, wd_pki_device *devices, size_t count,
                          char *err, size_t cap) {
  X509_CRL *crl = X509_CRL_new();
  if (crl == NULL) {
    fail(err, cap, "out of memory");
    return NULL;
  }
  ASN1_TIME *now = X509_gmtime_adj(NULL, 0);
  ASN1_TIME *later = X509_gmtime_adj(NULL, 30L * 24L * 3600L);
  int stamped = now != NULL && later != NULL &&
                X509_CRL_set1_lastUpdate(crl, now) == 1 &&
                X509_CRL_set1_nextUpdate(crl, later) == 1;
  ASN1_TIME_free(now);
  ASN1_TIME_free(later);
  if (X509_CRL_set_version(crl, 1) != 1 ||
      X509_CRL_set_issuer_name(crl, X509_get_subject_name(pki->ca_cert)) != 1 ||
      !stamped)
    goto bad;
  ASN1_TIME *when = X509_gmtime_adj(NULL, 0);
  for (size_t i = 0; i < count; i++) {
    if (!devices[i].revoked) continue;
    BIGNUM *bn = NULL;
    if (BN_hex2bn(&bn, devices[i].serial) == 0) continue;
    ASN1_INTEGER *serial = BN_to_ASN1_INTEGER(bn, NULL);
    BN_free(bn);
    if (serial == NULL) continue;
    X509_REVOKED *revoked = X509_REVOKED_new();
    if (revoked != NULL && when != NULL &&
        X509_REVOKED_set_serialNumber(revoked, serial) == 1 &&
        X509_REVOKED_set_revocationDate(revoked, when) == 1)
      X509_CRL_add0_revoked(crl, revoked);
    else if (revoked != NULL)
      X509_REVOKED_free(revoked);
    ASN1_INTEGER_free(serial);
  }
  ASN1_TIME_free(when);
  X509_CRL_sort(crl);
  if (X509_CRL_sign(crl, pki->ca_key, EVP_sha256()) <= 0) goto bad;
  return crl;
bad:
  X509_CRL_free(crl);
  fail(err, cap, "cannot build a CRL");
  return NULL;
}

static int refresh_crl(wd_pki *pki, char *err, size_t cap) {
  wd_pki_device *devices = NULL;
  size_t count = 0;
  if (read_devices(pki, &devices, &count) != 0) {
    fail(err, cap, "cannot read the device list");
    return -1;
  }
  X509_CRL *crl = make_crl(pki, devices, count, err, cap);
  free(devices);
  if (crl == NULL) return -1;
  char *text = pem_crl_text(crl);
  if (text == NULL) {
    X509_CRL_free(crl);
    fail(err, cap, "cannot encode the CRL");
    return -1;
  }
  int rc = write_text(pki->crl_path, text, strlen(text), 0644);
  free(text);
  if (rc != 0) {
    X509_CRL_free(crl);
    fail(err, cap, "cannot write the CRL");
    return -1;
  }
  X509_CRL_free(pki->crl);
  pki->crl = crl;
  return 0;
}

static int ensure_ca(wd_pki *pki, char *err, size_t cap) {
  pki->ca_cert = load_cert(pki->ca_cert_path);
  pki->ca_key = load_key(pki->ca_key_path);
  if (pki->ca_cert != NULL && pki->ca_key != NULL) return 0;
  X509_free(pki->ca_cert);
  EVP_PKEY_free(pki->ca_key);
  pki->ca_cert = NULL;
  pki->ca_key = NULL;

  pki->ca_key = gen_key();
  if (pki->ca_key == NULL) {
    fail(err, cap, "cannot generate the CA key");
    return -1;
  }
  pki->ca_cert = make_cert(pki->ca_key, "ward local CA", NULL, NULL, NULL, true,
                           WD_SCOPE_NONE, false, 3650);
  if (pki->ca_cert == NULL) {
    fail(err, cap, "cannot generate the CA certificate");
    return -1;
  }
  char *cert = pem_cert(pki->ca_cert);
  char *key = pem_key(pki->ca_key);
  int rc = -1;
  if (cert != NULL && key != NULL &&
      write_text(pki->ca_cert_path, cert, strlen(cert), 0644) == 0 &&
      write_text(pki->ca_key_path, key, strlen(key), 0600) == 0)
    rc = 0;
  free(cert);
  free(key);
  if (rc != 0) fail(err, cap, "cannot write the CA to %s", pki->dir);
  return rc;
}

static int ensure_server(wd_pki *pki, char *err, size_t cap) {
  pki->server_cert = load_cert(pki->server_cert_path);
  pki->server_key = load_key(pki->server_key_path);
  if (pki->server_cert != NULL && pki->server_key != NULL) return 0;
  X509_free(pki->server_cert);
  EVP_PKEY_free(pki->server_key);
  pki->server_cert = NULL;
  pki->server_key = NULL;

  pki->server_key = gen_key();
  if (pki->server_key == NULL) {
    fail(err, cap, "cannot generate the server key");
    return -1;
  }
  pki->server_cert =
      make_cert(pki->server_key, "ward", NULL, pki->ca_cert, pki->ca_key, false,
                WD_SCOPE_NONE, true, 730);
  if (pki->server_cert == NULL) {
    fail(err, cap, "cannot issue the server certificate");
    return -1;
  }
  char *cert = pem_cert(pki->server_cert);
  char *key = pem_key(pki->server_key);
  int rc = -1;
  if (cert != NULL && key != NULL &&
      write_text(pki->server_cert_path, cert, strlen(cert), 0644) == 0 &&
      write_text(pki->server_key_path, key, strlen(key), 0600) == 0)
    rc = 0;
  free(cert);
  free(key);
  if (rc != 0) fail(err, cap, "cannot write the server certificate");
  return rc;
}

static int fingerprint_of(X509 *cert, char out[65]) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), digest, &len) != 1 || len != 32)
    return -1;
  for (unsigned int i = 0; i < len; i++)
    snprintf(out + i * 2, 3, "%02x", digest[i]);
  out[64] = '\0';
  return 0;
}

wd_pki *wd_pki_open(const char *dir, char **err) {
  if (err != NULL) *err = NULL;
  char message[160];
  message[0] = '\0';
  if (wu_mkdir_p(dir, 0700) != 0) {
    if (err != NULL)
      *err = strdup("cannot create the pki directory");
    return NULL;
  }
  wd_pki *pki = calloc(1, sizeof(*pki));
  if (pki == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  pki->dir = strdup(dir);
  pki->ca_cert_path = wu_path_join(dir, "ca.crt");
  pki->ca_key_path = wu_path_join(dir, "ca.key");
  pki->server_cert_path = wu_path_join(dir, "server.crt");
  pki->server_key_path = wu_path_join(dir, "server.key");
  pki->crl_path = wu_path_join(dir, "crl.pem");
  pki->devices_path = wu_path_join(dir, "devices.txt");
  if (pki->dir == NULL || pki->ca_cert_path == NULL ||
      pki->ca_key_path == NULL || pki->server_cert_path == NULL ||
      pki->server_key_path == NULL || pki->crl_path == NULL ||
      pki->devices_path == NULL) {
    wd_pki_free(pki);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  if (ensure_ca(pki, message, sizeof(message)) != 0 ||
      ensure_server(pki, message, sizeof(message)) != 0) {
    wd_pki_free(pki);
    if (err != NULL) *err = strdup(message);
    return NULL;
  }
  pki->crl = load_crl(pki->crl_path);
  if (pki->crl == NULL && refresh_crl(pki, message, sizeof(message)) != 0) {
    wd_pki_free(pki);
    if (err != NULL) *err = strdup(message);
    return NULL;
  }
  if (fingerprint_of(pki->server_cert, pki->fingerprint) != 0) {
    wd_pki_free(pki);
    if (err != NULL) *err = strdup("cannot fingerprint the server certificate");
    return NULL;
  }
  return pki;
}

void wd_pki_free(wd_pki *pki) {
  if (pki == NULL) return;
  free(pki->dir);
  free(pki->ca_cert_path);
  free(pki->ca_key_path);
  free(pki->server_cert_path);
  free(pki->server_key_path);
  free(pki->crl_path);
  free(pki->devices_path);
  X509_free(pki->ca_cert);
  EVP_PKEY_free(pki->ca_key);
  X509_free(pki->server_cert);
  EVP_PKEY_free(pki->server_key);
  X509_CRL_free(pki->crl);
  free(pki);
}

const char *wd_pki_dir(const wd_pki *pki) { return pki->dir; }
const char *wd_pki_server_cert(const wd_pki *pki) { return pki->server_cert_path; }
const char *wd_pki_server_key(const wd_pki *pki) { return pki->server_key_path; }
const char *wd_pki_ca_cert(const wd_pki *pki) { return pki->ca_cert_path; }
const char *wd_pki_crl(const wd_pki *pki) { return pki->crl_path; }

int wd_pki_fingerprint(wd_pki *pki, char out[65]) {
  memcpy(out, pki->fingerprint, 65);
  return 0;
}

const char *wd_scope_name(int scope) {
  switch (scope) {
    case WD_SCOPE_READONLY:
      return "readonly";
    case WD_SCOPE_PHONE:
      return "phone";
    case WD_SCOPE_DESKTOP:
      return "desktop";
    default:
      return NULL;
  }
}

int wd_scope_parse(const char *name) {
  if (name == NULL) return WD_SCOPE_NONE;
  for (int scope = WD_SCOPE_READONLY; scope <= WD_SCOPE_DESKTOP; scope++)
    if (strcmp(name, wd_scope_name(scope)) == 0) return scope;
  return WD_SCOPE_NONE;
}

static char *base32(const uint8_t *data, size_t len) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  size_t out_len = (len * 8 + 4) / 5;
  char *out = malloc(out_len + 1);
  if (out == NULL) return NULL;
  size_t at = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    acc = (acc << 8) | data[i];
    bits += 8;
    while (bits >= 5) {
      out[at++] = alphabet[(acc >> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out[at++] = alphabet[(acc << (5 - bits)) & 31];
  out[at] = '\0';
  return out;
}

char *wd_pki_token_mint(wd_pki *pki, uint64_t now_ms, uint64_t ttl_ms,
                        int scope) {
  wd_pki_token *slot = &pki->tokens[pki->token_at % WD_PKI_TOKENS];
  pki->token_at++;
  if (wu_random_bytes(slot->secret, sizeof(slot->secret)) != 0) return NULL;
  slot->expires_ms = now_ms + (ttl_ms == 0 ? WD_PKI_TOKEN_TTL : ttl_ms);
  slot->scope = scope;
  slot->used = false;
  return base32(slot->secret, sizeof(slot->secret));
}

int wd_pki_token_redeem(wd_pki *pki, const char *token, uint64_t now_ms,
                        int *scope_out) {
  if (token == NULL) return -1;
  for (size_t i = 0; i < WD_PKI_TOKENS; i++) {
    wd_pki_token *slot = &pki->tokens[i];
    if (slot->expires_ms == 0 || slot->used) continue;
    char *text = base32(slot->secret, sizeof(slot->secret));
    if (text == NULL) continue;
    bool match = false;
    if (strlen(text) == strlen(token)) {
      unsigned char diff = 0;
      for (size_t j = 0; text[j] != '\0'; j++)
        diff |= (unsigned char)(text[j] ^ token[j]);
      match = diff == 0;
    }
    free(text);
    if (!match) continue;
    if (now_ms >= slot->expires_ms) return -1;
    slot->used = true;
    slot->expires_ms = 0;
    if (scope_out != NULL) *scope_out = slot->scope;
    return 0;
  }
  return -1;
}

int wd_pki_keypair(char **key_pem, char **csr_pem, char *err, size_t cap) {
  *key_pem = NULL;
  *csr_pem = NULL;
  EVP_PKEY *key = gen_key();
  if (key == NULL) {
    fail(err, cap, "cannot generate a device key");
    return -1;
  }
  X509_REQ *req = X509_REQ_new();
  if (req == NULL) {
    EVP_PKEY_free(key);
    fail(err, cap, "out of memory");
    return -1;
  }
  int rc = -1;
  if (X509_REQ_set_version(req, 0) != 1) goto done;
  X509_NAME *subject = X509_REQ_get_subject_name(req);
  if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8,
                                 (const unsigned char *)"ward device", -1, -1,
                                 0) != 1)
    goto done;
  if (X509_REQ_set_pubkey(req, key) != 1) goto done;
  if (X509_REQ_sign(req, key, EVP_sha256()) <= 0) goto done;

  *key_pem = pem_key(key);
  BIO *bio = BIO_new(BIO_s_mem());
  if (bio == NULL) goto done;
  if (PEM_write_bio_X509_REQ(bio, req) == 1) *csr_pem = bio_text(bio);
  BIO_free(bio);
  if (*key_pem != NULL && *csr_pem != NULL) rc = 0;
done:
  if (rc != 0) {
    free(*key_pem);
    free(*csr_pem);
    *key_pem = NULL;
    *csr_pem = NULL;
    fail(err, cap, "cannot build a device key and request");
  }
  X509_REQ_free(req);
  EVP_PKEY_free(key);
  return rc;
}

int wd_pki_issue(wd_pki *pki, const char *csr_pem, int scope, char **cert_pem,
                 char serial[WD_PKI_SERIAL_MAX], char *err, size_t cap) {
  if (wd_scope_name(scope) == NULL) {
    fail(err, cap, "unknown scope");
    return -1;
  }
  BIO *bio = BIO_new_mem_buf(csr_pem, -1);
  if (bio == NULL) {
    fail(err, cap, "out of memory");
    return -1;
  }
  X509_REQ *req = PEM_read_bio_X509_REQ(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (req == NULL) {
    fail(err, cap, "cannot parse the certificate request");
    return -1;
  }
  int rc = -1;
  EVP_PKEY *pubkey = X509_REQ_get0_pubkey(req);
  if (pubkey == NULL || X509_REQ_verify(req, pubkey) != 1) {
    fail(err, cap, "the certificate request is not self-signed");
    goto done;
  }

  char name[64] = "device";
  X509_NAME *subject = X509_REQ_get_subject_name(req);
  if (subject != NULL) {
    char raw[128];
    int len = X509_NAME_get_text_by_NID(subject, NID_commonName, raw,
                                        sizeof(raw));
    if (len > 0) {
      raw[len] = '\0';
      sanitise_name(raw, name, sizeof(name));
    }
  }
  X509 *cert = make_cert(pubkey, name, NULL, pki->ca_cert, pki->ca_key, false,
                         scope, false, 730);
  if (cert == NULL) {
    fail(err, cap, "cannot sign the certificate");
    goto done;
  }
  char *text_serial = serial_text(cert);
  *cert_pem = pem_cert(cert);
  if (text_serial == NULL) {
    X509_free(cert);
    fail(err, cap, "cannot read the certificate serial");
    goto done;
  }
  snprintf(serial, WD_PKI_SERIAL_MAX, "%s", text_serial);
  free(text_serial);
  if (*cert_pem == NULL) {
    X509_free(cert);
    fail(err, cap, "cannot encode the certificate");
    goto done;
  }

  wd_pki_device *devices = NULL;
  size_t count = 0;
  if (read_devices(pki, &devices, &count) != 0) {
    free(*cert_pem);
    *cert_pem = NULL;
    X509_free(cert);
    fail(err, cap, "cannot read the device list");
    goto done;
  }
  wd_pki_device *grown = realloc(devices, (count + 1) * sizeof(*devices));
  if (grown == NULL) {
    free(devices);
    free(*cert_pem);
    *cert_pem = NULL;
    X509_free(cert);
    fail(err, cap, "out of memory");
    goto done;
  }
  memset(&grown[count], 0, sizeof(grown[count]));
  snprintf(grown[count].serial, sizeof(grown[count].serial), "%s", serial);
  snprintf(grown[count].name, sizeof(grown[count].name), "%s", name);
  grown[count].scope = scope;
  grown[count].issued_ms = wu_now_ms();
  grown[count].revoked = false;
  if (write_devices(grown, count + 1, pki->devices_path, err, cap) != 0) {
    free(grown);
    free(*cert_pem);
    *cert_pem = NULL;
    X509_free(cert);
    goto done;
  }
  free(grown);
  X509_free(cert);
  rc = 0;
done:
  X509_REQ_free(req);
  return rc;
}

int wd_pki_peer_check(wd_pki *pki, void *x509, int *scope_out,
                      char serial[WD_PKI_SERIAL_MAX], char *err, size_t cap) {
  X509 *cert = x509;
  if (cert == NULL) {
    fail(err, cap, "no peer certificate");
    return -1;
  }
  if (X509_NAME_cmp(X509_get_issuer_name(cert),
                    X509_get_subject_name(pki->ca_cert)) != 0) {
    fail(err, cap, "certificate is not issued by this ward CA");
    return -1;
  }
  EVP_PKEY *ca_pub = X509_get_pubkey(pki->ca_cert);
  if (ca_pub == NULL) {
    fail(err, cap, "cannot read the CA key");
    return -1;
  }
  int verified = X509_verify(cert, ca_pub);
  EVP_PKEY_free(ca_pub);
  if (verified != 1) {
    fail(err, cap, "certificate signature does not verify");
    return -1;
  }
  if (X509_cmp_current_time(X509_get0_notBefore(cert)) > 0 ||
      X509_cmp_current_time(X509_get0_notAfter(cert)) < 0) {
    fail(err, cap, "certificate is not valid now");
    return -1;
  }
  if (crl_revoked(pki->crl, cert)) {
    fail(err, cap, "device certificate is revoked");
    return -1;
  }
  int scope = scope_from_name(X509_get_subject_name(cert));
  if (scope == WD_SCOPE_NONE) {
    fail(err, cap, "certificate carries no ward scope");
    return -1;
  }
  char *text_serial = serial_text(cert);
  if (text_serial == NULL) {
    fail(err, cap, "cannot read the certificate serial");
    return -1;
  }
  snprintf(serial, WD_PKI_SERIAL_MAX, "%s", text_serial);
  free(text_serial);
  if (scope_out != NULL) *scope_out = scope;
  return 0;
}

int wd_pki_revoke(wd_pki *pki, const char *serial, char *err, size_t cap) {
  wd_pki_device *devices = NULL;
  size_t count = 0;
  if (read_devices(pki, &devices, &count) != 0) {
    fail(err, cap, "cannot read the device list");
    return -1;
  }
  bool found = false;
  for (size_t i = 0; i < count; i++)
    if (strcmp(devices[i].serial, serial) == 0) {
      devices[i].revoked = true;
      found = true;
    }
  if (!found) {
    free(devices);
    fail(err, cap, "no such device");
    return -1;
  }
  int rc = write_devices(devices, count, pki->devices_path, err, cap);
  free(devices);
  if (rc != 0) return -1;
  return refresh_crl(pki, err, cap);
}

int wd_pki_devices(wd_pki *pki, wd_pki_device **out, size_t *count, char *err,
                   size_t cap) {
  if (read_devices(pki, out, count) != 0) {
    fail(err, cap, "cannot read the device list");
    return -1;
  }
  return 0;
}
