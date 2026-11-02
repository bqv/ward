/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "client/client.h"
#include "proto/codec.h"
#include "fake_endpoint.h"

static int run_capture(const char *socket, const char *bin, char *const argv[],
                       char *out, size_t cap) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    alarm(45);
    setenv("WARD_SOCKET", socket, 1);
    dup2(fds[1], 1);
    dup2(fds[1], 2);
    close(fds[0]);
    close(fds[1]);
    execv(bin, argv);
    _exit(127);
  }
  close(fds[1]);
  size_t used = 0;
  while (used + 1 < cap) {
    ssize_t got = read(fds[0], out + used, cap - 1 - used);
    if (got <= 0) break;
    used += got;
  }
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int count_prompted(wd_client *client, const char *text, int want) {
  int seen = 0;
  while (seen < want) {
    wd_frame frame;
    uint8_t *body = wd_client_read_frame(client->fd, &frame);
    if (body == NULL) return seen;
    if (frame.type == WD_FRAME_EVENT) {
      cbor_item_t *root = wd_decode(body, frame.len);
      if (root != NULL) {
        char *kind = NULL;
        cbor_item_t *payload = NULL;
        bool read = wd_read_event(root, &kind, &payload);
        if (read && kind != NULL &&
            strcmp(kind, "session.prompted") == 0) {
          char *said = wd_text_copy(wd_get(payload, "text"));
          if (said != NULL && strcmp(said, text) == 0) seen++;
          free(said);
        }
        free(kind);
        wd_free(root);
      }
    }
    free(body);
  }
  return seen;
}

