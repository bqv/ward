/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "wd_abi.h"

#define HEAP_SIZE 8192u
#define REPLY_SIZE 2048u
#define TEXT_SIZE 512u

static uint8_t heap[HEAP_SIZE];
static uint32_t heap_at;
static uint8_t reply[REPLY_SIZE];
static uint8_t callbuf[1024];

__attribute__((import_module("ward"), import_name("cap")))
extern void ward_cap(uint32_t at, uint32_t len, uint32_t out);

__attribute__((export_name("ward_alloc")))
uint32_t ward_alloc(uint32_t len) {
  if (heap_at + len > HEAP_SIZE) return 0;
  uint32_t at = (uint32_t)(uintptr_t)(heap + heap_at);
  heap_at += len;
  return at;
}

static void pair(uint32_t out, const uint8_t *data, uint32_t n) {
  uint32_t at = (uint32_t)(uintptr_t)reply;
  for (uint32_t i = 0; i < n && i < REPLY_SIZE; i++) reply[i] = data[i];
  uint8_t *o = (uint8_t *)(uintptr_t)out;
  wd_put32(o, at);
  wd_put32(o + 4, n);
}

static int cap_call(const char *capability, const uint8_t *payload, uint32_t len,
                    uint8_t *out, uint32_t *out_len) {
  uint32_t name_len = wd_len(capability);
  if (4 + name_len + len > sizeof(callbuf)) return -1;
  wd_put32(callbuf, name_len);
  for (uint32_t i = 0; i < name_len; i++) callbuf[4 + i] = (uint8_t)capability[i];
  for (uint32_t i = 0; i < len; i++) callbuf[4 + name_len + i] = payload[i];
  uint32_t desc[2];
  ward_cap((uint32_t)(uintptr_t)callbuf, 4 + name_len + len,
           (uint32_t)(uintptr_t)desc);
  if (desc[1] > REPLY_SIZE) return -1;
  *out_len = desc[1];
  const uint8_t *p = (const uint8_t *)(uintptr_t)desc[0];
  for (uint32_t i = 0; i < desc[1]; i++) out[i] = p[i];
  return 0;
}

static int clock_text(char *out, uint32_t cap) {
  uint8_t payload[8];
  wd_w w;
  wd_w_init(&w, payload, sizeof(payload));
  wd_w_map(&w, 0);
  uint8_t answer[REPLY_SIZE];
  uint32_t answer_len = 0;
  if (cap_call("clock.now", payload, w.at, answer, &answer_len) != 0) return -1;
  wd_r r = {answer, answer_len, 0, 0};
  uint32_t n = 0;
  if (wd_r_map(&r, &n) != 0) return -1;
  out[0] = '\0';
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return -1;
    if (wd_key_eq(kp, kn, "text")) {
      if (wd_r_text(&r, out, cap, 0) != 0) return -1;
    } else if (wd_r_skip(&r) != 0) {
      return -1;
    }
  }
  return 0;
}

static int policy_check(const char *tool) {
  uint8_t payload[256];
  wd_w w;
  wd_w_init(&w, payload, sizeof(payload));
  wd_w_map(&w, 1);
  wd_w_textz(&w, "tool");
  wd_w_textz(&w, tool);
  if (w.failed) return 0;
  uint8_t answer[REPLY_SIZE];
  uint32_t answer_len = 0;
  if (cap_call("policy.check", payload, w.at, answer, &answer_len) != 0) return 0;
  wd_r r = {answer, answer_len, 0, 0};
  uint32_t n = 0;
  char decision[TEXT_SIZE];
  decision[0] = '\0';
  if (wd_r_map(&r, &n) != 0) return 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return 0;
    if (wd_key_eq(kp, kn, "decision")) {
      if (wd_r_text(&r, decision, sizeof(decision), 0) != 0) return 0;
    } else if (wd_r_skip(&r) != 0) {
      return 0;
    }
  }
  if (wd_key_eq((const uint8_t *)decision, wd_len(decision), "allow")) return 1;
  if (wd_key_eq((const uint8_t *)decision, wd_len(decision), "ask")) return 2;
  return 0;
}

