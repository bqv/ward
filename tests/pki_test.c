/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net/pki.h"

static X509 *parse_cert(const char *pem) {
  BIO *bio = BIO_new_mem_buf(pem, -1);
  assert(bio != NULL);
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  return cert;
}

static void purge(const char *dir) {
  static const char *names[] = {"ca.crt", "ca.key", "server.crt", "server.key",
                                "crl.pem", "devices.txt"};
  char path[512];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    unlink(path);
  }
  rmdir(dir);
}

int main(void) {
  char template[] = "/tmp/ward-pki-XXXXXX";
  char *dir = mkdtemp(template);
  assert(dir != NULL);
  char err[256] = {0};
  char *open_err = NULL;

  wd_pki *pki = wd_pki_open(dir, &open_err);
  assert(pki != NULL);
  assert(open_err == NULL);
  assert(access(wd_pki_server_cert(pki), R_OK) == 0);
  assert(access(wd_pki_server_key(pki), R_OK) == 0);
  assert(access(wd_pki_ca_cert(pki), R_OK) == 0);

  char fingerprint[65];
  assert(wd_pki_fingerprint(pki, fingerprint) == 0);
  assert(strlen(fingerprint) == 64);

  char *token = wd_pki_token_mint(pki, 1000, 300000, WD_SCOPE_PHONE);
  assert(token != NULL);
  assert(strlen(token) == 26);
  assert(wd_pki_token_redeem(pki, "AAAAAAAAAAAAAAAAAAAAAAAAAA", 1000, NULL) != 0);
  int token_scope = WD_SCOPE_NONE;
  assert(wd_pki_token_redeem(pki, token, 1000, &token_scope) == 0);
  assert(token_scope == WD_SCOPE_PHONE);
  assert(wd_pki_token_redeem(pki, token, 1000, NULL) != 0);
  free(token);

  token = wd_pki_token_mint(pki, 1000, 10, WD_SCOPE_READONLY);
  assert(token != NULL);
  assert(wd_pki_token_redeem(pki, token, 1010, NULL) != 0);
  free(token);

  char *key = NULL;
  char *csr = NULL;
  assert(wd_pki_keypair(&key, &csr, err, sizeof(err)) == 0);
  assert(key != NULL && csr != NULL);

  char *cert_pem = NULL;
  char serial[WD_PKI_SERIAL_MAX] = {0};
  assert(wd_pki_issue(pki, csr, WD_SCOPE_PHONE, &cert_pem, serial, err,
                      sizeof(err)) == 0);
  assert(cert_pem != NULL);
  assert(serial[0] != '\0');

  X509 *cert = parse_cert(cert_pem);
  assert(cert != NULL);
  int scope = WD_SCOPE_NONE;
  char seen[WD_PKI_SERIAL_MAX] = {0};
  assert(wd_pki_peer_check(pki, cert, &scope, seen, err, sizeof(err)) == 0);
  assert(scope == WD_SCOPE_PHONE);
  assert(strcmp(seen, serial) == 0);

  wd_pki_device *devices = NULL;
  size_t count = 0;
  assert(wd_pki_devices(pki, &devices, &count, err, sizeof(err)) == 0);
  assert(count == 1);
  assert(strcmp(devices[0].serial, serial) == 0);
  assert(devices[0].scope == WD_SCOPE_PHONE);
  assert(!devices[0].revoked);
  free(devices);

  assert(wd_pki_issue(pki, csr, WD_SCOPE_NONE, &cert_pem, serial, err,
                      sizeof(err)) != 0);
  assert(wd_pki_issue(pki, "not a request", WD_SCOPE_PHONE, &cert_pem, serial,
                      err, sizeof(err)) != 0);

  X509_free(cert);
  free(cert_pem);
  free(key);
  free(csr);

  assert(wd_pki_revoke(pki, serial, err, sizeof(err)) == 0);
  assert(wd_pki_revoke(pki, serial, err, sizeof(err)) == 0);
  assert(wd_pki_revoke(pki, "00AA", err, sizeof(err)) != 0);

  wd_pki_free(pki);

  pki = wd_pki_open(dir, &open_err);
  assert(pki != NULL);
  char again[65];
  assert(wd_pki_fingerprint(pki, again) == 0);
  assert(strcmp(again, fingerprint) == 0);

  char *fresh_key = NULL;
  char *fresh_csr = NULL;
  assert(wd_pki_keypair(&fresh_key, &fresh_csr, err, sizeof(err)) == 0);
  char *fresh_pem = NULL;
  char fresh_serial[WD_PKI_SERIAL_MAX] = {0};
  assert(wd_pki_issue(pki, fresh_csr, WD_SCOPE_DESKTOP, &fresh_pem,
                      fresh_serial, err, sizeof(err)) == 0);
  X509 *fresh = parse_cert(fresh_pem);
  assert(fresh != NULL);
  scope = WD_SCOPE_NONE;
  assert(wd_pki_peer_check(pki, fresh, &scope, seen, err, sizeof(err)) == 0);
  assert(scope == WD_SCOPE_DESKTOP);

  assert(wd_pki_revoke(pki, fresh_serial, err, sizeof(err)) == 0);
  assert(wd_pki_peer_check(pki, fresh, &scope, seen, err, sizeof(err)) != 0);
  assert(strstr(err, "revoked") != NULL);

  X509_free(fresh);
  free(fresh_pem);
  free(fresh_key);
  free(fresh_csr);

  char other_template[] = "/tmp/ward-pki-other-XXXXXX";
  char *other_dir = mkdtemp(other_template);
  assert(other_dir != NULL);
  char *other_err = NULL;
  wd_pki *other = wd_pki_open(other_dir, &other_err);
  assert(other != NULL);
  char *alien_key = NULL;
  char *alien_csr = NULL;
  assert(wd_pki_keypair(&alien_key, &alien_csr, err, sizeof(err)) == 0);
  char *alien_pem = NULL;
  char alien_serial[WD_PKI_SERIAL_MAX] = {0};
  assert(wd_pki_issue(other, alien_csr, WD_SCOPE_READONLY, &alien_pem,
                      alien_serial, err, sizeof(err)) == 0);
  X509 *alien = parse_cert(alien_pem);
  assert(alien != NULL);
  assert(wd_pki_peer_check(pki, alien, &scope, seen, err, sizeof(err)) != 0);
  X509_free(alien);
  free(alien_pem);
  free(alien_key);
  free(alien_csr);
  wd_pki_free(other);
  purge(other_dir);

  wd_pki_free(pki);
  purge(dir);
  printf("pki tests passed\n");
  return 0;
}