int main(void) {
  const char *modules = getenv("WARD_MODULE_DIR");
  const char *bin = getenv("WARD_NATIVE");
  const char *daemon = getenv("WARD_BIN");
  assert(modules != NULL && bin != NULL && daemon != NULL);

  char root[] = "/tmp/ward-native-gate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char state[256];
  char sock[128];
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(sock, sizeof(sock), "%s/sock", root);
  assert(mkdir(state, 0700) == 0);

  sweep_test_cgroups();
  int endpoint_port = 0;
  pid_t endpoint = 0;
  char *endpoint_url = fake_endpoint(&endpoint_port, &endpoint);

  char models_path[512];
  snprintf(models_path, sizeof(models_path), "%s/models.toml", state);
  FILE *models = fopen(models_path, "w");
  assert(models != NULL);
  assert(fprintf(models,
                 "[[model]]\nid = \"local:qwen\"\nprovider = \"stadi\"\n"
                 "model = \"qwen3\"\ncontext = \"32768\"\n"
                 "endpoint = \"%s\"\n",
                 endpoint_url) > 0);
  fclose(models);

  int ready[2];
  assert(pipe(ready) == 0);
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    dup2(ready[1], 1);
    close(ready[1]);
    char cgroup_base[128];
    snprintf(cgroup_base, sizeof(cgroup_base),
             "/sys/fs/cgroup/ward/test/gate-%d", (int)getpid());
    setenv("WARD_CGROUP", cgroup_base, 1);
    setenv("WARD_MODULE_DIR", modules, 1);
    setenv("WARD_TOOL_ROOT", root, 1);
    setenv("WARD_QUIC_BIND", "off", 1);
    setenv("WARD_FAKE_DELAY_MS", "4000", 1);
    setenv("WARD_TCP_BIND", "127.0.0.1:0", 1);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    execl(daemon, "ward", "up", "--socket", sock, "--state", state,
          (char *)NULL);
    _exit(127);
  }
  close(ready[1]);
  char line[256];
  char tcp_url[64];
  tcp_url[0] = '\0';
  bool ready_seen = false;
  size_t used = 0;
  while (!ready_seen) {
    size_t got = read(ready[0], line + used, 1);
    if (got <= 0) break;
    if (line[used++] != '\n') continue;
    line[used] = '\0';
    if (strncmp(line, "tcp ", 4) == 0) {
      snprintf(tcp_url, sizeof(tcp_url), "tcp://%s", line + 4);
      size_t len = strlen(tcp_url);
      while (len > 0 && (tcp_url[len - 1] == '\n')) tcp_url[--len] = '\0';
    }
    if (strncmp(line, "ready ", 6) == 0) ready_seen = true;
    used = 0;
  }
  close(ready[0]);
  assert(ready_seen);
  assert(tcp_url[0] != '\0');

  char out[8192];
  char *create_argv[] = {(char *)bin, "create", root, NULL};
  assert(run_capture(sock, bin, create_argv, out, sizeof(out)) == 0);
  char session[64];
  size_t at = 0;
  while (out[at] != '\0' && out[at] != '\n' && at + 1 < sizeof(session)) {
    session[at] = out[at];
    at++;
  }
  session[at] = '\0';
  assert(at == 16);

  wd_client queuer;
  assert(wd_client_connect(&queuer, sock) == 0);
  assert(wd_client_handshake(&queuer) == 0);
  cbor_item_t *created = wd_map(1);
  wd_put(created, "cwd", wd_text(root));
  cbor_item_t *created_result = NULL;
  assert(wd_client_call(&queuer, "session.create", created, &created_result) == 0);
  cbor_item_t *created_session = wd_get(created_result, "session");
  char *queued_session = wd_text_copy(wd_get(created_session, "id"));
  assert(queued_session != NULL);
  wd_free(created_result);
  cbor_item_t *first = wd_map(2);
  wd_put(first, "id", wd_text(queued_session));
  wd_put(first, "text", wd_text("first prompt"));
  cbor_item_t *first_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", first, &first_result) == 0);
  wd_free(first_result);
  cbor_item_t *second = wd_map(2);
  wd_put(second, "id", wd_text(queued_session));
  wd_put(second, "text", wd_text("second prompt"));
  cbor_item_t *second_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", second, &second_result) == 0);
  bool queued = false;
  uint64_t ahead = 99;
  assert(wd_bool_of(wd_get(second_result, "queued"), &queued));
  assert(queued);
  assert(wd_uint_of(wd_get(second_result, "ahead"), &ahead));
  assert(ahead == 0);
  wd_free(second_result);
  cbor_item_t *queue_params = wd_map(1);
  wd_put(queue_params, "id", wd_text(queued_session));
  cbor_item_t *queue_result = NULL;
  assert(wd_client_call(&queuer, "session.queue", queue_params,
                        &queue_result) == 0);
  cbor_item_t *queued_items = wd_get(queue_result, "items");
  assert(cbor_array_size(queued_items) == 1);
  cbor_item_t *queued_item = cbor_array_get(queued_items, 0);
  char *queued_text = wd_text_copy(wd_get(queued_item, "text"));
  assert(queued_text != NULL && strstr(queued_text, "second prompt") != NULL);
  free(queued_text);
  cbor_decref(&queued_item);
  wd_free(queue_result);
  cbor_item_t *steer_create = wd_map(1);
  wd_put(steer_create, "cwd", wd_text(root));
  cbor_item_t *steer_created = NULL;
  assert(wd_client_call(&queuer, "session.create", steer_create,
                        &steer_created) == 0);
  char *steer_session =
      wd_text_copy(wd_get(wd_get(steer_created, "session"), "id"));
  assert(steer_session != NULL);
  wd_free(steer_created);
  cbor_item_t *third = wd_map(2);
  wd_put(third, "id", wd_text(steer_session));
  wd_put(third, "text", wd_text("/write steer.txt go"));
  cbor_item_t *third_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", third, &third_result) == 0);
  cbor_item_t *third_approval = wd_get(third_result, "approval");
  assert(third_approval != NULL);
  uint64_t third_call = 0;
  uint64_t third_epoch = 0;
  assert(wd_uint_of(wd_get(third_approval, "call"), &third_call));
  assert(wd_uint_of(wd_get(third_approval, "epoch"), &third_epoch));
  wd_free(third_result);
  cbor_item_t *steer = wd_map(3);
  wd_put(steer, "id", wd_text(steer_session));
  wd_put(steer, "text", wd_text("steer this way"));
  wd_put(steer, "steer", cbor_build_bool(true));
  cbor_item_t *steer_result = NULL;
  assert(wd_client_call(&queuer, "session.prompt", steer, &steer_result) == 0);
  bool steered = false;
  assert(wd_bool_of(wd_get(steer_result, "steered"), &steered));
  assert(steered);
  wd_free(steer_result);
  free(steer_session);
  free(queued_session);
  close(queuer.fd);

  char *prompt_argv[] = {(char *)bin, "prompt", session, "native hello", NULL};
  int prompt_rc = run_capture(sock, bin, prompt_argv, out, sizeof(out));
  if (prompt_rc != 0)
    fprintf(stderr, "prompt rc=%d daemon_alive=%d\n", prompt_rc,
            kill(pid, 0) == 0 ? 1 : 0);
  assert(prompt_rc == 0);

  char note_path[256];
  snprintf(note_path, sizeof(note_path), "%s/note.txt", root);
  FILE *note = fopen(note_path, "w");
  assert(note != NULL);
  assert(fputs("desktop fixture\n", note) >= 0);
  fclose(note);
  wd_client reader;
  assert(wd_client_connect(&reader, sock) == 0);
  assert(wd_client_handshake(&reader) == 0);
  cbor_item_t *read_params = wd_map(2);
  wd_put(read_params, "id", wd_text(session));
  wd_put(read_params, "path", wd_text("note.txt"));
  cbor_item_t *read_result = NULL;
  assert(wd_client_call(&reader, "session.file_read", read_params,
                        &read_result) == 0);
  char *read_text = wd_text_copy(wd_get(read_result, "text"));
  assert(read_text != NULL && strstr(read_text, "desktop fixture") != NULL);
  free(read_text);
  wd_free(read_result);
  cbor_item_t *escape_params = wd_map(2);
  wd_put(escape_params, "id", wd_text(session));
  wd_put(escape_params, "path", wd_text("../outside.txt"));
  cbor_item_t *escape_result = NULL;
  assert(wd_client_call(&reader, "session.file_read", escape_params,
                        &escape_result) != 0);
  close(reader.fd);

  char *resume_argv[] = {(char *)bin, "resume", session, NULL};
  int resume_rc = run_capture(sock, bin, resume_argv, out, sizeof(out));
  if (resume_rc != 0) fprintf(stderr, "resume rc=%d out=%s\n", resume_rc, out);
  assert(resume_rc == 0);
  assert(strstr(out, "native hello") != NULL);

  char tcp_out[8192];
  char *tcp_create[] = {(char *)bin, "create", root, NULL};
  assert(run_capture(tcp_url, bin, tcp_create, tcp_out, sizeof(tcp_out)) == 0);
  assert(strlen(tcp_out) > 16);

  wd_client writer;
  assert(wd_client_connect(&writer, sock) == 0);
  assert(wd_client_handshake(&writer) == 0);
  cbor_item_t *pick = wd_map(2);
  wd_put(pick, "id", wd_text("full-access"));
  wd_put(pick, "confirm", cbor_build_bool(true));
  cbor_item_t *picked = NULL;
  assert(wd_client_call(&writer, "policy.select", pick, &picked) == 0);
  wd_free(picked);
  cbor_item_t *after = NULL;
  assert(wd_client_call(&writer, "policy.list", wd_map(0), &after) == 0);
  char *now = wd_text_copy(wd_get(after, "current"));
  assert(now != NULL && strcmp(now, "full-access") == 0);
  free(now);
  wd_free(after);
  cbor_item_t *free_created = wd_map(1);
  wd_put(free_created, "cwd", wd_text(root));
  cbor_item_t *free_session = NULL;
  assert(wd_client_call(&writer, "session.create", free_created,
                        &free_session) == 0);
  char *free_id = wd_text_copy(wd_get(wd_get(free_session, "session"), "id"));
  assert(free_id != NULL);
  wd_free(free_session);
  cbor_item_t *free_prompt = wd_map(2);
  wd_put(free_prompt, "id", wd_text(free_id));
  wd_put(free_prompt, "text", wd_text("/write free.txt unapproved"));
  cbor_item_t *free_answered = NULL;
  assert(wd_client_call(&writer, "session.prompt", free_prompt,
                        &free_answered) == 0);
  assert(wd_get(free_answered, "approval") == NULL);
  wd_free(free_answered);
  char free_path[512];
  snprintf(free_path, sizeof(free_path), "%s/free.txt", root);
  assert(access(free_path, R_OK) == 0);
  free(free_id);
  cbor_item_t *deny_pick = wd_map(1);
  wd_put(deny_pick, "id", wd_text("reads-only"));
  cbor_item_t *deny_picked = NULL;
  assert(wd_client_call(&writer, "policy.select", deny_pick, &deny_picked) == 0);
  wd_free(deny_picked);
  cbor_item_t *deny_created = wd_map(1);
  wd_put(deny_created, "cwd", wd_text(root));
  cbor_item_t *deny_session = NULL;
  assert(wd_client_call(&writer, "session.create", deny_created,
                        &deny_session) == 0);
  char *deny_id = wd_text_copy(wd_get(wd_get(deny_session, "session"), "id"));
  assert(deny_id != NULL);
  wd_free(deny_session);
  cbor_item_t *deny_prompt = wd_map(2);
  wd_put(deny_prompt, "id", wd_text(deny_id));
  wd_put(deny_prompt, "text", wd_text("/write blocked.txt denied"));
  cbor_item_t *deny_answered = NULL;
  assert(wd_client_call(&writer, "session.prompt", deny_prompt,
                        &deny_answered) == 0);
  assert(wd_get(deny_answered, "approval") == NULL);
  wd_free(deny_answered);
  char deny_path[512];
  snprintf(deny_path, sizeof(deny_path), "%s/blocked.txt", root);
  assert(access(deny_path, F_OK) != 0);
  free(deny_id);
  close(writer.fd);

  wd_client follower;
  assert(wd_client_connect(&follower, sock) == 0);
  assert(wd_client_handshake(&follower) == 0);
  cbor_item_t *reset_back = wd_map(1);
  wd_put(reset_back, "id", wd_text("default"));
  cbor_item_t *reset_done = NULL;
  assert(wd_client_call(&follower, "policy.select", reset_back, &reset_done) ==
         0);
  wd_free(reset_done);
  cbor_item_t *live_create = wd_map(1);
  wd_put(live_create, "cwd", wd_text(root));
  cbor_item_t *live_session = NULL;
  assert(wd_client_call(&follower, "session.create", live_create,
                        &live_session) == 0);
  char *live_id = wd_text_copy(wd_get(wd_get(live_session, "session"), "id"));
  assert(live_id != NULL);
  wd_free(live_session);
  cbor_item_t *live_prompt = wd_map(2);
  wd_put(live_prompt, "id", wd_text(live_id));
  wd_put(live_prompt, "text", wd_text("/write live.txt body"));
  cbor_item_t *live_answered = NULL;
  assert(wd_client_call(&follower, "session.prompt", live_prompt,
                        &live_answered) == 0);
  assert(wd_get(live_answered, "approval") != NULL);
  wd_free(live_answered);
  cbor_item_t *follow_params = wd_map(2);
  wd_put(follow_params, "id", wd_text(live_id));
  wd_put(follow_params, "from", wd_uint(0));
  cbor_item_t *follow_answer = NULL;
  assert(wd_client_call(&follower, "job.follow", follow_params,
                        &follow_answer) == 0);
  cbor_item_t *chunks = wd_get(follow_answer, "chunks");
  assert(chunks != NULL && cbor_array_size(chunks) == 1);
  uint64_t next_at = 0;
  assert(wd_uint_of(wd_get(follow_answer, "next"), &next_at));
  assert(next_at == 0);
  wd_free(follow_answer);
  cbor_item_t *cancel_params = wd_map(1);
  wd_put(cancel_params, "id", wd_text(live_id));
  cbor_item_t *cancel_result = NULL;
  assert(wd_client_call(&follower, "session.cancel", cancel_params,
                        &cancel_result) == 0);
  wd_free(cancel_result);
  cbor_item_t *gone_params = wd_map(1);
  wd_put(gone_params, "id", wd_text(live_id));
  cbor_item_t *gone = NULL;
  assert(wd_client_call(&follower, "job.follow", gone_params, &gone) != 0);
  free(live_id);
  close(follower.fd);

  wd_client setter;
  assert(wd_client_connect(&setter, sock) == 0);
  assert(wd_client_handshake(&setter) == 0);
  cbor_item_t *setting = wd_map(2);
  wd_put(setting, "key", wd_text("max_runs"));
  wd_put(setting, "value", wd_text("3"));
  cbor_item_t *setting_done = NULL;
  assert(wd_client_call(&setter, "settings.set", setting, &setting_done) == 0);
  wd_free(setting_done);
  cbor_item_t *read_back = NULL;
  assert(wd_client_call(&setter, "settings.get", wd_map(0), &read_back) == 0);
  uint64_t stored_runs = 0;
  assert(wd_uint_of(wd_get(read_back, "max_runs"), &stored_runs));
  assert(stored_runs == 3);
  wd_free(read_back);
  cbor_item_t *bad = wd_map(2);
  wd_put(bad, "key", wd_text("max_runs"));
  wd_put(bad, "value", wd_text("99"));
  cbor_item_t *bad_result = NULL;
  assert(wd_client_call(&setter, "settings.set", bad, &bad_result) != 0);
  cbor_item_t *bad_key = wd_map(2);
  wd_put(bad_key, "key", wd_text("nonsense"));
  wd_put(bad_key, "value", wd_text("1"));
  cbor_item_t *bad_key_result = NULL;
  assert(wd_client_call(&setter, "settings.set", bad_key, &bad_key_result) != 0);
  close(setter.fd);

  wd_client watcher;
  assert(wd_client_connect(&watcher, sock) == 0);
  assert(wd_client_handshake(&watcher) == 0);
  wd_client speaker;
  assert(wd_client_connect(&speaker, sock) == 0);
  assert(wd_client_handshake(&speaker) == 0);
  cbor_item_t *seed = wd_map(1);
  wd_put(seed, "cwd", wd_text(root));
  cbor_item_t *seeded = NULL;
  assert(wd_client_call(&speaker, "session.create", seed, &seeded) == 0);
  char *late_id = wd_text_copy(wd_get(wd_get(seeded, "session"), "id"));
  assert(late_id != NULL);
  wd_free(seeded);
  cbor_item_t *watch_join = wd_map(2);
  wd_put(watch_join, "id", wd_text(late_id));
  wd_put(watch_join, "from", wd_uint(0));
  cbor_item_t *watch_joined = NULL;
  assert(wd_client_call(&watcher, "session.resume", watch_join,
                        &watch_joined) == 0);
  wd_free(watch_joined);
  cbor_item_t *late_pick = wd_map(1);
  wd_put(late_pick, "id", wd_text("default"));
  cbor_item_t *late_picked = NULL;
  assert(wd_client_call(&speaker, "policy.select", late_pick, &late_picked) ==
         0);
  wd_free(late_picked);
  cbor_item_t *late_park = wd_map(2);
  wd_put(late_park, "id", wd_text(late_id));
  wd_put(late_park, "text", wd_text("/write late.txt go"));
  cbor_item_t *late_parked = NULL;
  assert(wd_client_call(&speaker, "session.prompt", late_park,
                        &late_parked) == 0);
  assert(wd_get(late_parked, "approval") != NULL);
  wd_free(late_parked);
  cbor_item_t *late_steer = wd_map(3);
  wd_put(late_steer, "id", wd_text(late_id));
  wd_put(late_steer, "text", wd_text("carried text"));
  wd_put(late_steer, "steer", cbor_build_bool(true));
  cbor_item_t *late_steered = NULL;
  assert(wd_client_call(&speaker, "session.prompt", late_steer,
                        &late_steered) == 0);
  bool late_ok = false;
  assert(wd_bool_of(wd_get(late_steered, "steered"), &late_ok));
  assert(late_ok);
  wd_free(late_steered);
  cbor_item_t *late_cancel = wd_map(1);
  wd_put(late_cancel, "id", wd_text(late_id));
  cbor_item_t *late_cancelled = NULL;
  assert(wd_client_call(&speaker, "session.cancel", late_cancel,
                        &late_cancelled) == 0);
  wd_free(late_cancelled);
  assert(count_prompted(&watcher, "carried text", 2) == 2);
  free(late_id);
  close(speaker.fd);
  close(watcher.fd);

  wd_client admin;
  assert(wd_client_connect(&admin, sock) == 0);
  assert(wd_client_handshake(&admin) == 0);
  cbor_item_t *off = wd_map(2);
  wd_put(off, "name", wd_text("skills"));
  wd_put(off, "enabled", cbor_build_bool(false));
  cbor_item_t *off_done = NULL;
  assert(wd_client_call(&admin, "module.enable", off, &off_done) == 0);
  wd_free(off_done);
  cbor_item_t *listed = NULL;
  assert(wd_client_call(&admin, "module.list", wd_map(0), &listed) == 0);
  cbor_item_t *items = wd_get(listed, "modules");
  bool saw_disabled = false;
  for (size_t i = 0; i < cbor_array_size(items); i++) {
    cbor_item_t *entry = cbor_array_get(items, i);
    char *name = wd_text_copy(wd_get(entry, "name"));
    bool enabled_here = true;
    wd_bool_of(wd_get(entry, "enabled"), &enabled_here);
    if (name != NULL && strcmp(name, "skills") == 0 && !enabled_here)
      saw_disabled = true;
    free(name);
    cbor_decref(&entry);
  }
  assert(saw_disabled);
  wd_free(listed);
  cbor_item_t *back_on = wd_map(2);
  wd_put(back_on, "name", wd_text("skills"));
  wd_put(back_on, "enabled", cbor_build_bool(true));
  cbor_item_t *back_done = NULL;
  assert(wd_client_call(&admin, "module.enable", back_on, &back_done) == 0);
  wd_free(back_done);
  close(admin.fd);

  kill(pid, SIGTERM);
  int status = 0;
  waitpid(pid, &status, 0);
  stop_server(endpoint);
  drop_test_cgroup(getenv("WARD_CGROUP"));

  printf("native gate passed: ward-native drove a session over the socket\n");
  return 0;
}