static int engine_call(const uint8_t *payload, uint32_t len, uint8_t *out,
                       uint32_t *out_len) {
  static const char name[] = "engine.call";
  uint32_t name_len = sizeof(name) - 1;
  if (4 + name_len + len > sizeof(callbuf)) return -1;
  wd_put32(callbuf, name_len);
  for (uint32_t i = 0; i < name_len; i++) callbuf[4 + i] = (uint8_t)name[i];
  for (uint32_t i = 0; i < len; i++) callbuf[4 + name_len + i] = payload[i];
  uint32_t desc[2];
  ward_cap((uint32_t)(uintptr_t)callbuf, 4 + name_len + len,
           (uint32_t)(uintptr_t)desc);
  if (desc[1] > REPLY_SIZE) return -1;
  *out_len = desc[1];
  const uint8_t *p = (const uint8_t *)(uintptr_t)desc[0];
  for (uint32_t i = 0; i < desc[1]; i++) out[i] = p[i];
  return 0;
}

static void emit_text_delta(uint32_t out, const char *text) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "actions");
  wd_w_array(&w, 2);
  wd_w_map(&w, 3);
  wd_w_textz(&w, "kind");
  wd_w_textz(&w, "emit");
  wd_w_textz(&w, "event");
  wd_w_textz(&w, "text.delta");
  wd_w_textz(&w, "payload");
  wd_w_map(&w, 1);
  wd_w_textz(&w, "text");
  wd_w_textz(&w, text);
  wd_w_map(&w, 3);
  wd_w_textz(&w, "kind");
  wd_w_textz(&w, "emit");
  wd_w_textz(&w, "event");
  wd_w_textz(&w, "turn.end");
  wd_w_textz(&w, "payload");
  wd_w_map(&w, 0);
  if (!w.failed) pair(out, reply, w.at);
}

static void invoke_provider(uint32_t out, const char *text) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "actions");
  wd_w_array(&w, 1);
  wd_w_map(&w, 4);
  wd_w_textz(&w, "kind");
  wd_w_textz(&w, "invoke");
  wd_w_textz(&w, "module");
  wd_w_textz(&w, "provider");
  wd_w_textz(&w, "name");
  wd_w_textz(&w, "chat");
  wd_w_textz(&w, "payload");
  wd_w_map(&w, 1);
  wd_w_textz(&w, "prompt");
  wd_w_textz(&w, text);
  if (!w.failed) pair(out, reply, w.at);
}

static int text_prefix(const char *text, const char *prefix) {
  for (uint32_t i = 0; prefix[i] != '\0'; i++)
    if (text[i] != prefix[i]) return 0;
  return 1;
}

static void invoke_edit(uint32_t out, const char *path, const char *find,
                        const char *replace, int approve) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "actions");
  wd_w_array(&w, 1);
  wd_w_map(&w, approve ? 7 : 5);
  wd_w_textz(&w, "kind");
  wd_w_textz(&w, "invoke");
  wd_w_textz(&w, "module");
  wd_w_textz(&w, "tools");
  wd_w_textz(&w, "name");
  wd_w_textz(&w, "edit");
  wd_w_textz(&w, "call");
  wd_w_uint(&w, 1);
  if (approve) {
    wd_w_textz(&w, "approve");
    wd_w_bool(&w, 1);
    wd_w_textz(&w, "reason");
    wd_w_textz(&w, "edit a file in the session directory");
  }
  wd_w_textz(&w, "payload");
  wd_w_map(&w, 2);
  wd_w_textz(&w, "tool");
  wd_w_textz(&w, "edit");
  wd_w_textz(&w, "args");
  wd_w_map(&w, 3);
  wd_w_textz(&w, "path");
  wd_w_textz(&w, path);
  wd_w_textz(&w, "find");
  wd_w_textz(&w, find);
  wd_w_textz(&w, "replace");
  wd_w_textz(&w, replace);
  if (!w.failed) pair(out, reply, w.at);
}

static uint32_t split_token(const char *text, uint32_t at, char *out,
                            uint32_t cap) {
  uint32_t i = 0;
  while (text[at] == ' ') at++;
  while (text[at] != '\0' && text[at] != ' ' && i + 1 < cap)
    out[i++] = text[at++];
  out[i] = '\0';
  return at;
}

