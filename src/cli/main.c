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

#include "core/cgroup.h"
#include "client/client.h"
#include "core/util.h"
#include "daemon/server.h"
#include "proto/codec.h"
#include "proto/frame.h"

static void usage(FILE *out) {
  fprintf(out,
          "usage: ward <command>\n"
          "\n"
          "  up      [--socket PATH] [--state DIR]\n"
          "  -p TEXT [--session ID] [--cwd DIR] [--socket PATH]\n"
          "  version\n"
          "  session create [--cwd DIR] [--model NAME] [--json]\n"
          "  session list   [--limit N] [--json]\n"
          "  session get    <id> [--json]\n"
          "  session prompt <id> <text>\n"
          "  session resume <id> [--from SEQ] [--follow] [--json]\n"
          "  session approve <id> --call N --epoch N (--allow|--deny)\n"
          "  session cancel <id>\n"
          "  session rename <id> <name>\n"
          "  session pin|archive <id> on|off\n"
          "  session search <query> [--json]\n"
          "  session fork   <id> [--from SEQ] [--json]\n"
          "  session set-model <id> <model>\n"
          "  session set-system <id> <text>\n"
          "  session compact <id>\n"
          "  model list     [--json]\n"
          "  model probe    <id> [--json]\n"
          "  model select   <id>\n"
          "  prompt <id>    [--json]  show the assembled system prompt\n"
          "  stats          [<id>] [--json]\n"
          "  job list|kill  [<id>] [--json]\n"
          "  device pair    [--scope S] [--json]\n"
          "  device list    [--json]\n"
          "  device revoke  <serial>\n"
          "\n"
          "socket defaults to $XDG_RUNTIME_DIR/ward.sock or /tmp/ward-$UID.sock\n");
}

static char *socket_from_args(int argc, char **argv) {
  for (int i = 0; i < argc; i++)
    if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      return strdup(argv[i + 1]);
  const char *env = getenv("WARD_SOCKET");
  if (env != NULL && env[0] != '\0') return strdup(env);
  return wd_default_socket_path();
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
  char *name = wd_text_copy(wd_get(session, "name"));
  bool archived = false;
  bool pinned = false;
  wd_bool_of(wd_get(session, "archived"), &archived);
  wd_bool_of(wd_get(session, "pinned"), &pinned);
  uint64_t created = 0;
  wd_uint_of(wd_get(session, "created"), &created);
  printf("%-16s %s %-20s %-20s %s%s%s\n", id ? id : "?",
         format_time((int64_t)created), model ? model : "?",
         name != NULL ? name : "", cwd ? cwd : "",
         pinned ? " pinned" : "", archived ? " archived" : "");
  free(id);
  free(cwd);
  free(model);
  free(name);
}

static long long parse_size(const char *text, long long fallback) {
  if (text == NULL || text[0] == '\0') return fallback;
  char *end = NULL;
  long long value = strtoll(text, &end, 10);
  if (end == text || value <= 0) return fallback;
  if (*end == 'k' || *end == 'K')
    value *= 1024;
  else if (*end == 'm' || *end == 'M')
    value *= 1024 * 1024;
  else if (*end == 'g' || *end == 'G')
    value *= 1024 * 1024 * 1024;
  return value;
}

