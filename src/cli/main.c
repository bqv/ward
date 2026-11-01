/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cbor.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "core/util.h"
#include "daemon/server.h"
#include "proto/codec.h"
#include "proto/frame.h"

typedef struct {
  int fd;
  uint64_t next_id;
} wd_client;

static void usage(FILE *out) {
  fprintf(out,
          "usage: ward <command>\n"
          "\n"
          "  up      [--socket PATH] [--state DIR]\n"
          "  version\n"
          "  session create [--cwd DIR] [--model NAME] [--json]\n"
          "  session list   [--limit N] [--json]\n"
          "  session get    <id> [--json]\n"
          "  session resume <id> [--from SEQ] [--follow] [--json]\n"
          "  device pair    [--scope S] [--json]\n"
          "  device list    [--json]\n"
          "  device revoke  <serial>\n"
          "\n"
          "socket defaults to $XDG_RUNTIME_DIR/ward.sock or /tmp/ward-$UID.sock\n");
}

static int read_full(int fd, void *buf, size_t len) {
  size_t off = 0;
  uint8_t *p = buf;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n == 0) return -1;
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static int write_full(int fd, const void *buf, size_t len) {
  return wu_write_all(fd, buf, len);
}

static char *socket_from_args(int argc, char **argv) {
  for (int i = 0; i < argc; i++)
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      return strdup(argv[i + 1]);
  const char *env = getenv("WARD_SOCKET");
  if (env != NULL && env[0] != '\0') return strdup(env);
  return wu_default_socket_path();
}

static int client_connect(wd_client *client, const char *path) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    fprintf(stderr, "ward: socket path too long\n");
    return -1;
  }
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "ward: no daemon at %s: %s\n", path, strerror(errno));
    close(fd);
    return -1;
  }
  client->fd = fd;
  client->next_id = 1;
  return 0;
}

static int client_send_item(wd_client *client, cbor_item_t *item) {
  size_t len = 0;
  uint8_t *body = wd_encode(item, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, WD_FRAME_REQUEST, 0, 0, (uint32_t)len);
  int rc = write_full(client->fd, header, sizeof(header));
  if (rc == 0) rc = write_full(client->fd, body, len);
  free(body);
  return rc;
}

static uint8_t *client_read_frame(int fd, wd_frame *frame) {
  uint8_t header[WD_FRAME_HEADER];
  if (read_full(fd, header, sizeof(header)) != 0) return NULL;
  uint32_t len = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                 ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
  if (len == 0 || len > WD_MAX_BODY) return NULL;
  uint8_t *body = malloc(len);
  if (body == NULL) return NULL;
  if (read_full(fd, body, len) != 0) {
    free(body);
    return NULL;
  }
  frame->type = (uint16_t)((uint16_t)header[4] | ((uint16_t)header[5] << 8));
  frame->sub = 0;
  frame->seq = 0;
  for (unsigned i = 0; i < 8; i++) {
    frame->sub |= (uint64_t)header[6 + i] << (8 * i);
    frame->seq |= (uint64_t)header[14 + i] << (8 * i);
  }
  frame->len = len;
  frame->body = body;
  return body;
}

static int client_handshake(wd_client *client) {
  cbor_item_t *hello = wd_hello();
  size_t len = 0;
  uint8_t *body = wd_encode(hello, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, WD_FRAME_HELLO, 0, 0, (uint32_t)len);
  int rc = write_full(client->fd, header, sizeof(header));
  if (rc == 0) rc = write_full(client->fd, body, len);
  free(body);
  if (rc != 0) return -1;
  wd_frame frame;
  uint8_t *frame_body = client_read_frame(client->fd, &frame);
  if (frame_body == NULL) return -1;
  cbor_item_t *root = wd_decode(frame_body, frame.len);
  free(frame_body);
  if (root == NULL) return -1;
  unsigned version = 0;
  uint64_t uid = 0;
  int ok = frame.type == WD_FRAME_WELCOME &&
           wd_read_welcome(root, &version, &uid) && version == WD_PROTOCOL_VERSION;
  if (!ok) {
    char *name = wd_text_copy(wd_get(root, "error"));
    if (name != NULL) free(name);
    fprintf(stderr, "ward: handshake refused\n");
  }
  wd_free(root);
  return ok ? 0 : -1;
}

