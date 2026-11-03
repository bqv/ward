/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/search.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

#define WD_SEARCH_LIMIT_MAX 200

struct wd_search {
  sqlite3 *db;
};

static const char schema[] =
    "CREATE VIRTUAL TABLE IF NOT EXISTS transcript USING fts5("
    "text, session UNINDEXED, seq UNINDEXED, role UNINDEXED, ts UNINDEXED);";

wd_search *wd_search_open(const char *dir, char **err) {
  if (err != NULL) *err = NULL;
  char *path = wd_path_join(dir, "search.db");
  if (path == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  wd_search *search = calloc(1, sizeof(*search));
  if (search == NULL) {
    free(path);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  if (sqlite3_open(path, &search->db) != SQLITE_OK ||
      sqlite3_exec(search->db, schema, NULL, NULL, NULL) != SQLITE_OK) {
    if (err != NULL)
      *err = strdup(search->db != NULL ? sqlite3_errmsg(search->db)
                                       : "cannot open the search database");
    sqlite3_close(search->db);
    free(search);
    free(path);
    return NULL;
  }
  free(path);
  sqlite3_busy_timeout(search->db, 2000);
  return search;
}

void wd_search_free(wd_search *search) {
  if (search == NULL) return;
  sqlite3_close(search->db);
  free(search);
}

int wd_search_index(wd_search *search, const char *session, uint64_t seq,
                    const char *role, const char *ts, const char *text) {
  if (search == NULL || text == NULL || text[0] == '\0') return 0;
  static const char insert[] =
      "INSERT INTO transcript (text, session, seq, role, ts)"
      " VALUES (?1, ?2, ?3, ?4, ?5);";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(search->db, insert, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, session != NULL ? session : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)seq);
  sqlite3_bind_text(stmt, 4, role != NULL ? role : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, ts != NULL ? ts : "", -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? 0 : -1;
}

static void copy_field(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src != NULL ? src : "");
}

size_t wd_search_query(wd_search *search, const char *query,
                       const char *session, size_t limit, uint64_t before,
                       wd_search_hit *hits, size_t cap, uint64_t *next,
                       uint64_t *total) {
  if (next != NULL) *next = 0;
  if (total != NULL) *total = 0;
  if (search == NULL || query == NULL || query[0] == '\0' || hits == NULL)
    return 0;
  if (limit == 0) limit = 50;
  if (limit > WD_SEARCH_LIMIT_MAX) limit = WD_SEARCH_LIMIT_MAX;
  if (limit > cap) limit = cap;

  static const char count_sql[] =
      "SELECT COUNT(*) FROM transcript WHERE transcript MATCH ?1"
      " AND (?2 = '' OR session = ?2) AND (?3 = 0 OR seq < ?3);";
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(search->db, count_sql, -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session != NULL ? session : "", -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)before);
    if (sqlite3_step(stmt) == SQLITE_ROW && total != NULL)
      *total = (uint64_t)sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);

  static const char select_sql[] =
      "SELECT session, seq, role, ts, text,"
      " snippet(transcript, 0, '', '', '...', 12), bm25(transcript)"
      " FROM transcript WHERE transcript MATCH ?1"
      " AND (?2 = '' OR session = ?2) AND (?3 = 0 OR seq < ?3)"
      " ORDER BY seq DESC LIMIT ?4;";
  if (sqlite3_prepare_v2(search->db, select_sql, -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, session != NULL ? session : "", -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)before);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)limit);
  size_t count = 0;
  while (count < limit && sqlite3_step(stmt) == SQLITE_ROW) {
    wd_search_hit *hit = &hits[count];
    memset(hit, 0, sizeof(*hit));
    copy_field(hit->session, sizeof(hit->session),
               (const char *)sqlite3_column_text(stmt, 0));
    hit->seq = (uint64_t)sqlite3_column_int64(stmt, 1);
    copy_field(hit->role, sizeof(hit->role),
               (const char *)sqlite3_column_text(stmt, 2));
    copy_field(hit->ts, sizeof(hit->ts),
               (const char *)sqlite3_column_text(stmt, 3));
    const char *text = (const char *)sqlite3_column_text(stmt, 4);
    const char *snippet = (const char *)sqlite3_column_text(stmt, 5);
    hit->text = strdup(text != NULL ? text : "");
    hit->snippet = strdup(snippet != NULL ? snippet : "");
    hit->score = sqlite3_column_double(stmt, 6);
    count++;
  }
  sqlite3_finalize(stmt);
  if (count > 0 && next != NULL) *next = hits[count - 1].seq;
  return count;
}

int wd_search_rows(wd_search *search, size_t *out) {
  if (search == NULL || out == NULL) return -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(search->db, "SELECT COUNT(*) FROM transcript;", -1,
                         &stmt, NULL) != SQLITE_OK)
    return -1;
  int rc = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    *out = (size_t)sqlite3_column_int64(stmt, 0);
    rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

void wd_search_hits_free(wd_search_hit *hits, size_t count) {
  if (hits == NULL) return;
  for (size_t i = 0; i < count; i++) {
    free(hits[i].text);
    free(hits[i].snippet);
  }
}
