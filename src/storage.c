#include "storage.h"
#include <stdio.h>
int storage_open(sqlite3 **db, const char *path, long long *starts) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_open(path, db) != SQLITE_OK) goto fail;
    sqlite3_busy_timeout(*db, 250);
    if (sqlite3_exec(*db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_prepare_v2(*db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_step(stmt) != SQLITE_ROW) goto fail;
    int version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt); stmt = NULL;
    if (version > 2) { fprintf(stderr, "database schema is newer than server\n"); goto fail; }
    if (sqlite3_exec(*db,
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS server_starts ("
        "id INTEGER PRIMARY KEY, started_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, password_hash TEXT NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE IF NOT EXISTS rooms ("
        "id INTEGER PRIMARY KEY, owner_user_id INTEGER NOT NULL REFERENCES users(id),"
        "name TEXT NOT NULL, description TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'DRAFT' "
        "CHECK(status IN ('DRAFT','PUBLISHED','ACTIVE','FINISHED')),"
        "version INTEGER NOT NULL DEFAULT 0, published_at INTEGER,"
        "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));"
        "CREATE TABLE IF NOT EXISTS items ("
        "id INTEGER PRIMARY KEY, room_id INTEGER NOT NULL REFERENCES rooms(id),"
        "title TEXT NOT NULL, description TEXT NOT NULL,"
        "starting_price_vnd INTEGER NOT NULL CHECK(starting_price_vnd>=0),"
        "buy_now_price_vnd INTEGER NOT NULL CHECK(buy_now_price_vnd>starting_price_vnd),"
        "planned_start_at INTEGER NOT NULL CHECK(planned_start_at>0),"
        "duration_sec INTEGER NOT NULL CHECK(duration_sec BETWEEN 30 AND 86400),"
        "queue_position INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'QUEUED');"
        "CREATE INDEX IF NOT EXISTS items_room_order ON items(room_id,queue_position);"
        "CREATE INDEX IF NOT EXISTS rooms_owner ON rooms(owner_user_id);"
        "PRAGMA user_version=2;"
        "INSERT INTO server_starts DEFAULT VALUES;"
        "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_prepare_v2(*db, "SELECT count(*) FROM server_starts", -1, &stmt, NULL) != SQLITE_OK) goto fail;
    if (sqlite3_step(stmt) != SQLITE_ROW) goto fail;
    *starts = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
fail:
    fprintf(stderr, "storage: %s\n", *db ? sqlite3_errmsg(*db) : "open failed");
    sqlite3_finalize(stmt);
    if (*db) { sqlite3_exec(*db, "ROLLBACK", NULL, NULL, NULL); sqlite3_close(*db); *db = NULL; }
    return -1;
}
