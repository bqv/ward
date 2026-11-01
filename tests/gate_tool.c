/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/util.h"
#include "fake_endpoint.h"
#include <sys/stat.h>
#include "proto/codec.h"
#include "proto/frame.h"

typedef struct {
  int fd;
  uint64_t next_id;
} client;

typedef struct pending {
  struct pending *next;
  char *kind;
  cbor_item_t *payload;
  uint64_t seq;
} pending;

static pending *queue_head;
static pending *queue_tail;

static void queue_push(char *kind, cbor_item_t *payload, uint64_t seq) {
  pending *item = calloc(1, sizeof(*item));
  assert(item != NULL);
  item->kind = kind;
  item->payload = payload;
  item->seq = seq;
  if (queue_tail != NULL)
    queue_tail->next = item;
  else
    queue_head = item;
  queue_tail = item;
}

static pending *queue_pop(void) {
  pending *item = queue_head;
  if (item == NULL) return NULL;
  queue_head = item->next;
  if (queue_head == NULL) queue_tail = NULL;
  return item;
}

static int read_full(int fd, void *buf, size_t len) {
  size_t off = 0;
  uint8_t *p = buf;
  while (off < len) {
    ssize_t n = read(fd, p + off, len - off);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

static int send_item(int fd, uint16_t type, uint64_t sub, uint64_t seq,
                     cbor_item_t *item) {
  size_t len = 0;
  uint8_t *body = wd_encode(item, &len);
  if (body == NULL) return -1;
  uint8_t header[WD_FRAME_HEADER];
  wd_frame_header(header, type, sub, seq, (uint32_t)len);
  int rc = wd_write_all(fd, header, sizeof(header));
  if (rc == 0) rc = wd_write_all(fd, body, len);
  free(body);
  return rc;
}

static cbor_item_t *next_frame(int fd, wd_frame *frame) {
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
  cbor_item_t *root = wd_decode(body, len);
  free(body);
  return root;
}

static uint64_t last_turn;

static int request(client *c, uint64_t id, const char *method,
                   cbor_item_t *params, cbor_item_t **result, bool *ok_out) {
  if (send_item(c->fd, WD_FRAME_REQUEST, 0, 0, wd_request(id, method, params)) !=
      0)
    return -1;
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(c->fd, &frame);
    if (root == NULL) return -1;
    if (frame.type == WD_FRAME_EVENT) {
      char *kind = NULL;
      cbor_item_t *payload = NULL;
      if (wd_read_event(root, &kind, &payload) && cbor_incref(payload))
        queue_push(kind, payload, frame.seq);
      else
        free(kind);
      wd_free(root);
      continue;
    }
    uint64_t got = 0;
    bool ok = false;
    cbor_item_t *value = NULL;
    if (frame.type != WD_FRAME_RESPONSE ||
        !wd_read_response(root, &got, &ok, &value) || got != id) {
      wd_free(root);
      return -1;
    }
    if (!cbor_incref(value)) {
      wd_free(root);
      return -1;
    }
    wd_free(root);
    *result = value;
    uint64_t turn = 0;
    if (wd_uint_of(wd_get(value, "turn"), &turn)) last_turn = turn;
    if (ok_out != NULL) *ok_out = ok;
    return 0;
  }
}

static int connect_client(client *c, const char *sock, uint64_t first_id) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
  c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (c->fd < 0) return -1;
  if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
  c->next_id = first_id;
  if (send_item(c->fd, WD_FRAME_HELLO, 0, 0, wd_hello()) != 0) return -1;
  wd_frame frame;
  cbor_item_t *welcome = next_frame(c->fd, &frame);
  if (welcome == NULL || frame.type != WD_FRAME_WELCOME) return -1;
  wd_free(welcome);
  return 0;
}

static cbor_item_t *next_event(client *c, char **kind, uint64_t *seq) {
  pending *queued = queue_pop();
  if (queued != NULL) {
    *kind = queued->kind;
    if (seq != NULL) *seq = queued->seq;
    cbor_item_t *payload = queued->payload;
    free(queued);
    return payload;
  }
  for (;;) {
    wd_frame frame;
    cbor_item_t *root = next_frame(c->fd, &frame);
    if (root == NULL) return NULL;
    if (frame.type != WD_FRAME_EVENT) {
      wd_free(root);
      continue;
    }
    cbor_item_t *payload = NULL;
    if (!wd_read_event(root, kind, &payload)) {
      wd_free(root);
      return NULL;
    }
    if (!cbor_incref(payload)) {
      free(*kind);
      wd_free(root);
      return NULL;
    }
    if (seq != NULL) *seq = frame.seq;
    wd_free(root);
    return payload;
  }
}

static cbor_item_t *wait_turn_end(client *c, uint64_t turn) {
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, NULL);
    if (payload == NULL) return NULL;
    if (kind != NULL && strcmp(kind, "turn.end") == 0) {
      uint64_t got = 0;
      wd_uint_of(wd_get(payload, "turn"), &got);
      if (turn == 0 || got == turn) {
        free(kind);
        return payload;
      }
    }
    free(kind);
    wd_free(payload);
  }
}

static void turn_text(client *c, uint64_t turn, char *out, size_t cap) {
  out[0] = '\0';
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, NULL);
    assert(payload != NULL);
    if (kind != NULL && strcmp(kind, "text.delta") == 0) {
      char *text = wd_text_copy(wd_get(payload, "text"));
      if (text != NULL) {
        size_t have = strlen(out);
        snprintf(out + have, cap - have, "%s", text);
        free(text);
      }
      wd_free(payload);
      free(kind);
      continue;
    }
    if (kind != NULL && strcmp(kind, "turn.end") == 0) {
      uint64_t ended = 0;
      if (wd_uint_of(wd_get(payload, "turn"), &ended) && ended == turn) {
        wd_free(payload);
        free(kind);
        return;
      }
    }
    wd_free(payload);
    free(kind);
  }
}

static cbor_item_t *wait_event(client *c, const char *want, uint64_t *seq) {
  for (;;) {
    char *kind = NULL;
    cbor_item_t *payload = next_event(c, &kind, seq);
    if (payload == NULL) return NULL;
    if (kind != NULL && strcmp(kind, want) == 0) {
      free(kind);
      return payload;
    }
    free(kind);
    wd_free(payload);
  }
}

