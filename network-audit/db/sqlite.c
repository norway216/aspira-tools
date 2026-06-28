/*
 * sqlite.c — Optional SQLite3 Persistence Layer
 *
 * When HAVE_SQLITE3 is not defined, all functions compile to empty
 * stubs that return 0/-1 as appropriate.
 */

#include "sqlite.h"

#ifdef HAVE_SQLITE3
#include <sqlite3.h>

/* ---- Singleton connection ---- */
static sqlite3 *g_db = NULL;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

int
db_open(const char *path)
{
    if (g_db) return 0;                /* already open */

    int rc = sqlite3_open(path, &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    /* Create schema */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS asset ("
        "  ip        TEXT    NOT NULL,"
        "  port      INTEGER NOT NULL,"
        "  service   TEXT,"
        "  state     INTEGER,"
        "  last_seen INTEGER,"
        "  PRIMARY KEY (ip, port)"
        ");";

    char *err_msg = NULL;
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    /* Enable WAL for better concurrency */
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    return 0;
}

void
db_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/* ============================================================
 *  Store asset (INSERT OR REPLACE)
 * ============================================================ */

int
db_store_asset(const asset_row_t *row)
{
    if (!g_db) return -1;

    const char *sql =
        "INSERT OR REPLACE INTO asset (ip, port, service, state, last_seen) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite prepare error: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    sqlite3_bind_text(stmt,  1, row->ip,      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,   2, (int)row->port);
    sqlite3_bind_text(stmt,  3, row->service, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,   4, row->state);
    sqlite3_bind_int64(stmt, 5, row->last_seen);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ============================================================
 *  Find asset by IP + port
 * ============================================================ */

int
db_find_asset(const char *ip, uint16_t port, asset_row_t *row)
{
    if (!g_db || !ip || !row) return -1;

    const char *sql =
        "SELECT ip, port, service, state, last_seen FROM asset "
        "WHERE ip = ? AND port = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, ip,        -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt,  2, (int)port);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        strncpy(row->ip, (const char *)sqlite3_column_text(stmt, 0),
                sizeof(row->ip) - 1);
        row->port     = (uint16_t)sqlite3_column_int(stmt, 1);
        strncpy(row->service, (const char *)sqlite3_column_text(stmt, 2),
                sizeof(row->service) - 1);
        row->state    = sqlite3_column_int(stmt, 3);
        row->last_seen = (int64_t)sqlite3_column_int64(stmt, 4);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -1;                         /* not found */
}

/* ============================================================
 *  Count assets
 * ============================================================ */

int
db_asset_count(void)
{
    if (!g_db) return -1;

    const char *sql = "SELECT COUNT(*) FROM asset;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

#else  /* !HAVE_SQLITE3 — empty stubs */

int  db_open(const char *path)       { (void)path; return 0;  }
void db_close(void)                  {                         }
int  db_store_asset(const asset_row_t *row) { (void)row; return 0; }
int  db_find_asset(const char *ip, uint16_t port, asset_row_t *row)
                                     { (void)ip; (void)port; (void)row; return -1; }
int  db_asset_count(void)            { return 0;              }

#endif /* HAVE_SQLITE3 */
