/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/labels.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/util.h"

struct wd_labels {
  sqlite3 *db;
};

static const char schema[] =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "id TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '',"
    "archived INTEGER NOT NULL DEFAULT 0, pinned INTEGER NOT NULL DEFAULT 0,"
    "updated_ms INTEGER NOT NULL DEFAULT 0);";

wd_labels *wd_labels_open(const char *dir, char **err) {
  if (err != NULL) *err = NULL;
  char *path = wd_path_join(dir, "labels.db");
  if (path == NULL) {
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  wd_labels *labels = calloc(1, sizeof(*labels));
  if (labels == NULL) {
    free(path);
    if (err != NULL) *err = strdup("out of memory");
    return NULL;
  }
  if (sqlite3_open(path, &labels->db) != SQLITE_OK) {
    if (err != NULL)
      *err = strdup(labels->db != NULL ? sqlite3_errmsg(labels->db)
                                       : "cannot open the label database");
    sqlite3_close(labels->db);
    free(labels);
    free(path);
    return NULL;
  }
  free(path);
  (void)sqlite3_busy_timeout(labels->db, 2000);
  char *message = NULL;
  if (sqlite3_exec(labels->db, schema, NULL, NULL, &message) != SQLITE_OK) {
    if (err != NULL) *err = strdup(message != NULL ? message : "no schema");
    sqlite3_free(message);
    sqlite3_close(labels->db);
    free(labels);
    return NULL;
  }
  return labels;
}

void wd_labels_free(wd_labels *labels) {
  if (labels == NULL) return;
  sqlite3_close(labels->db);
  free(labels);
}

static int update(wd_labels *labels, const char *sql, const char *id,
                  const char *text, int flag) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(labels->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  int rc = -1;
  if (text != NULL) {
    if (sqlite3_bind_text(stmt, 1, text, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)wd_now_ms()) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE)
      rc = 0;
  } else {
    if (sqlite3_bind_int(stmt, 1, flag) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)wd_now_ms()) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE)
      rc = 0;
  }
  sqlite3_finalize(stmt);
  return rc;
}

int wd_labels_touch(wd_labels *labels, const char *id) {
  return update(labels,
                "INSERT INTO sessions(id, updated_ms) VALUES(?2, ?3) "
                "ON CONFLICT(id) DO NOTHING",
                id, NULL, 0);
}

int wd_labels_set_name(wd_labels *labels, const char *id, const char *name) {
  return update(labels,
                "INSERT INTO sessions(id, name, updated_ms) VALUES(?2, ?1, ?3) "
                "ON CONFLICT(id) DO UPDATE SET name=excluded.name, "
                "updated_ms=excluded.updated_ms",
                id, name, 0);
}

int wd_labels_set_archived(wd_labels *labels, const char *id, bool archived) {
  return update(labels,
                "INSERT INTO sessions(id, archived, updated_ms) "
                "VALUES(?2, ?1, ?3) ON CONFLICT(id) DO UPDATE SET "
                "archived=excluded.archived, updated_ms=excluded.updated_ms",
                id, NULL, archived ? 1 : 0);
}

int wd_labels_set_pinned(wd_labels *labels, const char *id, bool pinned) {
  return update(labels,
                "INSERT INTO sessions(id, pinned, updated_ms) "
                "VALUES(?2, ?1, ?3) ON CONFLICT(id) DO UPDATE SET "
                "pinned=excluded.pinned, updated_ms=excluded.updated_ms",
                id, NULL, pinned ? 1 : 0);
}

static void read_row(sqlite3_stmt *stmt, wd_label *label) {
  const unsigned char *id = sqlite3_column_text(stmt, 0);
  const unsigned char *name = sqlite3_column_text(stmt, 1);
  snprintf(label->id, sizeof(label->id), "%s", id != NULL ? (const char *)id : "");
  snprintf(label->name, sizeof(label->name), "%s",
           name != NULL ? (const char *)name : "");
  label->archived = sqlite3_column_int(stmt, 2) != 0;
  label->pinned = sqlite3_column_int(stmt, 3) != 0;
  label->updated_ms = (uint64_t)sqlite3_column_int64(stmt, 4);
}

static int collect(wd_labels *labels, const char *sql, const char *query,
                   size_t limit, wd_label **out, size_t *count, bool named) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(labels->db, sql, -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  if (named) {
    char pattern[WD_LABEL_NAME + 4];
    snprintf(pattern, sizeof(pattern), "%%%s%%", query);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)(limit == 0 ? 100 : limit));
  } else {
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)(limit == 0 ? 100 : limit));
  }
  size_t cap = 0;
  for (;;) {
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      free(*out);
      *out = NULL;
      *count = 0;
      return -1;
    }
    if (*count == cap) {
      size_t grown = cap == 0 ? 16 : cap * 2;
      wd_label *next = realloc(*out, grown * sizeof(**out));
      if (next == NULL) {
        sqlite3_finalize(stmt);
        free(*out);
        *out = NULL;
        *count = 0;
        return -1;
      }
      *out = next;
      cap = grown;
    }
    read_row(stmt, &(*out)[*count]);
    (*count)++;
  }
  sqlite3_finalize(stmt);
  return 0;
}

int wd_labels_get(wd_labels *labels, const char *id, wd_label *out) {
  memset(out, 0, sizeof(*out));
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(labels->db,
                         "SELECT id, name, archived, pinned, updated_ms FROM "
                         "sessions WHERE id = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    read_row(stmt, out);
    rc = 0;
  } else {
    rc = -1;
  }
  sqlite3_finalize(stmt);
  return rc;
}

int wd_labels_all(wd_labels *labels, wd_label **out, size_t *count) {
  return collect(labels,
                 "SELECT id, name, archived, pinned, updated_ms FROM sessions "
                 "ORDER BY updated_ms DESC LIMIT ?1",
                 NULL, 0, out, count, false);
}

int wd_labels_search(wd_labels *labels, const char *query, size_t limit,
                     wd_label **out, size_t *count) {
  if (query == NULL || query[0] == '\0') {
    *out = NULL;
    *count = 0;
    return 0;
  }
  return collect(labels,
                 "SELECT id, name, archived, pinned, updated_ms FROM sessions "
                 "WHERE name LIKE ?1 OR id LIKE ?1 ORDER BY updated_ms DESC "
                 "LIMIT ?2",
                 query, limit, out, count, true);
}