int main(void) {
  alarm(120);
  const char *bin = getenv("WARD_BIN");
  const char *modules = getenv("WARD_MODULE_DIR");
  if (bin == NULL || modules == NULL) {
    return 1;
  }
  int endpoint_port = 0;
  pid_t endpoint = 0;
#ifdef __SANITIZE_ADDRESS__
  setenv("WARD_FAKE_DELAY_MS", "4000", 1);
#else
  setenv("WARD_FAKE_DELAY_MS", "3000", 1);
#endif
  setenv("WARD_MAX_RUNS", "2", 1);
  fake_endpoint(&endpoint_port, &endpoint);
  char root[] = "/tmp/ward-tool-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char rules_path[512];
  snprintf(rules_path, sizeof(rules_path), "%s/AGENTS.md", root);
  FILE *rules = fopen(rules_path, "wb");
  assert(rules != NULL);
  assert(fputs("be kind to the tools\n", rules) >= 0);
  fclose(rules);

  char stub_path[512];
  snprintf(stub_path, sizeof(stub_path), "%s/search-stub", root);
  FILE *stub = fopen(stub_path, "wb");
  assert(stub != NULL);
  assert(fputs("#!/bin/bash\necho 'stub result one'\n", stub) >= 0);
  fclose(stub);
  assert(chmod(stub_path, 0755) == 0);
  setenv("WARD_SEARCH_BIN", stub_path, 1);
  char mcp_path[512];
  snprintf(mcp_path, sizeof(mcp_path), "%s/mcp-stub", root);
  FILE *mcp = fopen(mcp_path, "wb");
  assert(mcp != NULL);
  assert(fputs("#!/bin/bash\nread -r line\necho '{\"result\":\"mcp ok\"}'\n",
               mcp) >= 0);
  fclose(mcp);
  assert(chmod(mcp_path, 0755) == 0);
  setenv("WARD_MCP_BIN", mcp_path, 1);
  char recall_path[512];
  snprintf(recall_path, sizeof(recall_path), "%s/recall-stub", root);
  FILE *recall = fopen(recall_path, "wb");
  assert(recall != NULL);
  assert(fputs("#!/bin/bash\necho '1. score +0.9  ~/ref/NOTE.md remember this'\n",
               recall) >= 0);
  fclose(recall);
  assert(chmod(recall_path, 0755) == 0);
  setenv("WARD_RECALL_BIN", recall_path, 1);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);
  char models_path[512];
  snprintf(models_path, sizeof(models_path), "%s/models.toml", state);
  FILE *models = fopen(models_path, "w");
  assert(models != NULL);
  assert(fputs("[[model]]\nid = \"local:qwen\"\nprovider = \"stadi\"\n"
               "model = \"qwen3\"\ncontext = \"32768\"\n"
               "price = \"0.04,2.0\"\n",
               models) >= 0);
  fclose(models);

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    execl(bin, "ward", "up", "--socket", sock, "--state", state, (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[256];
  size_t used = 0;
  while (used + 1 < sizeof(line)) {
    ssize_t n = read(ready[0], line + used, 1);
    if (n <= 0) break;
    if (line[used++] == '\n') break;
  }
  line[used] = '\0';
  close(ready[0]);
  if (strncmp(line, "ready ", 6) != 0) {
    kill(pid, SIGKILL);
    return 1;
  }

  client a;
  assert(connect_client(&a, sock, 1) == 0);

  cbor_item_t *created = NULL;
  cbor_item_t *create = wd_map(1);
  wd_put(create, "cwd", wd_text(root));
  assert(request(&a, a.next_id++, "session.create", create, &created, NULL) ==
         0);
  char *session = wd_text_copy(wd_get(wd_get(created, "session"), "id"));
  assert(session != NULL);
  wd_free(created);

  bool catalog_ok = false;
  cbor_item_t *listed = NULL;
  assert(request(&a, a.next_id++, "model.list", wd_map(0), &listed, NULL) == 0);
  cbor_item_t *catalog = wd_get(listed, "models");
  assert(cbor_array_size(catalog) == 1);
  char *catalog_id = wd_text_copy(wd_get(cbor_array_handle(catalog)[0], "id"));
  assert(catalog_id != NULL && strcmp(catalog_id, "local:qwen") == 0);
  free(catalog_id);
  wd_free(listed);

  cbor_item_t *chosen = NULL;
  cbor_item_t *set = wd_map(2);
  wd_put(set, "id", wd_text(session));
  wd_put(set, "model", wd_text("local:qwen"));
  assert(request(&a, a.next_id++, "session.set_model", set, &chosen,
                 &catalog_ok) == 0);
  assert(catalog_ok);
  char *chosen_model =
      wd_text_copy(wd_get(wd_get(chosen, "session"), "model"));
  assert(chosen_model != NULL && strcmp(chosen_model, "local:qwen") == 0);
  free(chosen_model);
  wd_free(chosen);

  cbor_item_t *rejected = NULL;
  cbor_item_t *bad = wd_map(2);
  wd_put(bad, "id", wd_text(session));
  wd_put(bad, "model", wd_text("no:such"));
  catalog_ok = true;
  assert(request(&a, a.next_id++, "session.set_model", bad, &rejected,
                 &catalog_ok) == 0);
  assert(!catalog_ok);
  char *rejected_name = wd_text_copy(wd_get(rejected, "name"));
  assert(rejected_name != NULL && strcmp(rejected_name, "not_found") == 0);
  free(rejected_name);
  wd_free(rejected);

  cbor_item_t *bad_create = NULL;
  cbor_item_t *create_bad = wd_map(2);
  wd_put(create_bad, "cwd", wd_text(root));
  wd_put(create_bad, "model", wd_text("no:such"));
  catalog_ok = true;
  assert(request(&a, a.next_id++, "session.create", create_bad, &bad_create,
                 &catalog_ok) == 0);
  assert(!catalog_ok);
  wd_free(bad_create);

  cbor_item_t *renamed = NULL;
  cbor_item_t *rename_params = wd_map(2);
  wd_put(rename_params, "id", wd_text(session));
  wd_put(rename_params, "name", wd_text("gated session"));
  assert(request(&a, a.next_id++, "session.rename", rename_params, &renamed,
                 NULL) == 0);
  wd_free(renamed);
  cbor_item_t *named_get = NULL;
  cbor_item_t *named_params = wd_map(1);
  wd_put(named_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.get", named_params, &named_get,
                 NULL) == 0);
  char *named = wd_text_copy(wd_get(wd_get(named_get, "session"), "name"));
  assert(named != NULL && strcmp(named, "gated session") == 0);
  free(named);
  wd_free(named_get);

  cbor_item_t *found = NULL;
  cbor_item_t *search = wd_map(1);
  wd_put(search, "query", wd_text("gated"));
  assert(request(&a, a.next_id++, "session.search", search, &found, NULL) == 0);
  assert(cbor_array_size(wd_get(found, "sessions")) == 1);
  wd_free(found);

  cbor_item_t *pinned = NULL;
  cbor_item_t *pin = wd_map(2);
  wd_put(pin, "id", wd_text(session));
  wd_put(pin, "pinned", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "session.pin", pin, &pinned, NULL) == 0);
  bool is_pinned = false;
  assert(wd_bool_of(wd_get(wd_get(pinned, "session"), "pinned"), &is_pinned));
  assert(is_pinned);
  wd_free(pinned);

  cbor_item_t *archived = NULL;
  cbor_item_t *archive = wd_map(2);
  wd_put(archive, "id", wd_text(session));
  wd_put(archive, "archived", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "session.archive", archive, &archived, NULL) ==
         0);
  wd_free(archived);

  cbor_item_t *visible = NULL;
  assert(request(&a, a.next_id++, "session.list", wd_map(0), &visible, NULL) ==
         0);
  assert(cbor_array_size(wd_get(visible, "sessions")) == 0);
  wd_free(visible);

  cbor_item_t *visible_all = NULL;
  cbor_item_t *list_params = wd_map(1);
  wd_put(list_params, "include_archived", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "session.list", list_params, &visible_all,
                 NULL) == 0);
  assert(cbor_array_size(wd_get(visible_all, "sessions")) == 1);
  wd_free(visible_all);

  cbor_item_t *resumed = NULL;
  cbor_item_t *resume = wd_map(1);
  wd_put(resume, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.resume", resume, &resumed, NULL) ==
         0);
  wd_free(resumed);

  cbor_item_t *plain = NULL;
  cbor_item_t *plain_prompt = wd_map(2);
  wd_put(plain_prompt, "id", wd_text(session));
  wd_put(plain_prompt, "text", wd_text("hello over the async provider"));
  assert(request(&a, a.next_id++, "session.prompt", plain_prompt, &plain,
                 NULL) == 0);
  wd_free(plain);
  char rule_text[2048];
  turn_text(&a, last_turn, rule_text, sizeof(rule_text));
  assert(strstr(rule_text, "[rules]") != NULL);



  cbor_item_t *answered = NULL;
  cbor_item_t *prompt = wd_map(2);
  wd_put(prompt, "id", wd_text(session));
  wd_put(prompt, "text", wd_text("/write out.txt hello there"));
  assert(request(&a, a.next_id++, "session.prompt", prompt, &answered, NULL) ==
         0);
  cbor_item_t *approval = wd_get(answered, "approval");
  assert(approval != NULL);
  uint64_t call = 0;
  uint64_t epoch = 0;
  assert(wd_uint_of(wd_get(approval, "call"), &call));
  assert(wd_uint_of(wd_get(approval, "epoch"), &epoch));
  char *reason = wd_text_copy(wd_get(approval, "reason"));
  assert(reason != NULL && reason[0] != '\0');
  free(reason);
  wd_free(answered);

  client b;
  assert(connect_client(&b, sock, 100) == 0);
  cbor_item_t *late = NULL;
  cbor_item_t *replay = wd_map(2);
  wd_put(replay, "id", wd_text(session));
  wd_put(replay, "from", wd_uint(0));
  assert(request(&b, b.next_id++, "session.resume", replay, &late, NULL) == 0);
  wd_free(late);
  uint64_t seen_seq = 0;
  cbor_item_t *seen = wait_event(&b, "approval.requested", &seen_seq);
  assert(seen != NULL);
  uint64_t late_call = 0;
  assert(wd_uint_of(wd_get(seen, "call"), &late_call));
  assert(late_call == call);
  wd_free(seen);

  cbor_item_t *resolved = NULL;
  cbor_item_t *respond = wd_map(4);
  wd_put(respond, "session", wd_text(session));
  wd_put(respond, "call", wd_uint(call));
  wd_put(respond, "epoch", wd_uint(epoch));
  wd_put(respond, "allow", cbor_build_bool(true));
  bool ok = false;
  assert(request(&a, a.next_id++, "approval.respond", respond, &resolved,
                 &ok) == 0);
  assert(ok);
  wd_free(resolved);

  cbor_item_t *event = wait_event(&a, "approval.resolved", NULL);
  assert(event != NULL);
  wd_free(event);
  event = wait_event(&a, "text.delta", NULL);
  assert(event != NULL);
  char *text = wd_text_copy(wd_get(event, "text"));
  assert(text != NULL && strstr(text, "wrote") != NULL);
  free(text);
  wd_free(event);
  event = wait_turn_end(&a, last_turn);
  assert(event != NULL);
  wd_free(event);

  cbor_item_t *stats_one = NULL;
  cbor_item_t *stats_params = wd_map(1);
  wd_put(stats_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "stats.session", stats_params, &stats_one,
                 NULL) == 0);
  uint64_t turns = 0;
  uint64_t text_bytes = 0;
  uint64_t events = 0;
  assert(wd_uint_of(wd_get(stats_one, "turns"), &turns));
  assert(wd_uint_of(wd_get(stats_one, "text_bytes"), &text_bytes));
  assert(wd_uint_of(wd_get(stats_one, "events"), &events));
  uint64_t prompt_bytes = 0;
  assert(wd_uint_of(wd_get(stats_one, "prompt_bytes"), &prompt_bytes));
  assert(turns >= 1 && text_bytes > 0 && events >= 4 && prompt_bytes > 0);
  bool estimated = false;
  assert(wd_bool_of(wd_get(stats_one, "tokens_estimated"), &estimated));
  assert(estimated);
  cbor_item_t *total = wd_get(stats_one, "cost_total");
  assert(total != NULL && cbor_isa_float_ctrl(total));
  assert(cbor_float_get_float(total) > 0);
  wd_free(stats_one);

  cbor_item_t *stats_all = NULL;
  assert(request(&a, a.next_id++, "stats.global", wd_map(0), &stats_all, NULL) ==
         0);
  uint64_t sessions_total = 0;
  assert(wd_uint_of(wd_get(stats_all, "sessions"), &sessions_total));
  assert(sessions_total >= 1);
  wd_free(stats_all);

  char path[512];
  snprintf(path, sizeof(path), "%s/out.txt", root);
  FILE *file = fopen(path, "r");
  assert(file != NULL);
  char content[64] = {0};
  assert(fgets(content, sizeof(content), file) != NULL);
  fclose(file);
  assert(strcmp(content, "hello there") == 0);

  cbor_item_t *compacted = NULL;
  cbor_item_t *compact_params = wd_map(1);
  wd_put(compact_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.compact", compact_params, &compacted,
                 NULL) == 0);
  char *summary = wd_text_copy(wd_get(compacted, "summary"));
  assert(summary != NULL && summary[0] != '\0');
  uint64_t compact_through = 0;
  assert(wd_uint_of(wd_get(compacted, "through"), &compact_through));
  assert(compact_through > 0);
  free(summary);
  wd_free(compacted);

  cbor_item_t *checked = NULL;
  cbor_item_t *check_params = wd_map(2);
  wd_put(check_params, "id", wd_text(session));
  wd_put(check_params, "label", wd_text("before the undo"));
  assert(request(&a, a.next_id++, "session.checkpoint", check_params, &checked,
                 NULL) == 0);
  uint64_t checkpoint_seq = 0;
  assert(wd_uint_of(wd_get(checked, "seq"), &checkpoint_seq));
  assert(checkpoint_seq > 0);
  wd_free(checked);

  cbor_item_t *branched = NULL;
  cbor_item_t *undo_params = wd_map(1);
  wd_put(undo_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.undo", undo_params, &branched,
                 NULL) == 0);
  char *branch_id = wd_text_copy(wd_get(wd_get(branched, "session"), "id"));
  assert(branch_id != NULL && strcmp(branch_id, session) != 0);
  uint64_t branch_seq = 0;
  assert(wd_uint_of(wd_get(wd_get(branched, "session"), "last_seq"),
                    &branch_seq));
  assert(branch_seq == checkpoint_seq + 1);
  free(branch_id);
  wd_free(branched);

  cbor_item_t *forked = NULL;
  cbor_item_t *fork_params = wd_map(1);
  wd_put(fork_params, "id", wd_text(session));
  assert(request(&a, a.next_id++, "session.fork", fork_params, &forked, NULL) ==
         0);
  char *forked_id = wd_text_copy(wd_get(wd_get(forked, "session"), "id"));
  assert(forked_id != NULL && strcmp(forked_id, session) != 0);
  wd_free(forked);

  cbor_item_t *forked_get = NULL;
  cbor_item_t *get_fork = wd_map(1);
  wd_put(get_fork, "id", wd_text(forked_id));
  assert(request(&a, a.next_id++, "session.get", get_fork, &forked_get, NULL) ==
         0);
  uint64_t forked_seq = 0;
  assert(wd_uint_of(wd_get(wd_get(forked_get, "session"), "last_seq"),
                    &forked_seq));
  assert(forked_seq >= 4);
  wd_free(forked_get);

  cbor_item_t *forked_replay = NULL;
  cbor_item_t *replay_fork = wd_map(2);
  wd_put(replay_fork, "id", wd_text(forked_id));
  wd_put(replay_fork, "from", wd_uint(0));
  assert(request(&a, a.next_id++, "session.resume", replay_fork,
                 &forked_replay, NULL) == 0);
  wd_free(forked_replay);
  cbor_item_t *seen_turn = wait_event(&a, "turn.end", NULL);
  assert(seen_turn != NULL);
  wd_free(seen_turn);
  free(forked_id);

  cbor_item_t *exec_prompt = NULL;
  cbor_item_t *run = wd_map(2);
  wd_put(run, "id", wd_text(session));
  wd_put(run, "text", wd_text("/run sleep 1"));
  assert(request(&a, a.next_id++, "session.prompt", run, &exec_prompt, NULL) ==
         0);
  uint64_t exec_call = 0;
  uint64_t exec_epoch = 0;
  assert(wd_uint_of(wd_get(wd_get(exec_prompt, "approval"), "call"), &exec_call));
  assert(wd_uint_of(wd_get(wd_get(exec_prompt, "approval"), "epoch"),
                    &exec_epoch));
  wd_free(exec_prompt);

  cbor_item_t *exec_allowed = NULL;
  cbor_item_t *exec_respond = wd_map(4);
  wd_put(exec_respond, "session", wd_text(session));
  wd_put(exec_respond, "call", wd_uint(exec_call));
  wd_put(exec_respond, "epoch", wd_uint(exec_epoch));
  wd_put(exec_respond, "allow", cbor_build_bool(true));
  catalog_ok = false;
  assert(request(&a, a.next_id++, "approval.respond", exec_respond,
                 &exec_allowed, &catalog_ok) == 0);
  assert(catalog_ok);
  wd_free(exec_allowed);

  cbor_item_t *meanwhile = NULL;
  assert(request(&a, a.next_id++, "session.list", wd_map(1), &meanwhile,
                 NULL) == 0);
  wd_free(meanwhile);

  cbor_item_t *exec_end = wait_turn_end(&a, last_turn);
  assert(exec_end != NULL);
  wd_free(exec_end);

  cbor_item_t *slow = NULL;
  cbor_item_t *slow_prompt = wd_map(2);
  wd_put(slow_prompt, "id", wd_text(session));
  wd_put(slow_prompt, "text", wd_text("/run sleep 5"));
  assert(request(&a, a.next_id++, "session.prompt", slow_prompt, &slow, NULL) ==
         0);
  uint64_t slow_call = 0;
  uint64_t slow_epoch = 0;
  assert(wd_uint_of(wd_get(wd_get(slow, "approval"), "call"), &slow_call));
  assert(wd_uint_of(wd_get(wd_get(slow, "approval"), "epoch"), &slow_epoch));
  wd_free(slow);

  cbor_item_t *slow_ok = NULL;
  cbor_item_t *slow_respond = wd_map(4);
  wd_put(slow_respond, "session", wd_text(session));
  wd_put(slow_respond, "call", wd_uint(slow_call));
  wd_put(slow_respond, "epoch", wd_uint(slow_epoch));
  wd_put(slow_respond, "allow", cbor_build_bool(true));
  catalog_ok = false;
  assert(request(&a, a.next_id++, "approval.respond", slow_respond, &slow_ok,
                 &catalog_ok) == 0);
  assert(catalog_ok);
  wd_free(slow_ok);

  cbor_item_t *cancel = NULL;
  cbor_item_t *cancel_params = wd_map(1);
  wd_put(cancel_params, "id", wd_text(session));
  bool cancelled = false;
  assert(request(&a, a.next_id++, "session.cancel", cancel_params, &cancel,
                 &cancelled) == 0);
  assert(cancelled);
  wd_free(cancel);
  uint64_t stopped_at = wd_now_ms();
  cbor_item_t *after_cancel = wait_turn_end(&a, last_turn);
  assert(after_cancel != NULL);
  assert(wd_now_ms() - stopped_at < 3000);
  wd_free(after_cancel);

  cbor_item_t *second = NULL;
  cbor_item_t *prompt2 = wd_map(2);
  wd_put(prompt2, "id", wd_text(session));
  wd_put(prompt2, "text", wd_text("/write blocked.txt nope"));
  assert(request(&a, a.next_id++, "session.prompt", prompt2, &second, NULL) ==
         0);
  uint64_t call2 = 0;
  uint64_t epoch2 = 0;
  assert(wd_uint_of(wd_get(wd_get(second, "approval"), "call"), &call2));
  assert(wd_uint_of(wd_get(wd_get(second, "approval"), "epoch"), &epoch2));
  wd_free(second);

  cbor_item_t *denied = NULL;
  cbor_item_t *refuse = wd_map(4);
  wd_put(refuse, "session", wd_text(session));
  wd_put(refuse, "call", wd_uint(call2));
  wd_put(refuse, "epoch", wd_uint(epoch2));
  wd_put(refuse, "allow", cbor_build_bool(false));
  assert(request(&a, a.next_id++, "approval.respond", refuse, &denied, &ok) ==
         0);
  assert(ok);
  wd_free(denied);
  event = wait_event(&a, "approval.resolved", NULL);
  assert(event != NULL);
  wd_free(event);
  event = wait_event(&a, "text.delta", NULL);
  assert(event != NULL);
  text = wd_text_copy(wd_get(event, "text"));
  assert(text != NULL && strstr(text, "denied") != NULL);
  free(text);
  wd_free(event);
  event = wait_turn_end(&a, last_turn);
  assert(event != NULL);
  wd_free(event);
  snprintf(path, sizeof(path), "%s/blocked.txt", root);
  assert(access(path, F_OK) != 0);

  cbor_item_t *again = NULL;
  cbor_item_t *repeat = wd_map(4);
  wd_put(repeat, "session", wd_text(session));
  wd_put(repeat, "call", wd_uint(call2));
  wd_put(repeat, "epoch", wd_uint(epoch2));
  wd_put(repeat, "allow", cbor_build_bool(true));
  ok = false;
  assert(request(&a, a.next_id++, "approval.respond", repeat, &again, &ok) ==
         0);
  assert(!ok);
  char *name = wd_text_copy(wd_get(again, "name"));
  assert(name != NULL && strcmp(name, "already_resolved") == 0);
  free(name);
  wd_free(again);

  cbor_item_t *sub_a = NULL;
  cbor_item_t *sub_a_params = wd_map(2);
  wd_put(sub_a_params, "session", wd_text(session));
  wd_put(sub_a_params, "text", wd_text("subagent one"));
  uint64_t started = wd_now_ms();
  assert(request(&a, a.next_id++, "subagent.prompt", sub_a_params, &sub_a,
                 NULL) == 0);
  char *child_a = wd_text_copy(wd_get(sub_a, "session"));
  uint64_t turn_a = 0;
  assert(wd_uint_of(wd_get(sub_a, "turn"), &turn_a));
  wd_free(sub_a);

  cbor_item_t *sub_b = NULL;
  cbor_item_t *sub_b_params = wd_map(2);
  wd_put(sub_b_params, "session", wd_text(session));
  wd_put(sub_b_params, "text", wd_text("subagent two"));
  assert(request(&a, a.next_id++, "subagent.prompt", sub_b_params, &sub_b,
                 NULL) == 0);
  char *child_b = wd_text_copy(wd_get(sub_b, "session"));
  uint64_t turn_b = 0;
  assert(wd_uint_of(wd_get(sub_b, "turn"), &turn_b));
  wd_free(sub_b);
  assert(child_a != NULL && child_b != NULL);
  assert(strcmp(child_a, child_b) != 0);
  assert(strcmp(child_a, session) != 0);

  cbor_item_t *watch_a = NULL;
  cbor_item_t *watch_a_params = wd_map(2);
  wd_put(watch_a_params, "id", wd_text(child_a));
  wd_put(watch_a_params, "from", wd_uint(0));
  assert(request(&a, a.next_id++, "session.resume", watch_a_params, &watch_a,
                 NULL) == 0);
  wd_free(watch_a);
  cbor_item_t *watch_b = NULL;
  cbor_item_t *watch_b_params = wd_map(2);
  wd_put(watch_b_params, "id", wd_text(child_b));
  wd_put(watch_b_params, "from", wd_uint(0));
  assert(request(&a, a.next_id++, "session.resume", watch_b_params, &watch_b,
                 NULL) == 0);
  wd_free(watch_b);

  cbor_item_t *plan = NULL;
  cbor_item_t *plan_prompt = wd_map(2);
  wd_put(plan_prompt, "id", wd_text(session));
  wd_put(plan_prompt, "text", wd_text("/plan write the docs first"));
  assert(request(&a, a.next_id++, "session.prompt", plan_prompt, &plan, NULL) ==
         0);
  uint64_t plan_call = 0;
  uint64_t plan_epoch = 0;
  assert(wd_uint_of(wd_get(wd_get(plan, "approval"), "call"), &plan_call));
  assert(wd_uint_of(wd_get(wd_get(plan, "approval"), "epoch"), &plan_epoch));
  wd_free(plan);
  cbor_item_t *plan_ok = NULL;
  cbor_item_t *plan_respond = wd_map(4);
  wd_put(plan_respond, "session", wd_text(session));
  wd_put(plan_respond, "call", wd_uint(plan_call));
  wd_put(plan_respond, "epoch", wd_uint(plan_epoch));
  wd_put(plan_respond, "allow", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "approval.respond", plan_respond, &plan_ok,
                 NULL) == 0);
  wd_free(plan_ok);
  cbor_item_t *plan_end = wait_turn_end(&a, last_turn);
  assert(plan_end != NULL);
  wd_free(plan_end);

  cbor_item_t *slash = NULL;
  cbor_item_t *slash_prompt = wd_map(2);
  wd_put(slash_prompt, "id", wd_text(session));
  wd_put(slash_prompt, "text", wd_text("/help"));
  assert(request(&a, a.next_id++, "session.prompt", slash_prompt, &slash,
                 NULL) == 0);
  wd_free(slash);
  cbor_item_t *slash_delta = wait_event(&a, "text.delta", NULL);
  assert(slash_delta != NULL);
  char *slash_text = wd_text_copy(wd_get(slash_delta, "text"));
  assert(slash_text != NULL && strstr(slash_text, "skills:") != NULL);
  free(slash_text);
  wd_free(slash_delta);
  cbor_item_t *slash_end = wait_turn_end(&a, last_turn);
  assert(slash_end != NULL);
  wd_free(slash_end);

  cbor_item_t *search_resp = NULL;
  cbor_item_t *search_prompt = wd_map(2);
  wd_put(search_prompt, "id", wd_text(session));
  wd_put(search_prompt, "text", wd_text("/search ward agent"));
  assert(request(&a, a.next_id++, "session.prompt", search_prompt,
                 &search_resp, NULL) == 0);
  wd_free(search_resp);
  cbor_item_t *search_delta = wait_event(&a, "text.delta", NULL);
  assert(search_delta != NULL);
  char *search_text = wd_text_copy(wd_get(search_delta, "text"));
  assert(search_text != NULL && strstr(search_text, "stub result one") != NULL);
  free(search_text);
  wd_free(search_delta);
  cbor_item_t *search_end = wait_turn_end(&a, last_turn);
  assert(search_end != NULL);
  wd_free(search_end);

  cbor_item_t *mcp_resp = NULL;
  cbor_item_t *mcp_prompt = wd_map(2);
  wd_put(mcp_prompt, "id", wd_text(session));
  wd_put(mcp_prompt, "text",
         wd_text("/mcp {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}"));
  assert(request(&a, a.next_id++, "session.prompt", mcp_prompt, &mcp_resp,
                 NULL) == 0);
  wd_free(mcp_resp);
  cbor_item_t *mcp_delta = wait_event(&a, "text.delta", NULL);
  assert(mcp_delta != NULL);
  char *mcp_text = wd_text_copy(wd_get(mcp_delta, "text"));
  assert(mcp_text != NULL && strstr(mcp_text, "mcp ok") != NULL);
  free(mcp_text);
  wd_free(mcp_delta);
  cbor_item_t *mcp_end = wait_turn_end(&a, last_turn);
  assert(mcp_end != NULL);
  wd_free(mcp_end);

  bool term_written = false;
  bool term_resized = false;
  bool term_closed = false;
  cbor_item_t *term_resp = NULL;
  cbor_item_t *term_params = wd_map(3);
  wd_put(term_params, "id", wd_text(session));
  wd_put(term_params, "command", wd_text("cat"));
  wd_put(term_params, "rows", wd_uint(30));
  assert(request(&a, a.next_id++, "terminal.open", term_params, &term_resp,
                 NULL) == 0);
  char *term_id = wd_text_copy(wd_get(term_resp, "terminal"));
  assert(term_id != NULL);
  wd_free(term_resp);
  cbor_item_t *term_write = wd_map(2);
  wd_put(term_write, "terminal", wd_text(term_id));
  wd_put(term_write, "data", wd_text("hello terminal\n"));
  cbor_item_t *term_write_resp = NULL;
  assert(request(&a, a.next_id++, "terminal.write", term_write,
                 &term_write_resp, NULL) == 0);
  assert(wd_bool_of(wd_get(term_write_resp, "written"), &term_written));
  wd_free(term_write_resp);
  cbor_item_t *term_out = wait_event(&a, "terminal.output", NULL);
  assert(term_out != NULL);
  char *term_data = wd_text_copy(wd_get(term_out, "data"));
  assert(term_data != NULL && strstr(term_data, "hello terminal") != NULL);
  free(term_data);
  wd_free(term_out);
  cbor_item_t *term_resize = wd_map(3);
  wd_put(term_resize, "terminal", wd_text(term_id));
  wd_put(term_resize, "rows", wd_uint(40));
  wd_put(term_resize, "cols", wd_uint(100));
  cbor_item_t *term_resize_resp = NULL;
  assert(request(&a, a.next_id++, "terminal.resize", term_resize,
                 &term_resize_resp, NULL) == 0);
  assert(wd_bool_of(wd_get(term_resize_resp, "resized"), &term_resized));
  wd_free(term_resize_resp);
  cbor_item_t *term_close = wd_map(1);
  wd_put(term_close, "terminal", wd_text(term_id));
  cbor_item_t *term_close_resp = NULL;
  assert(request(&a, a.next_id++, "terminal.close", term_close,
                 &term_close_resp, NULL) == 0);
  assert(wd_bool_of(wd_get(term_close_resp, "closed"), &term_closed));
  wd_free(term_close_resp);
  free(term_id);

  cbor_item_t *recall_resp = NULL;
  cbor_item_t *recall_prompt = wd_map(2);
  wd_put(recall_prompt, "id", wd_text(session));
  wd_put(recall_prompt, "text", wd_text("/recall ward terminal"));
  assert(request(&a, a.next_id++, "session.prompt", recall_prompt,
                 &recall_resp, NULL) == 0);
  wd_free(recall_resp);
  cbor_item_t *recall_delta = wait_event(&a, "text.delta", NULL);
  assert(recall_delta != NULL);
  char *recall_text = wd_text_copy(wd_get(recall_delta, "text"));
  assert(recall_text != NULL && strstr(recall_text, "remember this") != NULL);
  free(recall_text);
  wd_free(recall_delta);
  cbor_item_t *recall_end = wait_turn_end(&a, last_turn);
  assert(recall_end != NULL);
  wd_free(recall_end);

  cbor_item_t *att_add = NULL;
  cbor_item_t *att_params = wd_map(3);
  wd_put(att_params, "id", wd_text(session));
  wd_put(att_params, "name", wd_text("note.txt"));
  wd_put(att_params, "data", wd_text("attached body"));
  assert(request(&a, a.next_id++, "attachment.add", att_params, &att_add,
                 NULL) == 0);
  char *att_path = wd_text_copy(wd_get(att_add, "path"));
  assert(att_path != NULL && strstr(att_path, "note.txt") != NULL);
  free(att_path);
  wd_free(att_add);
  cbor_item_t *att_list = NULL;
  cbor_item_t *att_query = wd_map(1);
  wd_put(att_query, "id", wd_text(session));
  assert(request(&a, a.next_id++, "attachment.list", att_query, &att_list,
                 NULL) == 0);
  cbor_item_t *att_items = wd_get(att_list, "attachments");
  assert(att_items != NULL && cbor_array_size(att_items) == 1);
  wd_free(att_list);

  cbor_item_t *sched = NULL;
  cbor_item_t *sched_params = wd_map(3);
  wd_put(sched_params, "session", wd_text(session));
