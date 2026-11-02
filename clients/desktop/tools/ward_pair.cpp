/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "client/client.h"
#include "core/util.h"
#include "net/pki.h"
#include "net/quic.h"
#include "proto/codec.h"
}

namespace {

struct EnrolState {
    bool welcome = false;
    bool response = false;
    bool ok = false;
    cbor_item_t *result = nullptr;
};

void frameCallback(void *ctx, const wd_frame *frame) {
    EnrolState *state = static_cast<EnrolState *>(ctx);
    cbor_item_t *root = wd_decode(frame->body, frame->len);
    if (root == nullptr)
        return;
    if (frame->type == WD_FRAME_WELCOME) {
        unsigned version = 0;
        uint64_t uid = 0;
        state->welcome = wd_read_welcome(root, &version, &uid)
                      && version == WD_PROTOCOL_VERSION;
    } else if (frame->type == WD_FRAME_RESPONSE) {
        uint64_t id = 0;
        bool ok = false;
        cbor_item_t *value = nullptr;
        if (wd_read_response(root, &id, &ok, &value) && id == 1) {
            state->response = true;
            state->ok = ok;
            if (ok && value != nullptr && cbor_incref(value))
                state->result = value;
        }
    }
    wd_free(root);
}

int sendFrame(wd_quic_client *client, uint16_t type, cbor_item_t *body) {
    size_t len = 0;
    uint8_t *bytes = wd_encode(body, &len);
    if (bytes == nullptr)
        return -1;
    uint8_t header[WD_FRAME_HEADER];
    wd_frame_header(header, type, 0, 0, uint32_t(len));
    int rc = wd_quic_client_write(client, header, sizeof(header));
    if (rc == 0)
        rc = wd_quic_client_write(client, bytes, len);
    if (rc == 0)
        rc = wd_quic_client_flush(client);
    free(bytes);
    return rc;
}

bool waitFor(wd_quic_client *client, const bool *flag, uint64_t deadline) {
    for (;;) {
        if (*flag)
            return true;
        if (wd_quic_client_state(client) == WD_QUIC_CLOSED)
            return *flag;
        if (wd_now_ms() > deadline)
            return false;
        (void)wd_quic_client_service(client, wd_now_ms());
        (void)wd_quic_client_flush(client);
        struct pollfd pfd = {wd_quic_client_fd(client), POLLIN, 0};
        (void)poll(&pfd, 1, 50);
    }
}

bool waitEstablished(wd_quic_client *client, uint64_t deadline) {
    for (;;) {
        if (wd_quic_client_state(client) == WD_QUIC_ESTABLISHED)
            return true;
        if (wd_quic_client_state(client) == WD_QUIC_CLOSED)
            return false;
        if (wd_now_ms() > deadline)
            return false;
        (void)wd_quic_client_service(client, wd_now_ms());
        (void)wd_quic_client_flush(client);
        struct pollfd pfd = {wd_quic_client_fd(client), POLLIN, 0};
        (void)poll(&pfd, 1, 50);
    }
}

bool splitEndpoint(const char *endpoint, std::string *host, uint16_t *port) {
    const char *colon = strrchr(endpoint, ':');
    if (colon == nullptr)
        return false;
    host->assign(endpoint, size_t(colon - endpoint));
    *port = uint16_t(atoi(colon + 1));
    return !host->empty() && *port != 0;
}

bool writeFile(const std::string &path, const char *data, mode_t mode) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return false;
    size_t len = strlen(data);
    ssize_t written = write(fd, data, len);
    close(fd);
    return written == ssize_t(len);
}

} // namespace