static int client_request(wd_client *client, const char *method,
                          cbor_item_t *params, cbor_item_t **result) {
  uint64_t id = client->next_id++;
  if (client_send_item(client, wd_request(id, method, params)) != 0) return -1;
  for (;;) {
    wd_frame frame;
    uint8_t *body = client_read_frame(client->fd, &frame);
    if (body == NULL) return -1;
    cbor_item_t *root = wd_decode(body, frame.len);
    free(body);
    if (root == NULL) return -1;
    if (frame.type == WD_FRAME_EVENT) {
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (wd_read_event(root, &kind, &payload) && strcmp(kind, "replay.end") != 0)
        continue;
      free(kind);
      wd_free(root);
      continue;
    }
    if (frame.type != WD_FRAME_RESPONSE) {
      wd_free(root);
      return -1;
    }
    uint64_t got = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (!wd_read_response(root, &got, &ok, &value) || got != id) {
      wd_free(root);
      return -1;
    }
    if (!ok) {
      char *name = wd_text_copy(wd_get(value, "name"));
      char *message = wd_text_copy(wd_get(value, "message"));
      fprintf(stderr, "ward: %s: %s\n", name != NULL ? name : "error",
              message != NULL ? message : "");
      free(name);
      free(message);
      wd_free(root);
      return -1;
    }
    (void)cbor_incref(value);
    wd_free(root);
    *result = value;
    return 0;
  }
}

static void print_json(cbor_item_t *item) {
  char buffer[65536];
  wd_json(item, buffer, sizeof(buffer));
  printf("%s\n", buffer);
}

static const char *format_time(int64_t ms) {
  static char buffer[32];
  time_t seconds = (time_t)(ms / 1000);
  struct tm tm;
  localtime_r(&seconds, &tm);
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &tm);
  return buffer;
}

static void print_session_line(cbor_item_t *session) {
  char *id = wd_text_copy(wd_get(session, "id"));
  char *cwd = wd_text_copy(wd_get(session, "cwd"));
  char *model = wd_text_copy(wd_get(session, "model"));
  uint64_t created = 0;
  wd_uint_of(wd_get(session, "created"), &created);
  printf("%-16s %s %-20s %s\n", id ? id : "?", format_time((int64_t)created),
         model ? model : "?", cwd ? cwd : "?");
  free(id);
  free(cwd);
  free(model);
}