static int enter_cgroup(void) {
  const char *base = getenv("WARD_CGROUP");
  if (base == NULL) base = "/sys/fs/cgroup/ward";
  if (strcmp(base, "off") == 0 || base[0] == '\0') return 0;
  long long max = parse_size(getenv("WARD_CGROUP_MAX"), 2ll << 30);
  const char *high_env = getenv("WARD_CGROUP_HIGH");
  long long high = high_env != NULL ? parse_size(high_env, max) : max;
  long long pids = parse_size(getenv("WARD_CGROUP_PIDS"), 256);
  wd_cgroup_config config = {base, "harness", high, max, pids};
  char *err = NULL;
  if (wd_cgroup_enter(&config, &err) != 0) {
    fprintf(stderr, "ward: cgroup: %s\n", err != NULL ? err : "cannot enter");
    free(err);
    return -1;
  }
  return 0;
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
  if (enter_cgroup() != 0) return 1;
  char *socket_default = NULL;
  char *state_default = NULL;
  if (socket_path == NULL) {
    socket_default = wd_default_socket_path();
    socket_path = socket_default;
  }
  if (state_dir == NULL) {
    state_default = wd_default_state_dir();
    state_dir = state_default;
  }
  wd_up_config cfg = {.socket_path = socket_path, .state_dir = state_dir};
  int rc = wd_up_run(&cfg);
  wd_cgroup_leave();
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
  uint64_t call = 0;
  uint64_t epoch = 0;
  int allow = -1;
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
    } else if (strcmp(argv[i], "--call") == 0 && i + 1 < argc) {
      call = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--epoch") == 0 && i + 1 < argc) {
      epoch = strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--allow") == 0) {
      allow = 1;
    } else if (strcmp(argv[i], "--deny") == 0) {
      allow = 0;
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
  if (strcmp(sub, "create") == 0) {
    cbor_item_t *params = wd_map(2);
    if (cwd != NULL) wd_put(params, "cwd", wd_text(cwd));
    if (model != NULL) wd_put(params, "model", wd_text(model));
    if (wd_client_call(&client, "session.create", params, &result) != 0) goto done;
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
    if (wd_client_call(&client, "session.list", params, &result) != 0) goto done;
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
    if (wd_client_call(&client, "session.get", params, &result) != 0) goto done;
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
    if (wd_client_call(&client, "session.resume", params, &result) != 0) goto done;
    if (json) print_json(result);
    for (;;) {
      struct pollfd pfd = {.fd = client.fd, .events = POLLIN};
      int ready = poll(&pfd, 1, follow || have_from ? 1000 : 0);
      if (ready <= 0) break;
      wd_frame frame;
      uint8_t *body = wd_client_read_frame(client.fd, &frame);
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
  } else if (strcmp(sub, "rename") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session rename needs an id and a name\n");
      goto done;
    }
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, "name", wd_text(text));
    if (wd_client_call(&client, "session.rename", params, &result) != 0)
      goto done;
    printf("renamed %s\n", text);
    rc = 0;
  } else if (strcmp(sub, "pin") == 0 || strcmp(sub, "archive") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session %s needs an id and on or off\n", sub);
      goto done;
    }
    bool on = strcmp(text, "on") == 0;
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, strcmp(sub, "pin") == 0 ? "pinned" : "archived",
           cbor_build_bool(on));
    char method[32];
    snprintf(method, sizeof(method), "session.%s", sub);
    if (wd_client_call(&client, method, params, &result) != 0) goto done;
    printf("%s %s\n", sub, on ? "on" : "off");
    rc = 0;
  } else if (strcmp(sub, "search") == 0) {
    if (text == NULL) {
      fprintf(stderr, "ward: session search needs a query\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "query", wd_text(text));
    if (wd_client_call(&client, "session.search", params, &result) != 0)
      goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *sessions = wd_get(result, "sessions");
      size_t n = cbor_array_size(sessions);
      cbor_item_t **items = cbor_array_handle(sessions);
      if (n == 0) printf("no matches\n");
      for (size_t i = 0; i < n; i++) print_session_line(items[i]);
    }
    rc = 0;
  } else if (strcmp(sub, "cancel") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: session cancel needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (wd_client_call(&client, "session.cancel", params, &result) != 0)
      goto done;
    bool cancelled = false;
    wd_bool_of(wd_get(result, "cancelled"), &cancelled);
    printf(cancelled ? "cancelled\n" : "nothing to cancel\n");
    rc = 0;
  } else if (strcmp(sub, "fork") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: session fork needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(have_from ? 2 : 1);
    wd_put(params, "id", wd_text(id));
    if (have_from) wd_put(params, "from", wd_uint(from));
    if (wd_client_call(&client, "session.fork", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      char *forked = wd_text_copy(wd_get(wd_get(result, "session"), "id"));
      printf("forked %s\n", forked != NULL ? forked : "?");
      free(forked);
    }
    rc = 0;
  } else if (strcmp(sub, "set-model") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session set-model needs an id and a model\n");
      goto done;
    }
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, "model", wd_text(text));
    if (wd_client_call(&client, "session.set_model", params, &result) != 0)
      goto done;
    printf("model %s\n", text);
    rc = 0;
  } else if (strcmp(sub, "compact") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: session compact needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (wd_client_call(&client, "session.compact", params, &result) != 0)
      goto done;
    char *branch = wd_text_copy(wd_get(result, "session"));
    char *parent = wd_text_copy(wd_get(result, "parent"));
    char *summary = wd_text_copy(wd_get(result, "summary"));
    printf("compacted %s into %s\n%s\n", parent != NULL ? parent : "?",
           branch != NULL ? branch : "?", summary != NULL ? summary : "");
    free(branch);
    free(parent);
    free(summary);
    rc = 0;
  } else if (strcmp(sub, "set-system") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session set-system needs an id and text\n");
      goto done;
    }
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, "text", wd_text(text));
    if (wd_client_call(&client, "session.set_system", params, &result) != 0)
      goto done;
    printf("system prompt set\n");
    rc = 0;
  } else if (strcmp(sub, "prompt") == 0) {
    if (id == NULL || text == NULL) {
      fprintf(stderr, "ward: session prompt needs an id and text\n");
      goto done;
    }
    cbor_item_t *focus = wd_map(1);
    wd_put(focus, "id", wd_text(id));
    if (wd_client_call(&client, "session.resume", focus, &result) != 0) goto done;
    wd_free(result);
    result = NULL;
    uint64_t req = client.next_id++;
    cbor_item_t *params = wd_map(2);
    wd_put(params, "id", wd_text(id));
    wd_put(params, "text", wd_text(text));
    if (wd_client_send_item(&client, wd_request(req, "session.prompt", params)) != 0)
      goto done;
    for (;;) {
      wd_frame frame;
      uint8_t *body = wd_client_read_frame(client.fd, &frame);
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
      cbor_item_t *pending = wd_get(value, "approval");
      if (pending != NULL) {
        uint64_t pending_call = 0;
        uint64_t pending_epoch = 0;
        wd_uint_of(wd_get(pending, "call"), &pending_call);
        wd_uint_of(wd_get(pending, "epoch"), &pending_epoch);
        char *reason = wd_text_copy(wd_get(pending, "reason"));
        printf("approval %llu %llu %s\n", (unsigned long long)pending_call,
               (unsigned long long)pending_epoch,
               reason != NULL ? reason : "");
        free(reason);
      }
      wd_free(root);
      rc = 0;
      break;
    }
  } else if (strcmp(sub, "approve") == 0) {
    if (id == NULL || allow < 0) {
      fprintf(stderr, "ward: session approve needs an id and --allow or --deny\n");
      goto done;
    }
    cbor_item_t *focus = wd_map(1);
    wd_put(focus, "id", wd_text(id));
    if (wd_client_call(&client, "session.resume", focus, &result) != 0) goto done;
    wd_free(result);
    result = NULL;
    uint64_t req = client.next_id++;
    cbor_item_t *params = wd_map(4);
    wd_put(params, "session", wd_text(id));
    wd_put(params, "call", wd_uint(call));
    wd_put(params, "epoch", wd_uint(epoch));
    wd_put(params, "allow", cbor_build_bool(allow == 1));
    if (wd_client_send_item(&client, wd_request(req, "approval.respond", params)) !=
        0)
      goto done;
    rc = 0;
    for (;;) {
      wd_frame frame;
      uint8_t *body = wd_client_read_frame(client.fd, &frame);
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
        }
        free(kind);
        wd_free(root);
        if (end) {
          if (!json) putchar('\n');
          break;
        }
        continue;
      }
      uint64_t got = 0;
      bool ok = false;
      cbor_item_t *value = NULL;
      if (wd_read_response(root, &got, &ok, &value) && got == req && !ok) {
        char *name = wd_text_copy(wd_get(value, "name"));
        char *message = wd_text_copy(wd_get(value, "message"));
        fprintf(stderr, "ward: %s: %s\n", name != NULL ? name : "error",
                message != NULL ? message : "");
        free(name);
        free(message);
        rc = 1;
      }
      wd_free(root);
      if (rc != 0) break;
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

