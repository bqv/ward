/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/sse.h"

#define MAX_EVENTS 64
#define MAX_COPY 4096

typedef struct {
  char type[WD_SSE_MAX_TYPE + 1];
  size_t type_len;
  char data[MAX_COPY];
  size_t data_len;
  char id[128];
  size_t id_len;
} captured;

typedef struct {
  captured events[MAX_EVENTS];
  size_t count;
  int abort_at;
} collector;

static int collect(void *ctx, const wd_sse_event *event) {
  collector *c = ctx;
  assert(event->type != NULL);
  assert(event->type_len > 0 && event->type_len <= WD_SSE_MAX_TYPE);
  assert(event->data != NULL);
  assert(event->data_len <= WD_SSE_MAX_DATA);
  assert(event->id_len <= WD_SSE_MAX_ID);
  if (c->abort_at >= 0 && (int)c->count == c->abort_at) return 1;
  assert(c->count < MAX_EVENTS);
  captured *e = &c->events[c->count++];
  memcpy(e->type, event->type, event->type_len);
  e->type[event->type_len] = '\0';
  e->type_len = event->type_len;
  e->data_len = event->data_len < MAX_COPY ? event->data_len : MAX_COPY;
  memcpy(e->data, event->data, e->data_len);
  if (event->id_len > 0) {
    size_t n = event->id_len < sizeof(e->id) - 1 ? event->id_len : sizeof(e->id) - 1;
    memcpy(e->id, event->id, n);
    e->id[n] = '\0';
    e->id_len = event->id_len;
  } else {
    e->id[0] = '\0';
    e->id_len = 0;
  }
  return 0;
}

static void parse_len(const char *text, size_t len, collector *c) {
  wd_sse *sse = wd_sse_create(collect, c);
  assert(sse != NULL);
  assert(wd_sse_feed(sse, text, len) == 0);
  assert(wd_sse_finish(sse) == 0);
  assert(!wd_sse_failed(sse));
  wd_sse_destroy(sse);
}

static void parse(const char *text, collector *c) {
  parse_len(text, strlen(text), c);
}

static void test_spec_example(void) {
  collector c = {.abort_at = -1};
  parse("data: YHOO\ndata: +2\ndata: 10\n\n", &c);
  assert(c.count == 1);
  assert(strcmp(c.events[0].type, "message") == 0);
  assert(strcmp(c.events[0].data, "YHOO\n+2\n10") == 0);
}

static void test_type_and_reset(void) {
  collector c = {.abort_at = -1};
  parse("event: ping\ndata: one\n\ndata: two\n\n", &c);
  assert(c.count == 2);
  assert(strcmp(c.events[0].type, "ping") == 0);
  assert(strcmp(c.events[0].data, "one") == 0);
  assert(strcmp(c.events[1].type, "message") == 0);
  assert(strcmp(c.events[1].data, "two") == 0);
}

static void test_comments(void) {
  collector c = {.abort_at = -1};
  parse(": keepalive\n\ndata: x\n: trailing\n\n", &c);
  assert(c.count == 1);
  assert(strcmp(c.events[0].data, "x") == 0);
}

static void test_line_endings(void) {
  collector c = {.abort_at = -1};
  parse("data: a\r\n\r\ndata: b\r\rdata: c\n\n", &c);
  assert(c.count == 3);
  assert(strcmp(c.events[0].data, "a") == 0);
  assert(strcmp(c.events[1].data, "b") == 0);
  assert(strcmp(c.events[2].data, "c") == 0);
}

static void test_bare_field_and_spaces(void) {
  collector c = {.abort_at = -1};
  parse("data\ndata:  two\n\n", &c);
  assert(c.count == 1);
  assert(strcmp(c.events[0].data, "\n two") == 0);
}

static void test_id_persists(void) {
  collector c = {.abort_at = -1};
  parse("id: 7\ndata: a\n\ndata: b\n\n", &c);
  assert(c.count == 2);
  assert(strcmp(c.events[0].id, "7") == 0);
  assert(strcmp(c.events[1].id, "7") == 0);
}

static void test_bom(void) {
  collector c = {.abort_at = -1};
  parse("\xef\xbb\xbf" "data: a\n\n", &c);
  assert(c.count == 1);
  assert(strcmp(c.events[0].data, "a") == 0);
}

static void test_bom_lookalike(void) {
  collector c = {.abort_at = -1};
  parse("\xef\xbb" "data: a\n\n", &c);
  assert(c.count == 0);
}

static void test_no_data_no_event(void) {
  collector c = {.abort_at = -1};
  parse("\n\n\n", &c);
  assert(c.count == 0);
}