#ifdef __SANITIZE_ADDRESS__
  wd_put(sched_params, "cron", wd_text("@every 60000"));
#else
  wd_put(sched_params, "cron", wd_text("@every 150"));
#endif
  wd_put(sched_params, "text", wd_text("/read note.txt"));
  uint64_t set_at = wd_now_ms();
  assert(request(&a, a.next_id++, "schedule.set", sched_params, &sched, NULL) ==
         0);
  char *sched_id = wd_text_copy(wd_get(sched, "schedule"));
  uint64_t sched_next = 0;
  assert(wd_uint_of(wd_get(sched, "next_ms"), &sched_next));
#ifdef __SANITIZE_ADDRESS__
  assert(sched_next > set_at && sched_next - set_at <= 60100);
#else
  assert(sched_next > set_at && sched_next - set_at <= 200);
#endif
  wd_free(sched);

  cbor_item_t *sched_list = NULL;
  assert(request(&a, a.next_id++, "schedule.list", wd_map(0), &sched_list,
                 NULL) == 0);
  assert(cbor_array_size(wd_get(sched_list, "schedules")) == 1);
  wd_free(sched_list);

#ifndef __SANITIZE_ADDRESS__
  cbor_item_t *fired = wait_event(&a, "schedule.fired", NULL);
  assert(fired != NULL);
  wd_free(fired);
