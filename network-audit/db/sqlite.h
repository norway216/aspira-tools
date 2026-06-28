/*
 * sqlite.h — Optional SQLite3 Persistence Layer
 *
 * Compiled as empty stubs when HAVE_SQLITE3 is not defined.
 */
#ifndef DB_SQLITE_H
#define DB_SQLITE_H

#include "../include/net_audit.h"

/* ---- Lifecycle ---- */
int  db_open(const char *path);
void db_close(void);

/* ---- CRUD ---- */
int  db_store_asset(const asset_row_t *row);
int  db_find_asset(const char *ip, uint16_t port, asset_row_t *row);

/* ---- Stats ---- */
int  db_asset_count(void);

#endif /* DB_SQLITE_H */