int main(int argc, char **argv) {
    std::string socket_path;
    std::string out_dir;
    std::string scope = "desktop";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_path = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (strcmp(argv[i], "--scope") == 0 && i + 1 < argc)
            scope = argv[++i];
        else {
            fprintf(stderr, "usage: ward-pair [--socket path] [--out dir] [--scope readonly|phone|desktop]\n");
            return 2;
        }
    }
    if (out_dir.empty()) {
        const char *base = getenv("XDG_DATA_HOME");
        std::string root = base != nullptr && base[0] != '\0'
                               ? std::string(base)
                               : std::string(getenv("HOME") != nullptr ? getenv("HOME") : ".") + "/.local/share";
        out_dir = root + "/ward-gui/quic";
    }
    if (socket_path.empty()) {
        char *def = wd_default_socket_path();
        if (def == nullptr) {
            fprintf(stderr, "ward-pair: no socket; pass --socket\n");
            return 1;
        }
        socket_path = def;
        free(def);
    }

    wd_client local;
    if (wd_client_connect(&local, socket_path.c_str()) != 0 ||
        wd_client_handshake(&local) != 0) {
        fprintf(stderr, "ward-pair: cannot reach the local daemon at %s\n", socket_path.c_str());
        return 1;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "scope", wd_text(scope.c_str()));
    cbor_item_t *paired = nullptr;
    if (wd_client_call(&local, "device.pair", params, &paired) != 0) {
        fprintf(stderr, "ward-pair: device.pair failed\n");
        close(local.fd);
        return 1;
    }
    char *token = wd_text_copy(wd_get(paired, "token"));
    char *pin = wd_text_copy(wd_get(paired, "fingerprint"));
    char *endpoint = wd_text_copy(wd_get(paired, "endpoint"));
    std::string host;
    uint16_t port = 0;
    const bool endpoint_ok = endpoint != nullptr && splitEndpoint(endpoint, &host, &port);
    wd_free(paired);
    close(local.fd);
    if (token == nullptr || pin == nullptr || !endpoint_ok) {
        fprintf(stderr, "ward-pair: device.pair answered an unusable endpoint\n");
        free(token);
        free(pin);
        free(endpoint);
        return 1;
    }

    char failure[256] = "";
    char *key_pem = nullptr;
    char *csr_pem = nullptr;
    if (wd_pki_keypair(&key_pem, &csr_pem, failure, sizeof(failure)) != 0) {
        fprintf(stderr, "ward-pair: cannot generate a keypair: %s\n", failure);
        free(token);
        free(pin);
        free(endpoint);
        return 1;
    }

    EnrolState state;
    wd_quic_client_config cfg = {};
    cfg.pin_sha256 = pin;
    cfg.server_name = "ward";
    cfg.frame = frameCallback;
    cfg.ctx = &state;
    char *err = nullptr;
    wd_quic_client *quic = wd_quic_client_new(&cfg, &err);
    if (quic == nullptr) {
        fprintf(stderr, "ward-pair: cannot create the quic client: %s\n", err != nullptr ? err : "failed");
        free(err);
        free(token);
        free(pin);
        free(endpoint);
        free(key_pem);
        free(csr_pem);
        return 1;
    }
    free(err);
    if (wd_quic_client_start(quic, host.c_str(), port, wd_now_ms()) != 0 ||
        !waitEstablished(quic, wd_now_ms() + 5000) ||
        sendFrame(quic, WD_FRAME_HELLO, wd_hello()) != 0 ||
        !waitFor(quic, &state.welcome, wd_now_ms() + 5000)) {
        fprintf(stderr, "ward-pair: quic handshake failed\n");
        wd_quic_client_free(quic);
        free(token);
        free(pin);
        free(endpoint);
        free(key_pem);
        free(csr_pem);
        return 1;
    }
    cbor_item_t *enrol = wd_map(2);
    wd_put(enrol, "token", wd_text(token));
    wd_put(enrol, "csr", wd_text(csr_pem));
    if (sendFrame(quic, WD_FRAME_REQUEST, wd_request(1, "device.enrol", enrol)) != 0 ||
        !waitFor(quic, &state.response, wd_now_ms() + 5000) || !state.ok) {
        fprintf(stderr, "ward-pair: device.enrol refused\n");
        wd_quic_client_free(quic);
        free(token);
        free(pin);
        free(endpoint);
        free(key_pem);
        free(csr_pem);
        if (state.result != nullptr) wd_free(state.result);
        return 1;
    }
    char *cert_pem = wd_text_copy(wd_get(state.result, "cert"));
    char *ca_pem = wd_text_copy(wd_get(state.result, "ca"));
    char *serial = wd_text_copy(wd_get(state.result, "serial"));
    wd_free(state.result);
    wd_quic_client_free(quic);
    free(token);
    free(pin);
    free(endpoint);
    free(csr_pem);
    if (cert_pem == nullptr || ca_pem == nullptr) {
        fprintf(stderr, "ward-pair: device.enrol returned no certificate\n");
        free(key_pem);
        free(cert_pem);
        free(ca_pem);
        free(serial);
        return 1;
    }

    if (mkdir(out_dir.c_str(), 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "ward-pair: cannot create %s\n", out_dir.c_str());
        free(key_pem);
        free(cert_pem);
        free(ca_pem);
        free(serial);
        return 1;
    }
    const std::string key_path = out_dir + "/device.key";
    const std::string cert_path = out_dir + "/device.crt";
    const std::string ca_path = out_dir + "/ca.crt";
    if (!writeFile(key_path, key_pem, 0600) || !writeFile(cert_path, cert_pem, 0644)
            || !writeFile(ca_path, ca_pem, 0644)) {
        fprintf(stderr, "ward-pair: cannot write the credentials into %s\n", out_dir.c_str());
        free(key_pem);
        free(cert_pem);
        free(ca_pem);
        free(serial);
        return 1;
    }
    printf("paired %s\nkey %s\ncert %s\nca %s\n",
           serial != nullptr ? serial : "", key_path.c_str(), cert_path.c_str(),
           ca_path.c_str());
    free(key_pem);
    free(cert_pem);
    free(ca_pem);
    free(serial);
    return 0;
}