#endif

  cbor_item_t *deleted = NULL;
  cbor_item_t *delete_params = wd_map(1);
  wd_put(delete_params, "schedule", wd_text(sched_id));
  assert(request(&a, a.next_id++, "schedule.delete", delete_params, &deleted,
                 NULL) == 0);
  bool was_deleted = false;
  assert(wd_bool_of(wd_get(deleted, "deleted"), &was_deleted));
  assert(was_deleted);
  wd_free(deleted);
  free(sched_id);

  cbor_item_t *goal = NULL;
  cbor_item_t *goal_params = wd_map(2);
  wd_put(goal_params, "session", wd_text(session));
  wd_put(goal_params, "text", wd_text("ship M3"));
  assert(request(&a, a.next_id++, "goal.set", goal_params, &goal, NULL) == 0);
  char *goal_text = wd_text_copy(wd_get(goal, "goal"));
  assert(goal_text != NULL && strcmp(goal_text, "ship M3") == 0);
  free(goal_text);
  wd_free(goal);

  cbor_item_t *cleared = NULL;
  cbor_item_t *clear_params = wd_map(1);
  wd_put(clear_params, "session", wd_text(session));
  assert(request(&a, a.next_id++, "goal.clear", clear_params, &cleared, NULL) ==
         0);
  bool was_cleared = false;
  assert(wd_bool_of(wd_get(cleared, "cleared"), &was_cleared));
  assert(was_cleared);
  wd_free(cleared);

  cbor_item_t *third = NULL;
  cbor_item_t *third_params = wd_map(2);
  wd_put(third_params, "session", wd_text(session));
  wd_put(third_params, "text", wd_text("subagent three"));
  assert(request(&a, a.next_id++, "subagent.prompt", third_params, &third,
                 NULL) == 0);
  char *child_c = wd_text_copy(wd_get(third, "session"));
  bool third_queued = false;
  assert(wd_bool_of(wd_get(third, "queued"), &third_queued));
  assert(third_queued);
  wd_free(third);

  cbor_item_t *jobs = NULL;
  assert(request(&a, a.next_id++, "job.list", wd_map(0), &jobs, NULL) == 0);
  size_t job_count = cbor_array_size(wd_get(jobs, "jobs"));
  assert(job_count >= 3);
  size_t queued_count = 0;
  cbor_item_t **job_items = cbor_array_handle(wd_get(jobs, "jobs"));
  for (size_t i = 0; i < job_count; i++) {
    char *state = wd_text_copy(wd_get(job_items[i], "state"));
    if (state != NULL && strcmp(state, "queued") == 0) queued_count++;
    free(state);
  }
  assert(queued_count == 1);
  wd_free(jobs);

  cbor_item_t *cancel_sub = NULL;
  cbor_item_t *cancel_child = wd_map(1);
  wd_put(cancel_child, "id", wd_text(child_b));
  bool cancel_ok = false;
  assert(request(&a, a.next_id++, "session.cancel", cancel_child, &cancel_sub,
                 &cancel_ok) == 0);
  assert(cancel_ok);
  wd_free(cancel_sub);
  cbor_item_t *end_b = wait_turn_end(&a, turn_b);
  assert(end_b != NULL);
  bool aborted = false;
  assert(wd_bool_of(wd_get(end_b, "aborted"), &aborted));
  assert(aborted);
  wd_free(end_b);
  cbor_item_t *end_a = wait_turn_end(&a, turn_a);
  assert(end_a != NULL);
  wd_free(end_a);
  uint64_t elapsed = wd_now_ms() - started;