static int do_up(int argc, char **argv) {
  const char *socket_path = NULL;
  const char *state_dir = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      socket_path = argv[++i];
    } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
      state_dir = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      usage(stdout);
      return 0;
    } else {
      fprintf(stderr, "ward: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  char *socket_default = NULL;
  char *state_default = NULL;
  if (socket_path == NULL) {
    socket_default = wu_default_socket_path();
    socket_path = socket_default;
  }
  if (state_dir == NULL) {
    state_default = wu_default_state_dir();
    state_dir = state_default;
  }
  wd_up_config cfg = {.socket_path = socket_path, .state_dir = state_dir};
  int rc = wd_up_run(&cfg);
  free(socket_default);
  free(state_default);
  return rc == 0 ? 0 : 1;
}

static int command_session(int argc, char **argv) {
  if (argc == 0) {
    usage(stderr);
    return 2;
  }
  const char *sub = argv[0];
  bool json = false;
  bool follow = false;
  const char *id = NULL;
  const char *text = NULL;
  const char *cwd = NULL;
  const char *model = NULL;
  uint64_t from = 0;
  bool have_from = false;
  uint64_t limit = 100;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      json = true;
    } else if (strcmp(argv[i], "--follow") == 0) {
      follow = true;
    } else if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
      cwd = argv[++i];
    } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
      model = argv[++i];
    } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
      from = strtoull(argv[++i], NULL, 10);
      have_from = true;
    } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
      limit = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      i++;
    } else if (argv[i][0] != '-') {
      if (id == NULL) {
        id = argv[i];
      } else if (text == NULL) {
        text = argv[i];
      } else {
        fprintf(stderr, "ward: too many arguments\n");
        return 2;
      }
    } else {
      fprintf(stderr, "ward: unknown option %s\n", argv[i]);
      return 2;
    }
  }

  char *path = socket_from_args(argc, argv);
  if (path == NULL) return 1;
  wd_client client;
  if (client_connect(&client, path) != 0) {
    free(path);
    return 1;
  }
  free(path);
  if (client_handshake(&client) != 0) {
    close(client.fd);
    return 1;
  }

  int rc = 1;
  cbor_item_t *result = NULL;
  if (strcmp(sub, "create") == 0) {
    cbor_item_t *params = wd_map(2);
    if (cwd != NULL) wd_put(params, "cwd", wd_text(cwd));
    if (model != NULL) wd_put(params, "model", wd_text(model));
    if (client_request(&client, "session.create", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *session = wd_get(result, "session");
      char *created = wd_text_copy(wd_get(session, "id"));
      printf("created %s\n", created != NULL ? created : "?");
      free(created);
    }
    rc = 0;
  } else if (strcmp(sub, "list") == 0) {
    cbor_item_t *params = wd_map(1);
    wd_put(params, "limit", wd_uint(limit));
    if (client_request(&client, "session.list", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *sessions = wd_get(result, "sessions");
      size_t n = cbor_array_size(sessions);
      cbor_item_t **items = cbor_array_handle(sessions);
      for (size_t i = 0; i < n; i++) print_session_line(items[i]);
    }
    rc = 0;
  } else if (strcmp(sub, "get") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: session get needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (client_request(&client, "session.get", params, &result) != 0) goto done;
    cbor_item_t *session = wd_get(result, "session");
    if (json) {
      print_json(session);
    } else {
      char *sid = wd_text_copy(wd_get(session, "id"));
      char *scwd = wd_text_copy(wd_get(session, "cwd"));
      char *smodel = wd_text_copy(wd_get(session, "model"));
      uint64_t created = 0;
      wd_uint_of(wd_get(session, "created"), &created);
      printf("id      %s\ncwd     %s\nmodel   %s\ncreated %s\n", sid ? sid : "?",
             scwd ? scwd : "?", smodel ? smodel : "?",
             format_time((int64_t)created));
      free(sid);
      free(scwd);
      free(smodel);
    }
    rc = 0;
  } else if (strcmp(sub, "resume") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: session resume needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    if (have_from) wd_put(params, "from", wd_uint(from));
    if (client_request(&client, "session.resume", params, &result) != 0) goto done;
    if (json) print_json(result);
    for (;;) {
      struct pollfd pfd = {.fd = client.fd, .events = POLLIN};
      int ready = poll(&pfd, 1, follow || have_from ? 1000 : 0);
      if (ready <= 0) break;
      wd_frame frame;
      uint8_t *body = client_read_frame(client.fd, &frame);
      if (body == NULL) break;
      cbor_item_t *root = wd_decode(body, frame.len);
      free(body);
      if (root == NULL) break;
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (frame.type == WD_FRAME_EVENT && wd_read_event(root, &kind, &payload)) {
        if (json) {
          print_json(root);
        } else {
          printf("%llu %s ", (unsigned long long)frame.seq, kind);
          print_json(payload);
        }
        bool end = strcmp(kind, "replay.end") == 0;
        free(kind);
        wd_free(root);
        if (end) break;
        continue;
      }
      free(kind);
      wd_free(root);
      if (!follow) break;
    }
    rc = 0;
  } else if (strcmp(sub, "prompt") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session prompt needs an id and text\n");
      goto done;
    }
    cbor_item_t *focus = wd_map(1);
    wd_put(focus, "id", wd_text(id));
    if (client_request(&client, "session.resume", focus, &result) != 0) goto done;
    wd_free(result);
    result = NULL;
    uint64_t req = client.next_id++;
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, "text", wd_text(text));
    if (client_send_item(&client, wd_request(req, "session.prompt", params)) != 0)
      goto done;
    for (;;) {
      wd_frame frame;
      uint8_t *body = client_read_frame(client.fd, &frame);
      if (body == NULL) break;
      cbor_item_t *root = wd_decode(body, frame.len);
      free(body);
      if (root == NULL) break;
      if (frame.type == WD_FRAME_EVENT) {
        char *kind = NULL;
        cbor_item_t *payload = NULL;
        bool end = false;
        if (wd_read_event(root, &kind, &payload)) {
          if (json) {
            print_json(root);
          } else if (strcmp(kind, "text.delta") == 0) {
            char *piece = wd_text_copy(wd_get(payload, "text"));
            if (piece != NULL) {
              fputs(piece, stdout);
              fflush(stdout);
              free(piece);
            }
          } else if (strcmp(kind, "turn.end") == 0) {
            end = true;
          }
          free(kind);
          wd_free(root);
          if (end && !json) {
            putchar('\n');
            rc = 0;
            break;
          }
          continue;
        }
        free(kind);
        wd_free(root);
        continue;
      }
      if (frame.type != WD_FRAME_RESPONSE) {
        wd_free(root);
        break;
      }
      uint64_t got = 0;
      bool ok = false;
      cbor_item_t *value = NULL;
      if (!wd_read_response(root, &got, &ok, &value) || got != req) {
        wd_free(root);
        break;
      }
      if (!ok) {
        char *name = wd_text_copy(wd_get(value, "name"));
        char *message = wd_text_copy(wd_get(value, "message"));
        fprintf(stderr, "ward: %s: %s\n", name != NULL ? name : "error",
                message != NULL ? message : "");
        free(name);
        free(message);
        wd_free(root);
        break;
      }
      wd_free(root);
      rc = 0;
      break;
    }
  } else {
    fprintf(stderr, "ward: unknown session command %s\n", sub);
    rc = 2;
  }
