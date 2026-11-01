/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/turn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/http.h"
#include "proto/codec.h"

#define WD_TURN_DEPTH 8
#define WD_TURN_FUEL 50000000ull
#define WD_TURN_MEMORY (16 * 1024 * 1024)

struct wd_turn {
  wd_engine *engine;
  char *endpoint;
  wd_module *driver_module;
  wd_module *provider_module;
  wd_module *engine_module;
  wd_instance *driver;
  wd_instance *provider;
  wd_instance *eng;
};

static const wd_limits TURN_LIMITS = {.fuel = WD_TURN_FUEL,
                                      .max_memory = WD_TURN_MEMORY,
                                      .max_instances = 4};

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

static wd_module *load_module(wd_engine *engine, const char *dir,
                              const char *file, const wd_module_spec *spec,
                              char **err) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, file);
  size_t len = 0;
  uint8_t *bytes = read_file(path, &len);
  if (bytes == NULL) {
    *err = strdup("cannot read module");
    return NULL;
  }
  wd_module *module = wd_module_load(engine, spec, bytes, len, err);
  free(bytes);
  return module;
}

static int net_request(wd_turn *turn, const uint8_t *in, size_t in_len,
                       uint8_t **out, size_t *out_len, char **err) {
  if (turn->endpoint == NULL) {
    *err = strdup("no endpoint configured");
    return -1;
  }
  cbor_item_t *request = wd_decode(in, in_len);
  if (request == NULL) {
    *err = strdup("net.request: parameters are not cbor");
    return -1;
  }
  char *path = wd_text_copy(wd_get(request, "path"));
  char *body = wd_text_copy(wd_get(request, "body"));
  wd_free(request);
  char url[512];
  snprintf(url, sizeof(url), "%s%s", turn->endpoint, path != NULL ? path : "");
  free(path);
  if (!wd_http_loopback(url)) {
    free(body);
    *err = strdup("net.request: the endpoint is not loopback");
    return -1;
  }
  wd_http_response response;
  int rc = wd_http_request(url, "POST", body != NULL ? body : "", 300000,
                           4u << 20, &response, err);
  free(body);
  if (rc != 0) return -1;
  size_t total = 4 + response.len;
  uint8_t *bytes = malloc(total);
  if (bytes == NULL) {
    wd_http_response_free(&response);
    *err = strdup("net.request: out of memory");
    return -1;
  }
  bytes[0] = (uint8_t)(response.status & 0xff);
  bytes[1] = (uint8_t)((response.status >> 8) & 0xff);
  bytes[2] = (uint8_t)((response.status >> 16) & 0xff);
  bytes[3] = (uint8_t)((response.status >> 24) & 0xff);
  if (response.len > 0) memcpy(bytes + 4, response.body, response.len);
  wd_http_response_free(&response);
  *out = bytes;
  *out_len = total;
  return 0;
}

static int turn_capability(void *ctx, const char *capability,
                           const uint8_t *in, size_t in_len, uint8_t **out,
                           size_t *out_len, char **err) {
  wd_turn *turn = ctx;
  if (strcmp(capability, "engine.call") == 0)
    return wd_instance_call(turn->eng, in, in_len, out, out_len, err);
  if (strcmp(capability, "net.request") == 0)
    return net_request(turn, in, in_len, out, out_len, err);
  *err = strdup("unknown capability");
  return -1;
}

wd_turn *wd_turn_new(wd_engine *engine, const char *module_dir, char **err) {
  wd_turn *turn = calloc(1, sizeof(*turn));
  if (turn == NULL) {
    *err = strdup("out of memory");
    return NULL;
  }
  turn->engine = engine;
  const char *endpoint = getenv("WARD_ENDPOINT");
  if (endpoint != NULL) turn->endpoint = strdup(endpoint);
  static const char *const driver_caps[] = {"engine.call"};
  static const char *const provider_caps[] = {"net.request"};
  static const char *const no_caps[] = {NULL};
  wd_module_spec engine_spec = {"engine", no_caps, 0, true};
  wd_module_spec driver_spec = {"driver", driver_caps, 1, false};
  wd_module_spec provider_spec = {"provider", provider_caps, 1, false};

  turn->engine_module = load_module(engine, module_dir, "engine.wasm",
                                    &engine_spec, err);
  if (turn->engine_module == NULL) goto fail;
  turn->driver_module = load_module(engine, module_dir, "driver.wasm",
                                    &driver_spec, err);
  if (turn->driver_module == NULL) goto fail;
  turn->provider_module = load_module(engine, module_dir, "provider.wasm",
                                      &provider_spec, err);
  if (turn->provider_module == NULL) goto fail;

  turn->eng = wd_instance_new(turn->engine_module, no_caps, 0, &TURN_LIMITS,
                              NULL, NULL, err);
  if (turn->eng == NULL) goto fail;
  turn->provider = wd_instance_new(turn->provider_module, provider_caps, 1,
                                   &TURN_LIMITS, turn_capability, turn, err);
  if (turn->provider == NULL) goto fail;
  turn->driver = wd_instance_new(turn->driver_module, driver_caps, 1,
                                 &TURN_LIMITS, turn_capability, turn, err);
  if (turn->driver == NULL) goto fail;
  return turn;

fail:
  wd_turn_free(turn);
  return NULL;
}