#ifdef __SANITIZE_ADDRESS__
  assert(elapsed < 9000);
#else
  assert(elapsed < 7000);
#endif

  cbor_item_t *idle = NULL;
  cbor_item_t *idle_params = wd_map(1);
  wd_put(idle_params, "session", wd_text(child_a));
  bool idle_ok = false;
  assert(request(&a, a.next_id++, "subagent.interrupt", idle_params, &idle,
                 &idle_ok) == 0);
  assert(idle_ok);
  bool was_interrupted = true;
  assert(wd_bool_of(wd_get(idle, "interrupted"), &was_interrupted));
  assert(!was_interrupted);
  wd_free(idle);
  printf("subagents ran concurrently in %llu ms\n", (unsigned long long)elapsed);
  cbor_item_t *watch_c = NULL;
  cbor_item_t *watch_c_params = wd_map(2);
  wd_put(watch_c_params, "id", wd_text(child_c));
  wd_put(watch_c_params, "from", wd_uint(0));
  assert(request(&a, a.next_id++, "session.resume", watch_c_params, &watch_c,
                 NULL) == 0);
  wd_free(watch_c);
  cbor_item_t *end_c = wait_event(&a, "turn.end", NULL);
  assert(end_c != NULL);
  wd_free(end_c);
  free(child_a);
  free(child_b);
  free(child_c);

  cbor_item_t *att_prompt = NULL;
  cbor_item_t *att_turn = wd_map(2);
  wd_put(att_turn, "id", wd_text(session));
  wd_put(att_turn, "text", wd_text("what is attached"));
  assert(request(&a, a.next_id++, "session.prompt", att_turn, &att_prompt,
                 NULL) == 0);
  wd_free(att_prompt);
  char att_echo[2048];
  turn_text(&a, last_turn, att_echo, sizeof(att_echo));
  assert(strstr(att_echo, "[attach]") != NULL);

  cbor_item_t *judged = NULL;
  cbor_item_t *judge_prompt = wd_map(2);
  wd_put(judge_prompt, "id", wd_text(session));
  wd_put(judge_prompt, "text", wd_text("/read rm -rf everything"));
  assert(request(&a, a.next_id++, "session.prompt", judge_prompt, &judged,
                 NULL) == 0);
  cbor_item_t *judge_approval = wd_get(judged, "approval");
  assert(judge_approval != NULL);
  uint64_t judge_call = 0;
  uint64_t judge_epoch = 0;
  assert(wd_uint_of(wd_get(judge_approval, "call"), &judge_call));
  assert(wd_uint_of(wd_get(judge_approval, "epoch"), &judge_epoch));
  wd_free(judged);
  cbor_item_t *judge_answer = NULL;
  cbor_item_t *judge_respond = wd_map(4);
  wd_put(judge_respond, "session", wd_text(session));
  wd_put(judge_respond, "call", wd_uint(judge_call));
  wd_put(judge_respond, "epoch", wd_uint(judge_epoch));
  wd_put(judge_respond, "allow", cbor_build_bool(false));
  assert(request(&a, a.next_id++, "approval.respond", judge_respond,
                 &judge_answer, NULL) == 0);
  wd_free(judge_answer);
  cbor_item_t *judge_event = wait_event(&a, "judge.decision", NULL);
  assert(judge_event != NULL);
  char *judged_tool = wd_text_copy(wd_get(judge_event, "tool"));
  bool judged_escalated = false;
  assert(judged_tool != NULL && strcmp(judged_tool, "read") == 0);
  assert(wd_bool_of(wd_get(judge_event, "escalated"), &judged_escalated));
  assert(judged_escalated);
  free(judged_tool);
  wd_free(judge_event);
  char judged_text[2048];
  turn_text(&a, last_turn, judged_text, sizeof(judged_text));
  assert(judged_text[0] != '\0');

  cbor_item_t *py = NULL;
  cbor_item_t *py_prompt = wd_map(2);
  wd_put(py_prompt, "id", wd_text(session));
  wd_put(py_prompt, "text", wd_text("/py print(6*7)"));
  assert(request(&a, a.next_id++, "session.prompt", py_prompt, &py, NULL) == 0);
  cbor_item_t *py_approval = wd_get(py, "approval");
  assert(py_approval != NULL);
  uint64_t py_call = 0;
  uint64_t py_epoch = 0;
  assert(wd_uint_of(wd_get(py_approval, "call"), &py_call));
  assert(wd_uint_of(wd_get(py_approval, "epoch"), &py_epoch));
  wd_free(py);
  cbor_item_t *py_answer = NULL;
  cbor_item_t *py_respond = wd_map(4);
  wd_put(py_respond, "session", wd_text(session));
  wd_put(py_respond, "call", wd_uint(py_call));
  wd_put(py_respond, "epoch", wd_uint(py_epoch));
  wd_put(py_respond, "allow", cbor_build_bool(true));
  assert(request(&a, a.next_id++, "approval.respond", py_respond, &py_answer,
                 NULL) == 0);
  wd_free(py_answer);
  char py_text[4096];
  turn_text(&a, last_turn, py_text, sizeof(py_text));
  assert(strstr(py_text, "42") != NULL);

  cbor_item_t *mods = NULL;
  assert(request(&a, a.next_id++, "module.list", wd_map(0), &mods, NULL) == 0);
  cbor_item_t *mod_items = wd_get(mods, "modules");
  assert(mod_items != NULL && cbor_array_size(mod_items) >= 8);
  cbor_item_t *first_mod = cbor_array_get(mod_items, 0);
  assert(wd_get(first_mod, "name") != NULL);
  char *first_version = wd_text_copy(wd_get(first_mod, "version"));
  assert(first_version != NULL && first_version[0] != '\0');
  free(first_version);
  bool first_valid = false;
  assert(wd_bool_of(wd_get(first_mod, "valid"), &first_valid));
  assert(first_valid);
  cbor_decref(&first_mod);
  wd_free(mods);

  cbor_item_t *upgraded = NULL;
  cbor_item_t *upgrade_params = wd_map(3);
  wd_put(upgrade_params, "id", wd_text(session));
  wd_put(upgrade_params, "module", wd_text("judge"));
  wd_put(upgrade_params, "version", wd_text("1"));
  assert(request(&a, a.next_id++, "session.upgrade", upgrade_params, &upgraded,
                 NULL) == 0);
  bool was_upgraded = false;
  assert(wd_bool_of(wd_get(upgraded, "upgraded"), &was_upgraded));
  assert(was_upgraded);
  wd_free(upgraded);
  cbor_item_t *again_reply = NULL;
  cbor_item_t *again_params = wd_map(3);
  wd_put(again_params, "id", wd_text(session));
  wd_put(again_params, "module", wd_text("judge"));
  wd_put(again_params, "version", wd_text("1"));
  assert(request(&a, a.next_id++, "session.upgrade", again_params,
                 &again_reply, NULL) == 0);
  bool again_upgraded = false;
  assert(wd_bool_of(wd_get(again_reply, "upgraded"), &again_upgraded));
  assert(!again_upgraded);
  wd_free(again_reply);

  cbor_item_t *reload = NULL;
  assert(request(&a, a.next_id++, "module.reload", wd_map(0), &reload, NULL) ==
         0);
  cbor_item_t *reload_updated = wd_get(reload, "updated");
  cbor_item_t *reload_refused = wd_get(reload, "refused");
  assert(reload_updated != NULL && cbor_array_size(reload_updated) == 0);
  assert(reload_refused != NULL && cbor_array_size(reload_refused) == 0);
  wd_free(reload);

  free(session);
  close(b.fd);
  close(a.fd);
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);

  stop_server(endpoint);
  printf("tool gate passed: a write waited for an approval and was answered\n");
  return 0;
}