static void test_nul_in_id_ignored(void) {
  collector c = {.abort_at = -1};
  static const char with_nul[] = "id: 1\ndata: a\n\nid: x\0y\ndata: b\n\n";
  parse_len(with_nul, sizeof(with_nul) - 1, &c);
  assert(c.count == 2);
  assert(strcmp(c.events[0].id, "1") == 0);
  assert(strcmp(c.events[1].id, "1") == 0);
}

static void test_unknown_fields(void) {
  collector c = {.abort_at = -1};
  parse("retry: 3000\nweird: 1\ndata: a\n\n", &c);
  assert(c.count == 1);
  assert(strcmp(c.events[0].data, "a") == 0);
}

static void test_split_feeding(void) {
  const char *text =
      "event: delta\ndata: first line\ndata: second\n\nid: 9\n"
      ": comment\ndata: third\n\n";
  collector whole = {.abort_at = -1};
  parse(text, &whole);
  assert(whole.count == 2);
  for (size_t step = 1; step <= 3; step++) {
    collector c = {.abort_at = -1};
    wd_sse *sse = wd_sse_create(collect, &c);
    for (size_t i = 0; i < strlen(text); i += step) {
      size_t n = strlen(text) - i < step ? strlen(text) - i : step;
      assert(wd_sse_feed(sse, text + i, n) == 0);
    }
    assert(wd_sse_finish(sse) == 0);
    wd_sse_destroy(sse);
    assert(c.count == whole.count);
    for (size_t i = 0; i < c.count; i++) {
      assert(strcmp(c.events[i].type, whole.events[i].type) == 0);
      assert(strcmp(c.events[i].data, whole.events[i].data) == 0);
      assert(strcmp(c.events[i].id, whole.events[i].id) == 0);
    }
  }
}

static void test_oversized_line(void) {
  wd_sse *sse = wd_sse_create(NULL, NULL);
  char *big = malloc(WD_SSE_MAX_LINE + 2);
  assert(big != NULL);
  memset(big, 'a', WD_SSE_MAX_LINE + 1);
  big[WD_SSE_MAX_LINE + 1] = '\n';
  assert(wd_sse_feed(sse, big, WD_SSE_MAX_LINE + 2) == -1);
  assert(wd_sse_failed(sse));
  assert(wd_sse_feed(sse, "data: a\n\n", 9) == -1);
  free(big);
  wd_sse_destroy(sse);
}

static void test_oversized_data(void) {
  wd_sse *sse = wd_sse_create(NULL, NULL);
  char line[64];
  int failed = 0;
  for (size_t i = 0; i < (WD_SSE_MAX_DATA / 32u) + 4; i++) {
    int n = snprintf(line, sizeof(line), "data: %031zu\n", i);
    assert(n > 0);
    if (wd_sse_feed(sse, line, (size_t)n) != 0) {
      failed = 1;
      break;
    }
  }
  assert(failed);
  assert(wd_sse_failed(sse));
  wd_sse_destroy(sse);
}

static void test_emit_abort(void) {
  collector c = {.abort_at = 0};
  wd_sse *sse = wd_sse_create(collect, &c);
  assert(wd_sse_feed(sse, "data: a\n\ndata: b\n\n", 18) == -1);
  assert(wd_sse_failed(sse));
  assert(wd_sse_finish(sse) == -1);
  wd_sse_destroy(sse);
}

static void test_incomplete_discarded(void) {
  collector c = {.abort_at = -1};
  wd_sse *sse = wd_sse_create(collect, &c);
  assert(wd_sse_feed(sse, "data: a\n", 8) == 0);
  assert(wd_sse_finish(sse) == 0);
  assert(c.count == 0);
  wd_sse_destroy(sse);
}

static void test_empty_feed(void) {
  collector c = {.abort_at = -1};
  wd_sse *sse = wd_sse_create(collect, &c);
  assert(wd_sse_feed(sse, "", 0) == 0);
  assert(wd_sse_feed(sse, NULL, 0) == 0);
  assert(wd_sse_feed(sse, NULL, 1) == -1);
  wd_sse_destroy(sse);
}

int main(void) {
  test_spec_example();
  test_type_and_reset();
  test_comments();
  test_line_endings();
  test_bare_field_and_spaces();
  test_id_persists();
  test_bom();
  test_bom_lookalike();
  test_no_data_no_event();
  test_nul_in_id_ignored();
  test_unknown_fields();
  test_split_feeding();
  test_oversized_line();
  test_oversized_data();
  test_emit_abort();
  test_incomplete_discarded();
  test_empty_feed();
  printf("sse tests passed\n");
  return 0;
}