void wd_turn_free(wd_turn *turn) {
  if (turn == NULL) return;
  wd_instance_free(turn->driver);
  wd_instance_free(turn->provider);
  wd_instance_free(turn->eng);
  wd_module_free(turn->driver_module);
  wd_module_free(turn->provider_module);
  wd_module_free(turn->engine_module);
  free(turn->endpoint);
  free(turn);
}

static wd_instance *instance_for(wd_turn *turn, const char *module,
                                 const char *name) {
  if (strcmp(module, "provider") == 0 && strcmp(name, "chat") == 0)
    return turn->provider;
  return NULL;
}

static uint8_t *encode_keep(cbor_item_t *item, size_t *len) {
  size_t need = cbor_serialized_size(item);
  if (need == 0) return NULL;
  uint8_t *buf = malloc(need);
  if (buf == NULL) return NULL;
  size_t written = cbor_serialize(item, buf, need);
  if (written == 0) {
    free(buf);
    return NULL;
  }
  *len = written;
  return buf;
}

static int invoke(wd_instance *instance, cbor_item_t *payload,
                  cbor_item_t **result, char **err) {
  size_t len = 0;
  uint8_t *bytes = encode_keep(payload, &len);
  if (bytes == NULL) {
    *err = strdup("cannot encode a module payload");
    return -1;
  }
  uint8_t *out = NULL;
  size_t out_len = 0;
  int rc = wd_instance_call(instance, bytes, len, &out, &out_len, err);
  free(bytes);
  if (rc != 0) return -1;
  cbor_item_t *reply = wd_decode(out, out_len);
  free(out);
  if (reply == NULL) {
    *err = strdup("module returned invalid CBOR");
    return -1;
  }
  *result = reply;
  return 0;
}

static int process_action(wd_turn *turn, cbor_item_t *action, wd_turn_emit emit,
                          void *ctx, cbor_item_t **next, char **err) {
  char *kind = wd_text_copy(wd_get(action, "kind"));
  if (kind == NULL) {
    *err = strdup("action has no kind");
    return -1;
  }
  int rc = 0;
  if (strcmp(kind, "emit") == 0) {
    char *event = wd_text_copy(wd_get(action, "event"));
    if (event == NULL) {
      *err = strdup("emit action has no event");
      rc = -1;
    } else {
      rc = emit(ctx, event, wd_get(action, "payload"), err);
      free(event);
    }
  } else if (strcmp(kind, "invoke") == 0) {
    char *module = wd_text_copy(wd_get(action, "module"));
    char *name = wd_text_copy(wd_get(action, "name"));
    cbor_item_t *payload = wd_get(action, "payload");
    wd_instance *instance =
        module != NULL && name != NULL ? instance_for(turn, module, name) : NULL;
    if (instance == NULL || payload == NULL) {
      *err = strdup("no such module action");
      rc = -1;
    } else {
      cbor_item_t *result = NULL;
      rc = invoke(instance, payload, &result, err);
      if (rc == 0) {
        cbor_item_t *message = wd_map(2);
        wd_put(message, "kind", wd_text("result"));
        wd_put(message, "payload", result);
        *next = message;
      }
    }
    free(module);
    free(name);
  } else {
    *err = strdup("unknown action kind");
    rc = -1;
  }
  free(kind);
  return rc;
}

int wd_turn_prompt(wd_turn *turn, const char *text, wd_turn_emit emit, void *ctx,
                   char **err) {
  cbor_item_t *message = wd_map(2);
  if (message == NULL) {
    *err = strdup("out of memory");
    return -1;
  }
  wd_put(message, "kind", wd_text("step"));
  wd_put(message, "prompt", wd_text(text));

  for (int depth = 0; depth < WD_TURN_DEPTH; depth++) {
    size_t len = 0;
    uint8_t *bytes = wd_encode(message, &len);
    message = NULL;
    if (bytes == NULL) {
      *err = strdup("cannot encode the driver message");
      return -1;
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = wd_instance_call(turn->driver, bytes, len, &out, &out_len, err);
    free(bytes);
    if (rc != 0) return -1;
    cbor_item_t *reply = wd_decode(out, out_len);
    free(out);
    if (reply == NULL) {
      *err = strdup("driver returned invalid CBOR");
      return -1;
    }

    cbor_item_t *actions = wd_get(reply, "actions");
    size_t count = 0;
    cbor_item_t **items = NULL;
    if (actions != NULL && cbor_isa_array(actions)) {
      items = cbor_array_handle(actions);
      count = cbor_array_size(actions);
    }
    message = NULL;
    int failed = 0;
    for (size_t i = 0; i < count && !failed; i++) {
      cbor_item_t *next = NULL;
      if (process_action(turn, items[i], emit, ctx, &next, err) != 0) {
        failed = 1;
        break;
      }
      if (next != NULL) {
        message = next;
        break;
      }
    }
    wd_free(reply);
    if (failed) {
      wd_free(message);
      return -1;
    }
    if (message == NULL) return 0;
  }
  *err = strdup("turn exceeded its action depth");
  wd_free(message);
  return -1;
}