done:
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}

static int command_device(int argc, char **argv) {
  if (argc == 0) {
    usage(stderr);
    return 2;
  }
  const char *sub = argv[0];
  bool json = false;
  const char *scope = NULL;
  const char *serial = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      json = true;
    } else if (strcmp(argv[i], "--scope") == 0 && i + 1 < argc) {
      scope = argv[++i];
    } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      i++;
    } else if (argv[i][0] != '-') {
      if (serial == NULL)
        serial = argv[i];
      else {
        fprintf(stderr, "ward: too many arguments\n");
        return 2;
      }
    } else {
      fprintf(stderr, "ward: unknown option %s\n", argv[i]);
      return 2;
    }
  }

  char *path = socket_from_args(argc, argv);
  if (path == NULL) return 1;
  wd_client client;
  if (client_connect(&client, path) != 0) {
    free(path);
    return 1;
  }
  free(path);
  if (client_handshake(&client) != 0) {
    close(client.fd);
    return 1;
  }

  int rc = 1;
  cbor_item_t *result = NULL;
  if (strcmp(sub, "pair") == 0) {
    cbor_item_t *params = wd_map(1);
    if (scope != NULL) wd_put(params, "scope", wd_text(scope));
    if (client_request(&client, "device.pair", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      char *token = wd_text_copy(wd_get(result, "token"));
      char *fingerprint = wd_text_copy(wd_get(result, "fingerprint"));
      char *endpoint = wd_text_copy(wd_get(result, "endpoint"));
      char *granted = wd_text_copy(wd_get(result, "scope"));
      uint64_t expires = 0;
      wd_uint_of(wd_get(result, "expires_ms"), &expires);
      printf("endpoint    %s\nfingerprint %s\nscope       %s\n"
             "token       %s\nexpires     %s\n",
             endpoint != NULL ? endpoint : "?",
             fingerprint != NULL ? fingerprint : "?", granted != NULL ? granted : "?",
             token != NULL ? token : "?", format_time((int64_t)expires));
      printf("pair        %s %s %s\n", endpoint != NULL ? endpoint : "?",
             fingerprint != NULL ? fingerprint : "?", token != NULL ? token : "?");
      free(token);
      free(fingerprint);
      free(endpoint);
      free(granted);
    }
    rc = 0;
  } else if (strcmp(sub, "list") == 0) {
    cbor_item_t *params = wd_map(0);
    if (client_request(&client, "device.list", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *devices = wd_get(result, "devices");
      size_t n = cbor_array_size(devices);
      cbor_item_t **items = cbor_array_handle(devices);
      if (n == 0) printf("no devices\n");
      for (size_t i = 0; i < n; i++) {
        char *dev_serial = wd_text_copy(wd_get(items[i], "serial"));
        char *name = wd_text_copy(wd_get(items[i], "name"));
        char *dev_scope = wd_text_copy(wd_get(items[i], "scope"));
        uint64_t issued = 0;
        wd_uint_of(wd_get(items[i], "issued_ms"), &issued);
        bool revoked = false;
        wd_bool_of(wd_get(items[i], "revoked"), &revoked);
        printf("%-40s %-16s %-9s %s%s\n", dev_serial != NULL ? dev_serial : "?",
               name != NULL ? name : "?", dev_scope != NULL ? dev_scope : "?",
               format_time((int64_t)issued), revoked ? " revoked" : "");
        free(dev_serial);
        free(name);
        free(dev_scope);
      }
    }
    rc = 0;
  } else if (strcmp(sub, "revoke") == 0) {
    if (serial == NULL) {
      fprintf(stderr, "ward: device revoke needs a serial\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "serial", wd_text(serial));
    if (client_request(&client, "device.revoke", params, &result) != 0) goto done;
    printf("revoked %s\n", serial);
    rc = 0;
  } else {
    fprintf(stderr, "ward: unknown device command %s\n", sub);
  }
done:
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(stderr);
    return 2;
  }
  const char *command = argv[1];
  if (strcmp(command, "up") == 0) return do_up(argc - 2, argv + 2);
  if (strcmp(command, "version") == 0) {
    printf("ward %s protocol %d\n", WD_VERSION_TEXT, WD_PROTOCOL_VERSION);
    return 0;
  }
  if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0) {
    usage(stdout);
    return 0;
  }
  if (strcmp(command, "session") == 0) return command_session(argc - 2, argv + 2);
  if (strcmp(command, "device") == 0) return command_device(argc - 2, argv + 2);
  fprintf(stderr, "ward: unknown command %s\n", command);
  usage(stderr);
  return 2;
}