static void invoke_tool(uint32_t out, const char *tool, const char *path,
                        const char *text, int approve) {
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "actions");
  wd_w_array(&w, 1);
  wd_w_map(&w, approve ? 7 : 5);
  wd_w_textz(&w, "kind");
  wd_w_textz(&w, "invoke");
  wd_w_textz(&w, "module");
  wd_w_textz(&w, "tools");
  wd_w_textz(&w, "name");
  wd_w_textz(&w, tool);
  wd_w_textz(&w, "call");
  wd_w_uint(&w, 1);
  if (approve) {
    wd_w_textz(&w, "approve");
    wd_w_bool(&w, 1);
    wd_w_textz(&w, "reason");
    wd_w_textz(&w, "write a file in the session directory");
  }
  wd_w_textz(&w, "payload");
  wd_w_map(&w, 2);
  wd_w_textz(&w, "tool");
  wd_w_textz(&w, tool);
  wd_w_textz(&w, "args");
  wd_w_map(&w, text[0] != '\0' ? 2 : 1);
  wd_w_textz(&w, "path");
  wd_w_textz(&w, path);
  if (text[0] != '\0') {
    wd_w_textz(&w, "text");
    wd_w_textz(&w, text);
  }
  if (!w.failed) pair(out, reply, w.at);
}

