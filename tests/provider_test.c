/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/codec.h"
#include "wasm/host.h"

static const wd_limits LIMITS = {.fuel = 50000000, .max_memory = 16 << 20,
                                 .max_instances = 2};

static uint8_t *read_file(const char *name, size_t *len) {
  const char *dir = getenv("WARD_MODULE_DIR");
  assert(dir != NULL);
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "rb");
  assert(f != NULL);
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = malloc((size_t)size);
  assert(buf != NULL);
  assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
  fclose(f);
  *len = (size_t)size;
  return buf;
}

int main(void) {
  wd_engine *engine = wd_engine_new(NULL);
  assert(engine != NULL);
  size_t len = 0;
  uint8_t *bytes = read_file("provider.wasm", &len);
  static const char *const caps[] = {"net.request"};
  wd_module_spec spec = {"provider", caps, 1, false};
  char *err = NULL;
  wd_module *module = wd_module_load(engine, &spec, bytes, len, &err);
  assert(module != NULL);
  wd_instance *inst =
      wd_instance_new(module, caps, 1, &LIMITS, NULL, NULL, &err);
  assert(inst != NULL);

  cbor_item_t *history = wd_array(1);
  cbor_item_t *entry = wd_map(2);
  wd_put(entry, "role", wd_text("system"));
  wd_put(entry, "text", wd_text("be kind to the tools"));
  assert(cbor_array_push(history, entry));
  cbor_decref(&entry);

  cbor_item_t *payload = wd_map(3);
  wd_put(payload, "prompt", wd_text("hello"));
  wd_put(payload, "model", wd_text("qwen1_5-1_8b-chat-q4_k_m"));
  wd_put(payload, "history", history);

  size_t in_len = 0;
  uint8_t *in = wd_encode(payload, &in_len);
  assert(in != NULL);
  uint8_t *out = NULL;
  size_t out_len = 0;
  assert(wd_instance_call(inst, in, in_len, &out, &out_len, &err) == 0);
  free(in);
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  assert(reply != NULL);
  cbor_item_t *request = wd_get(reply, "request");
  assert(request != NULL);
  char *body = wd_text_copy(wd_get(request, "body"));
  assert(body != NULL);
  assert(strstr(body, "be kind to the tools") != NULL);
  assert(strstr(body, "hello") != NULL);
  assert(strstr(body, "\"model\":\"qwen1_5-1_8b-chat-q4_k_m\"") != NULL);
  assert(strstr(body, "\"max_tokens\":1024") != NULL);
  {
    int messages = 0;
    for (const char *at = body; (at = strstr(at, "\"messages\"")) != NULL; at++)
      messages++;
    assert(messages == 1);
    assert(strstr(body, "[\",") == NULL);
    assert(body[0] == '{' && body[strlen(body) - 1] == '}');
  }
  free(body);
  wd_free(reply);

  static const char stream[] =
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
      "\"content\":null}}],\"object\":\"chat.completion.chunk\"}\n\n"
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"}}],"
      "\"object\":\"chat.completion.chunk\"}\n\n"
      "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" there\"}}],"
      "\"object\":\"chat.completion.chunk\"}\n\n"
      "data: [DONE]\n\n";
  cbor_item_t *second = wd_map(2);
  wd_put(second, "status", wd_uint(200));
  wd_put(second, "body", wd_text(stream));
  size_t second_len = 0;
  uint8_t *second_bytes = wd_encode(second, &second_len);
  assert(second_bytes != NULL);
  uint8_t *second_out = NULL;
  size_t second_out_len = 0;
  assert(wd_instance_call(inst, second_bytes, second_len, &second_out,
                          &second_out_len, &err) == 0);
  free(second_bytes);
  cbor_item_t *answer = wd_decode(second_out, second_out_len);
  free(second_out);
  assert(answer != NULL);
  char *text = wd_text_copy(wd_get(answer, "text"));
  assert(text != NULL);
  assert(strcmp(text, "Hello there") == 0);
  free(text);
  wd_free(answer);

  static const char tool_body[] =
      "data: {\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":"
      "{\"tool_calls\":[{\"function\":{\"name\":\"read\",\"arguments\":"
      "\"{\\\"path\\\":\\\"notes.txt\\\"}\"}}]}}]}\n\n";
  cbor_item_t *tool_phase = wd_map(2);
  wd_put(tool_phase, "status", wd_uint(200));
  wd_put(tool_phase, "body", wd_text(tool_body));
  size_t tool_len = 0;
  uint8_t *tool_bytes = wd_encode(tool_phase, &tool_len);
  assert(tool_bytes != NULL);
  uint8_t *tool_out = NULL;
  size_t tool_out_len = 0;
  assert(wd_instance_call(inst, tool_bytes, tool_len, &tool_out, &tool_out_len,
                          &err) == 0);
  free(tool_bytes);
  cbor_item_t *tool_reply = wd_decode(tool_out, tool_out_len);
  free(tool_out);
  assert(tool_reply != NULL);
  char *tool_name = wd_text_copy(wd_get(tool_reply, "tool"));
  char *tool_args = wd_text_copy(wd_get(tool_reply, "arguments"));
  assert(tool_name != NULL && strcmp(tool_name, "read") == 0);
  assert(tool_args != NULL && strstr(tool_args, "notes.txt") != NULL);
  free(tool_name);
  free(tool_args);
  wd_free(tool_reply);

  static const char two_calls[] =
      "data: {\"choices\":[{\"message\":{\"tool_calls\":["
      "{\"function\":{\"name\":\"read\",\"arguments\":\"{}\"}},"
      "{\"function\":{\"name\":\"write\",\"arguments\":\"{}\"}}]}}]}\n\n";
  cbor_item_t *two_phase = wd_map(2);
  wd_put(two_phase, "status", wd_uint(200));
  wd_put(two_phase, "body", wd_text(two_calls));
  size_t two_len = 0;
  uint8_t *two_bytes = wd_encode(two_phase, &two_len);
  assert(two_bytes != NULL);
  uint8_t *two_out = NULL;
  size_t two_out_len = 0;
  assert(wd_instance_call(inst, two_bytes, two_len, &two_out, &two_out_len,
                          &err) == 0);
  free(two_bytes);
  cbor_item_t *two_reply = wd_decode(two_out, two_out_len);
  free(two_out);
  assert(two_reply != NULL);
  char *two_tool = wd_text_copy(wd_get(two_reply, "tool"));
  char *two_text = wd_text_copy(wd_get(two_reply, "text"));
  assert(two_tool != NULL && two_tool[0] == '\0');
  assert(two_text != NULL && strstr(two_text, "more than one tool call") != NULL);
  free(two_tool);
  free(two_text);
  wd_free(two_reply);

  static const char lenient_body[] =
      "data: {\"choices\":[{\"delta\":{\"content\":\"I will read it "
      "{\\\"tool\\\":\\\"read\\\",\\\"path\\\":\\\"notes.txt"
      "\\\"}\"}}]}\n\ndata: [DONE]\n\n";
  cbor_item_t *lenient_phase = wd_map(2);
  wd_put(lenient_phase, "status", wd_uint(200));
  wd_put(lenient_phase, "body", wd_text(lenient_body));
  size_t lenient_len = 0;
  uint8_t *lenient_bytes = wd_encode(lenient_phase, &lenient_len);
  assert(lenient_bytes != NULL);
  uint8_t *lenient_out = NULL;
  size_t lenient_out_len = 0;
  assert(wd_instance_call(inst, lenient_bytes, lenient_len, &lenient_out,
                          &lenient_out_len, &err) == 0);
  free(lenient_bytes);
  cbor_item_t *lenient_reply = wd_decode(lenient_out, lenient_out_len);
  free(lenient_out);
  assert(lenient_reply != NULL);
  char *lenient_tool = wd_text_copy(wd_get(lenient_reply, "tool"));
  char *lenient_args = wd_text_copy(wd_get(lenient_reply, "arguments"));
  assert(lenient_tool != NULL && strcmp(lenient_tool, "read") == 0);
  assert(lenient_args != NULL && strstr(lenient_args, "notes.txt") != NULL);
  free(lenient_tool);
  free(lenient_args);
  wd_free(lenient_reply);

  static const char partial_body[] =
      "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"tool\\\":"
      "\\\"read\\\"}\"}}]}\n\ndata: [DONE]\n\n";
  cbor_item_t *partial_phase = wd_map(2);
  wd_put(partial_phase, "status", wd_uint(200));
  wd_put(partial_phase, "body", wd_text(partial_body));
  size_t partial_len = 0;
  uint8_t *partial_bytes = wd_encode(partial_phase, &partial_len);
  assert(partial_bytes != NULL);
  uint8_t *partial_out = NULL;
  size_t partial_out_len = 0;
  assert(wd_instance_call(inst, partial_bytes, partial_len, &partial_out,
                          &partial_out_len, &err) == 0);
  free(partial_bytes);
  cbor_item_t *partial_reply = wd_decode(partial_out, partial_out_len);
  free(partial_out);
  assert(partial_reply != NULL);
  char *partial_tool = wd_text_copy(wd_get(partial_reply, "tool"));
  char *partial_text = wd_text_copy(wd_get(partial_reply, "text"));
  assert(partial_tool != NULL && strcmp(partial_tool, "read") == 0);
  assert(partial_text != NULL && strstr(partial_text, "without a path") != NULL);
  char *partial_args = wd_text_copy(wd_get(partial_reply, "arguments"));
  assert(partial_args != NULL && partial_args[0] == '\0');
  free(partial_args);
  free(partial_tool);
  free(partial_text);
  wd_free(partial_reply);

  wd_instance_free(inst);
  wd_module_free(module);
  wd_engine_free(engine);
  free(bytes);
  printf("provider tests passed\n");
  return 0;
}