static int command_job(int argc, char **argv) {
  const char *sub = argc > 0 ? argv[0] : "list";
  const char *job = argc > 1 ? argv[1] : NULL;
  bool json = false;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) json = true;
  }
  char *path = socket_from_args(argc, argv);
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
  if (strcmp(sub, "list") == 0) {
    if (wd_client_call(&client, "job.list", wd_map(0), &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *jobs = wd_get(result, "jobs");
      size_t n = cbor_array_size(jobs);
      cbor_item_t **items = cbor_array_handle(jobs);
      if (n == 0) printf("no jobs running\n");
      for (size_t i = 0; i < n; i++) {
        char *id = wd_text_copy(wd_get(items[i], "job"));
        char *state = wd_text_copy(wd_get(items[i], "state"));
        printf("%-16s %s\n", id != NULL ? id : "?",
               state != NULL ? state : "?");
        free(id);
        free(state);
      }
    }
    rc = 0;
  } else if (strcmp(sub, "kill") == 0) {
    if (job == NULL) {
      fprintf(stderr, "ward: job kill needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "job", wd_text(job));
    if (wd_client_call(&client, "job.kill", params, &result) != 0) goto done;
    bool killed = false;
    wd_bool_of(wd_get(result, "killed"), &killed);
    printf("%s %s\n", job, killed ? "killed" : "not running");
    rc = killed ? 0 : 1;
  } else {
    fprintf(stderr, "ward: job list|kill\n");
  }
done:
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}

static int command_prompt(int argc, char **argv) {
  const char *id = NULL;
  bool json = false;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0)
      json = true;
    else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      i++;
    else if (argv[i][0] != '-')
      id = argv[i];
  }
  if (id == NULL) {
    fprintf(stderr, "ward: prompt needs a session id\n");
    return 2;
  }
  char *path = socket_from_args(argc, argv);
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
  cbor_item_t *params = wd_map(1);
  wd_put(params, "id", wd_text(id));
  if (wd_client_call(&client, "prompt.preview", params, &result) != 0) goto done;
  if (json) {
    print_json(result);
  } else {
    cbor_item_t *sections = wd_get(result, "sections");
    for (size_t i = 0; sections != NULL && i < cbor_array_size(sections); i++) {
      cbor_item_t *item = cbor_array_get(sections, i);
      if (item == NULL) continue;
      char *name = wd_text_copy(wd_get(item, "name"));
      char *source = wd_text_copy(wd_get(item, "source"));
      char *text = wd_text_copy(wd_get(item, "text"));
      printf("%s (%s)\n%s\n\n", name != NULL ? name : "?",
             source != NULL ? source : "?", text != NULL ? text : "");
      free(name);
      free(source);
      free(text);
      cbor_decref(&item);
    }
  }
  rc = 0;
done:
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}

