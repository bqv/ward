/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client/client.h"
#include "core/util.h"
#include "proto/codec.h"

JNIEXPORT jlong JNICALL Java_uk_xa0_ward_WardClient_connect(JNIEnv *env,
                                                            jobject self,
                                                            jstring path) {
  (void)self;
  const char *text = (*env)->GetStringUTFChars(env, path, NULL);
  if (text == NULL) return 0;
  wd_client *client = calloc(1, sizeof(*client));
  int rc = client != NULL ? wd_client_connect(client, text) : -1;
  (*env)->ReleaseStringUTFChars(env, path, text);
  if (rc != 0) {
    free(client);
    return 0;
  }
  if (wd_client_handshake(client) != 0) {
    close(client->fd);
    free(client);
    return 0;
  }
  return (jlong)(intptr_t)client;
}

JNIEXPORT void JNICALL Java_uk_xa0_ward_WardClient_close(JNIEnv *env,
                                                         jobject self,
                                                         jlong handle) {
  (void)env;
  (void)self;
  wd_client *client = (wd_client *)(intptr_t)handle;
  if (client == NULL) return;
  close(client->fd);
  free(client);
}

JNIEXPORT jstring JNICALL Java_uk_xa0_ward_WardClient_createSession(
    JNIEnv *env, jobject self, jlong handle, jstring cwd) {
  (void)self;
  wd_client *client = (wd_client *)(intptr_t)handle;
  if (client == NULL) return NULL;
  const char *text = (*env)->GetStringUTFChars(env, cwd, NULL);
  if (text == NULL) return NULL;
  cbor_item_t *params = wd_map(1);
  wd_put(params, "cwd", wd_text(text));
  (*env)->ReleaseStringUTFChars(env, cwd, text);
  cbor_item_t *result = NULL;
  int rc = wd_client_call(client, "session.create", params, &result);
  if (rc != 0 || result == NULL) return NULL;
  char *id = wd_text_copy(wd_get(wd_get(result, "session"), "id"));
  wd_free(result);
  if (id == NULL) return NULL;
  jstring out = (*env)->NewStringUTF(env, id);
  free(id);
  return out;
}

JNIEXPORT jboolean JNICALL Java_uk_xa0_ward_WardClient_prompt(
    JNIEnv *env, jobject self, jlong handle, jstring session, jstring text) {
  (void)self;
  wd_client *client = (wd_client *)(intptr_t)handle;
  if (client == NULL) return JNI_FALSE;
  const char *id = (*env)->GetStringUTFChars(env, session, NULL);
  const char *body = (*env)->GetStringUTFChars(env, text, NULL);
  if (id == NULL || body == NULL) return JNI_FALSE;
  cbor_item_t *params = wd_map(2);
  wd_put(params, "id", wd_text(id));
  wd_put(params, "text", wd_text(body));
  (*env)->ReleaseStringUTFChars(env, session, id);
  (*env)->ReleaseStringUTFChars(env, text, body);
  cbor_item_t *result = NULL;
  int rc = wd_client_call(client, "session.prompt", params, &result);
  if (rc != 0 || result == NULL) return JNI_FALSE;
  wd_free(result);
  return JNI_TRUE;
}

JNIEXPORT jbyteArray JNICALL Java_uk_xa0_ward_WardClient_call(JNIEnv *env,
                                                              jobject self,
                                                              jlong handle,
                                                              jstring method,
                                                              jbyteArray params) {
  (void)self;
  wd_client *client = (wd_client *)(intptr_t)handle;
  if (client == NULL) return NULL;
  const char *name = (*env)->GetStringUTFChars(env, method, NULL);
  if (name == NULL) return NULL;
  jsize params_len = (*env)->GetArrayLength(env, params);
  jbyte *params_bytes = (*env)->GetByteArrayElements(env, params, NULL);
  cbor_item_t *request = wd_decode((const uint8_t *)params_bytes,
                                   (size_t)params_len);
  (*env)->ReleaseByteArrayElements(env, params, params_bytes, JNI_ABORT);
  cbor_item_t *result = NULL;
  int rc = -1;
  if (request != NULL) {
    rc = wd_client_call(client, name, request, &result);
    wd_free(request);
  }
  (*env)->ReleaseStringUTFChars(env, method, name);
  if (rc != 0 || result == NULL) return NULL;
  size_t len = 0;
  uint8_t *bytes = wd_encode(result, &len);
  if (bytes == NULL) return NULL;
  jbyteArray out = (*env)->NewByteArray(env, (jsize)len);
  if (out != NULL) (*env)->SetByteArrayRegion(env, out, 0, (jsize)len,
                                              (const jbyte *)bytes);
  free(bytes);
  return out;
}
