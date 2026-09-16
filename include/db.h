/*
 * db.h - database handle, statement execution and result output.
 *
 * This module owns everything that talks to libsqlite3. Nothing above it
 * includes <sqlite3.h>; nothing in it knows about terminals or completion.
 */
#ifndef SQLSH_DB_H
#define SQLSH_DB_H

#include <stdbool.h>
#include <stdio.h>

/* Output formats for query results. Mirrors the useful subset of sqlite3's
 * .mode, without the formats nobody reads interactively. */
typedef enum {
    DB_MODE_COLUMN = 0, /* aligned columns, the interactive default */
    DB_MODE_LIST,       /* values separated by DB_SEPARATOR */
    DB_MODE_CSV,
    DB_MODE_JSON,
    DB_MODE_LINE /* one "column = value" per line */
} DbMode;

typedef struct Db Db;

/* Open PATH, or an in-memory database when PATH is NULL.
 * Returns NULL on failure, after writing a message to ERR. */
Db *db_open(const char *path, FILE *err);

/* Close and free. Safe on NULL. */
void db_close(Db *db);

/* Execute the (possibly multi-statement) SQL in TEXT, writing results to OUT
 * and any error to ERR. Returns false if any statement failed; execution stops
 * at the first failure. */
bool db_exec(Db *db, const char *text, FILE *out, FILE *err);

/* True when TEXT forms one or more complete statements, i.e. the shell should
 * execute rather than ask for a continuation line. */
bool db_is_complete(const char *text);

/* The path this database was opened from, or ":memory:". Never NULL. */
const char *db_path(const Db *db);

/* Version string of the libsqlite3 we are linked against. Exposed here so
 * that no module above db.c needs to include <sqlite3.h>. */
const char *db_sqlite_version(void);

void db_set_mode(Db *db, DbMode mode);
DbMode db_mode(const Db *db);

void db_set_headers(Db *db, bool on);
bool db_headers(const Db *db);

/* Separator used by DB_MODE_LIST. Copied; defaults to "|". */
void db_set_separator(Db *db, const char *sep);

/* Text printed for SQL NULL. Copied; defaults to "" in column mode. */
void db_set_null_text(Db *db, const char *text);
const char *db_null_text(const Db *db);

#endif /* SQLSH_DB_H */
