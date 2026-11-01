/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef WARD_NET_PKI_H
#define WARD_NET_PKI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WD_PKI_SERIAL_MAX 64
#define WD_PKI_TOKEN_MAX 64

enum wd_scope { WD_SCOPE_NONE = 0, WD_SCOPE_READONLY = 1, WD_SCOPE_PHONE = 2,
                WD_SCOPE_DESKTOP = 3 };

typedef struct wd_pki wd_pki;

wd_pki *wd_pki_open(const char *dir, char **err);
void wd_pki_free(wd_pki *pki);

const char *wd_pki_dir(const wd_pki *pki);
const char *wd_pki_server_cert(const wd_pki *pki);
const char *wd_pki_server_key(const wd_pki *pki);
const char *wd_pki_ca_cert(const wd_pki *pki);
const char *wd_pki_crl(const wd_pki *pki);

int wd_pki_fingerprint(wd_pki *pki, char out[65]);

char *wd_pki_token_mint(wd_pki *pki, uint64_t now_ms, uint64_t ttl_ms,
                        int scope);
int wd_pki_token_redeem(wd_pki *pki, const char *token, uint64_t now_ms,
                        int *scope_out);

const char *wd_scope_name(int scope);
int wd_scope_parse(const char *name);

int wd_pki_keypair(char **key_pem, char **csr_pem, char *err, size_t errcap);

int wd_pki_issue(wd_pki *pki, const char *csr_pem, int scope, char **cert_pem,
                 char serial[WD_PKI_SERIAL_MAX], char *err, size_t errcap);

int wd_pki_peer_check(wd_pki *pki, void *x509, int *scope_out,
                      char serial[WD_PKI_SERIAL_MAX], char *err, size_t errcap);

int wd_pki_revoke(wd_pki *pki, const char *serial, char *err, size_t errcap);

typedef struct {
  char serial[WD_PKI_SERIAL_MAX];
  char name[64];
  int scope;
  uint64_t issued_ms;
  bool revoked;
} wd_pki_device;

int wd_pki_devices(wd_pki *pki, wd_pki_device **out, size_t *count, char *err,
                   size_t errcap);

#endif