static int command_stats(int argc, char **argv) {
  const char *id = NULL;
  bool json = false;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0)
      json = true;
    else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      i++;
    else if (argv[i][0] != '-')
      id = argv[i];
  }
  char *path = socket_from_args(argc, argv);
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
  if (id != NULL) {
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (wd_client_call(&client, "stats.session", params, &result) != 0)
      goto done;
  } else {
    if (wd_client_call(&client, "stats.global", wd_map(0), &result) != 0)
      goto done;
  }
  if (json) {
    print_json(result);
  } else {
    char *session = wd_text_copy(wd_get(result, "session"));
    char *model = wd_text_copy(wd_get(result, "model"));
    uint64_t sessions = 0, turns = 0, text_bytes = 0, events = 0;
    uint64_t prompt_bytes = 0;
    wd_uint_of(wd_get(result, "sessions"), &sessions);
    wd_uint_of(wd_get(result, "turns"), &turns);
    wd_uint_of(wd_get(result, "text_bytes"), &text_bytes);
    wd_uint_of(wd_get(result, "events"), &events);
    wd_uint_of(wd_get(result, "prompt_bytes"), &prompt_bytes);
    if (session != NULL) printf("session     %s\n", session);
    if (model != NULL) printf("model       %s\n", model);
    if (session == NULL) printf("sessions    %llu\n", (unsigned long long)sessions);
    printf("events      %llu\nturns       %llu\ntext bytes  %llu\n"
           "prompt bytes %llu\n",
           (unsigned long long)events, (unsigned long long)turns,
           (unsigned long long)text_bytes, (unsigned long long)prompt_bytes);
    uint64_t prompt_tokens = 0, predicted_tokens = 0, cache_tokens = 0;
    wd_uint_of(wd_get(result, "prompt_tokens"), &prompt_tokens);
    wd_uint_of(wd_get(result, "predicted_tokens"), &predicted_tokens);
    wd_uint_of(wd_get(result, "cache_tokens"), &cache_tokens);
    if (prompt_tokens > 0 || predicted_tokens > 0)
      printf("tokens      %llu in, %llu out, %llu cached (measured)\n",
             (unsigned long long)prompt_tokens,
             (unsigned long long)predicted_tokens,
             (unsigned long long)cache_tokens);
    cbor_item_t *cost = wd_get(result, "cost_total");
    if (cost != NULL && cbor_isa_float_ctrl(cost))
      printf("cost        %.6f (%s tokens)\n", cbor_float_get_float(cost),
             cbor_get_bool(wd_get(result, "tokens_estimated")) ? "estimated"
                                                               : "measured");
    free(session);
    free(model);
  }
  rc = 0;