__attribute__((export_name("ward_call")))
void ward_call(uint32_t at, uint32_t len, uint32_t out) {
  heap_at = 0;
  wd_r r = {(const uint8_t *)(uintptr_t)at, len, 0, 0};
  uint32_t n = 0;
  if (wd_r_map(&r, &n) != 0) return;
  int step = 0;
  int result = 0;
  char kind[TEXT_SIZE];
  char text[TEXT_SIZE];
  char payload[TEXT_SIZE];
  uint32_t plen = 0;
  kind[0] = '\0';
  text[0] = '\0';
  payload[0] = '\0';
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *kp = 0;
    uint32_t kn = 0;
    if (wd_r_text_ptr(&r, &kp, &kn) != 0) return;
    if (wd_key_eq(kp, kn, "kind")) {
      if (wd_r_text(&r, kind, sizeof(kind), &plen) != 0) return;
      step = wd_key_eq((const uint8_t *)kind, plen, "step");
      result = wd_key_eq((const uint8_t *)kind, plen, "result");
    } else if (wd_key_eq(kp, kn, "text")) {
      if (wd_r_text(&r, text, sizeof(text), &plen) != 0) return;
    } else if (wd_key_eq(kp, kn, "prompt")) {
      if (wd_r_text(&r, text, sizeof(text), &plen) != 0) return;
    } else if (wd_key_eq(kp, kn, "payload")) {
      uint32_t pn = 0;
      if (wd_r_map(&r, &pn) != 0) return;
      for (uint32_t k = 0; k < pn; k++) {
        const uint8_t *pk = 0;
        uint32_t pl = 0;
        if (wd_r_text_ptr(&r, &pk, &pl) != 0) return;
        if (wd_key_eq(pk, pl, "text")) {
          if (wd_r_text(&r, payload, sizeof(payload), &plen) != 0) return;
        } else if (wd_key_eq(pk, pl, "error")) {
          if (wd_r_text(&r, payload, sizeof(payload), &plen) != 0) return;
        } else if (wd_r_skip(&r) != 0) {
          return;
        }
      }
    } else if (wd_r_skip(&r) != 0) {
      return;
    }
  }
  if (step) {
    if (text_prefix(text, "/read ")) {
      int verdict = policy_check("read");
      if (verdict == 0) {
        emit_text_delta(out, "the policy denied read");
        return;
      }
      invoke_tool(out, "read", text + 6, "", verdict == 2);
      return;
    }
    if (text_prefix(text, "/write ")) {
      char path[TEXT_SIZE];
      char body[TEXT_SIZE];
      uint32_t at = 7;
      uint32_t i = 0;
      while (text[at] != '\0' && text[at] != ' ' && i + 1 < sizeof(path))
        path[i++] = text[at++];
      path[i] = '\0';
      while (text[at] == ' ') at++;
      i = 0;
      while (text[at] != '\0' && i + 1 < sizeof(body)) body[i++] = text[at++];
      body[i] = '\0';
      if (path[0] != '\0') {
        int verdict = policy_check("write");
        if (verdict == 0) {
          emit_text_delta(out, "the policy denied write");
          return;
        }
        invoke_tool(out, "write", path, body, verdict == 2);
        return;
      }
    }
    if (text_prefix(text, "/grep ")) {
      int verdict = policy_check("grep");
      if (verdict == 0) {
        emit_text_delta(out, "the policy denied grep");
        return;
      }
      invoke_tool(out, "grep", "", text + 6, verdict == 2);
      return;
    }
    if (text_prefix(text, "/edit ")) {
      char path[TEXT_SIZE];
      char find[TEXT_SIZE];
      char replace[TEXT_SIZE];
      uint32_t at = split_token(text, 6, path, sizeof(path));
      at = split_token(text, at, find, sizeof(find));
      split_token(text, at, replace, sizeof(replace));
      if (path[0] == '\0' || find[0] == '\0' || replace[0] == '\0') {
        emit_text_delta(out, "edit needs a path, a find and a replace");
        return;
      }
      int verdict = policy_check("edit");
      if (verdict == 0) {
        emit_text_delta(out, "the policy denied edit");
        return;
      }
      invoke_edit(out, path, find, replace, verdict == 2);
      return;
    }
    if (text_prefix(text, "/run ")) {
      int verdict = policy_check("exec");
      if (verdict == 0) {
        emit_text_delta(out, "the policy denied exec");
        return;
      }
      invoke_tool(out, "exec", text + 5, text + 5, verdict == 2);
      return;
    }
    char clock[TEXT_SIZE];
    char stamped[TEXT_SIZE];
    stamped[0] = '\0';
    if (clock_text(clock, sizeof(clock)) == 0 && clock[0] != '\0') {
      uint32_t at = 0;
      stamped[at++] = '[';
      for (uint32_t i = 0; clock[i] != '\0' && at + 4 < sizeof(stamped); i++)
        stamped[at++] = clock[i];
      stamped[at++] = ']';
      stamped[at++] = ' ';
      for (uint32_t i = 0; text[i] != '\0' && at + 1 < sizeof(stamped); i++)
        stamped[at++] = text[i];
      stamped[at] = '\0';
    } else {
      for (uint32_t i = 0; text[i] != '\0' && i + 1 < sizeof(stamped); i++)
        stamped[i] = text[i];
      stamped[wd_len(text)] = '\0';
    }
    wd_w w;
    uint8_t message[1024];
    wd_w_init(&w, message, sizeof(message));
    wd_w_map(&w, 1);
    wd_w_textz(&w, "start");
    wd_w_textz(&w, stamped);
    if (w.failed) return;
    uint8_t answer[REPLY_SIZE];
    uint32_t answer_len = 0;
    if (engine_call(message, w.at, answer, &answer_len) != 0) return;
    wd_r er = {answer, answer_len, 0, 0};
    uint32_t en = 0;
    char prompt[TEXT_SIZE];
    prompt[0] = '\0';
    if (wd_r_map(&er, &en) != 0) return;
    for (uint32_t i = 0; i < en; i++) {
      const uint8_t *kp = 0;
      uint32_t kn = 0;
      if (wd_r_text_ptr(&er, &kp, &kn) != 0) return;
      if (wd_key_eq(kp, kn, "prompt")) {
        if (wd_r_text(&er, prompt, sizeof(prompt), 0) != 0) return;
      } else if (wd_r_skip(&er) != 0) {
        return;
      }
    }
    invoke_provider(out, prompt);
    return;
  }
  if (result) {
    wd_w w;
    uint8_t message[1024];
    wd_w_init(&w, message, sizeof(message));
    wd_w_map(&w, 1);
    wd_w_textz(&w, "result");
    wd_w_textz(&w, payload[0] != '\0' ? payload : text);
    if (w.failed) return;
    uint8_t answer[REPLY_SIZE];
    uint32_t answer_len = 0;
    if (engine_call(message, w.at, answer, &answer_len) != 0) return;
    wd_r er = {answer, answer_len, 0, 0};
    uint32_t en = 0;
    char done[TEXT_SIZE];
    done[0] = '\0';
    if (wd_r_map(&er, &en) != 0) return;
    for (uint32_t i = 0; i < en; i++) {
      const uint8_t *kp = 0;
      uint32_t kn = 0;
      if (wd_r_text_ptr(&er, &kp, &kn) != 0) return;
      if (wd_key_eq(kp, kn, "text")) {
        if (wd_r_text(&er, done, sizeof(done), 0) != 0) return;
      } else if (wd_r_skip(&er) != 0) {
        return;
      }
    }
    emit_text_delta(out, done);
    return;
  }
  wd_w w;
  wd_w_init(&w, reply, REPLY_SIZE);
  wd_w_map(&w, 1);
  wd_w_textz(&w, "actions");
  wd_w_array(&w, 0);
  if (!w.failed) pair(out, reply, w.at);
}
