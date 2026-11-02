/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "client/client.h"
#include "core/util.h"
#include "proto/codec.h"

static void usage(FILE *out) {
  fprintf(out,
          "usage: ward-native <command>\n"
          "\n"
          "  create <cwd>          create a session and print its id\n"
          "  prompt <id> <text>    send a prompt\n"
          "  resume <id> [--follow] print the session's text and stop at turn.end\n"
          "\n"
          "socket defaults to $WARD_SOCKET, then $XDG_RUNTIME_DIR/ward.sock\n");
}

static char *socket_path(void) {
  const char *env = getenv("WARD_SOCKET");
  if (env != NULL && env[0] != '\0') return strdup(env);
  return wd_default_socket_path();
}

static int wait_readable(int fd, int seconds) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  struct timeval timeout = {seconds, 0};
  return select(fd + 1, &set, NULL, NULL, &timeout);
}

static int print_events(int fd, int stop_at_turn_end) {
  for (;;) {
    if (wait_readable(fd, 30) <= 0) return -1;
    wd_frame frame;
    uint8_t *body = wd_client_read_frame(fd, &frame);
    if (body == NULL) return -1;
    cbor_item_t *root = wd_decode(body, frame.len);
    free(body);
    if (root == NULL) return -1;
    if (frame.type == WD_FRAME_EVENT) {
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (wd_read_event(root, &kind, &payload)) {
        if (kind != NULL && strcmp(kind, "text.delta") == 0) {
          char *text = wd_text_copy(wd_get(payload, "text"));
          if (text != NULL) fputs(text, stdout);
          free(text);
        }
        int ended = stop_at_turn_end && kind != NULL &&
                    strcmp(kind, "turn.end") == 0;
        free(kind);
        wd_free(payload);
        wd_free(root);
        if (ended) {
          fputc('\n', stdout);
          return 0;
        }
        continue;
      }
    }
    wd_free(root);
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  char *path = socket_path();
  if (path == NULL) return 1;
  wd_client client;
  if (wd_client_connect(&client, path) != 0) {
    free(path);
    return 1;
  }
  free(path);
  if (wd_client_handshake(&client) != 0) {
    close(client.fd);
    return 1;
  }
  int rc = 1;
  cbor_item_t *result = NULL;
  if (strcmp(argv[1], "create") == 0 && argc >= 3) {
    cbor_item_t *params = wd_map(1);
    wd_put(params, "cwd", wd_text(argv[2]));
    if (wd_client_call(&client, "session.create", params, &result) == 0) {
      cbor_item_t *session = wd_get(result, "session");
      char *id = wd_text_copy(wd_get(session, "id"));
      if (id != NULL) {
        printf("%s\n", id);
        free(id);
        rc = 0;
      }
    }
  } else if (strcmp(argv[1], "prompt") == 0 && argc >= 4) {
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(argv[2]));
    wd_put(params, "text", wd_text(argv[3]));
    if (wd_client_call(&client, "session.prompt", params, &result) == 0) rc = 0;
  } else if (strcmp(argv[1], "resume") == 0 && argc >= 3) {
    cbor_item_t *params = wd_map(3);
    wd_put(params, "id", wd_text(argv[2]));
    wd_put(params, "from", wd_uint(0));
    wd_put(params, "follow", cbor_build_bool(true));
    if (wd_client_call(&client, "session.resume", params, &result) == 0) {
      wd_free(result);
      result = NULL;
      rc = print_events(client.fd, true) == 0 ? 0 : 1;
    }
  } else {
    usage(stderr);
  }
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}