done:
  if (result != NULL) wd_free(result);
  close(client.fd);
  return rc;
}

static int command_model(int argc, char **argv) {
  if (argc == 0) {
    usage(stderr);
    return 2;
  }
  const char *sub = argv[0];
  bool json = false;
  const char *id = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) {
      json = true;
    } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      i++;
    } else if (argv[i][0] != '-') {
      id = argv[i];
    } else {
      fprintf(stderr, "ward: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  char *path = socket_from_args(argc, argv);
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
  if (strcmp(sub, "list") == 0) {
    if (wd_client_call(&client, "model.list", wd_map(0), &result) != 0)
      goto done;
    if (json) {
      print_json(result);
    } else {
      cbor_item_t *models = wd_get(result, "models");
      size_t n = cbor_array_size(models);
      cbor_item_t **items = cbor_array_handle(models);
      if (n == 0) printf("no models\n");
      for (size_t i = 0; i < n; i++) {
        char *model_id = wd_text_copy(wd_get(items[i], "id"));
        char *provider = wd_text_copy(wd_get(items[i], "provider"));
        char *name = wd_text_copy(wd_get(items[i], "model"));
        char *context = wd_text_copy(wd_get(items[i], "context"));
        printf("%-24s %-12s %-20s %s\n", model_id != NULL ? model_id : "?",
               provider != NULL ? provider : "?", name != NULL ? name : "?",
               context != NULL ? context : "");
        free(model_id);
        free(provider);
        free(name);
        free(context);
      }
    }
    rc = 0;
  } else if (strcmp(sub, "probe") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: model probe needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (wd_client_call(&client, "model.probe", params, &result) != 0) goto done;
    if (json) {
      print_json(result);
    } else {
      uint64_t n_ctx = 0;
      uint64_t n_slots = 0;
      wd_uint_of(wd_get(result, "n_ctx"), &n_ctx);
      wd_uint_of(wd_get(result, "n_slots"), &n_slots);
      char *source = wd_text_copy(wd_get(result, "context_source"));
      printf("%s n_ctx %llu n_slots %llu from %s\n", id,
             (unsigned long long)n_ctx, (unsigned long long)n_slots,
             source != NULL ? source : "?");
      free(source);
    }
    rc = 0;
  } else if (strcmp(sub, "select") == 0) {
    if (id == NULL) {
      fprintf(stderr, "ward: model select needs an id\n");
      goto done;
    }
    cbor_item_t *params = wd_map(1);
    wd_put(params, "id", wd_text(id));
    if (wd_client_call(&client, "model.select", params, &result) != 0) goto done;
    printf("model %s\n", id);
    rc = 0;
  } else {
    fprintf(stderr, "ward: unknown model command %s\n", sub);
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
  if (strcmp(sub, "pair") == 0) {
    cbor_item_t *params = wd_map(1);
    if (scope != NULL) wd_put(params, "scope", wd_text(scope));
    if (wd_client_call(&client, "device.pair", params, &result) != 0) goto done;
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
    if (wd_client_call(&client, "device.list", params, &result) != 0) goto done;
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
    if (wd_client_call(&client, "device.revoke", params, &result) != 0) goto done;
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

static int command_print(int argc, char **argv) {
  const char *text = NULL;
  const char *session = NULL;
  const char *cwd = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      text = argv[++i];
    } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
      session = argv[++i];
    } else if (strcmp(argv[i], "--cwd") == 0 && i + 1 < argc) {
      cwd = argv[++i];
    } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
      i++;
    } else if (argv[i][0] != '-') {
      if (text == NULL)
        text = argv[i];
      else {
        fprintf(stderr, "ward: too many arguments\n");
        return 2;
      }
    } else {
      fprintf(stderr, "ward: unknown option %s\n", argv[i]);
      return 2;
    }
  }
  if (text == NULL) {
    fprintf(stderr, "ward: -p needs a prompt\n");
    return 2;
  }
  char *own_session = NULL;
  if (session == NULL) {
    char *path = socket_from_args(argc, argv);
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
    cbor_item_t *params = wd_map(cwd != NULL ? 1 : 0);
    if (cwd != NULL) wd_put(params, "cwd", wd_text(cwd));
    cbor_item_t *result = NULL;
    int rc = wd_client_call(&client, "session.create", params, &result);
    close(client.fd);
    if (rc != 0) return 1;
    own_session = wd_text_copy(wd_get(wd_get(result, "session"), "id"));
    wd_free(result);
    if (own_session == NULL) return 1;
    session = own_session;
  }
  char *forward[3] = {"prompt", (char *)session, (char *)text};
  int rc = command_session(3, forward);
  free(own_session);
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
  if (strcmp(command, "-p") == 0) return command_print(argc - 1, argv + 1);
  if (strcmp(command, "session") == 0) return command_session(argc - 2, argv + 2);
  if (strcmp(command, "device") == 0) return command_device(argc - 2, argv + 2);
  if (strcmp(command, "model") == 0) return command_model(argc - 2, argv + 2);
  if (strcmp(command, "prompt") == 0) return command_prompt(argc - 2, argv + 2);
  if (strcmp(command, "stats") == 0) return command_stats(argc - 2, argv + 2);
  if (strcmp(command, "job") == 0) return command_job(argc - 2, argv + 2);
  fprintf(stderr, "ward: unknown command %s\n", command);
  usage(stderr);
  return 2;
}
