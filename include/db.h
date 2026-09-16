/*
 * db.h - database handle, statement execution and result output.
 *
 * This module owns everything that talks to libsqlite3. Nothing above it
 * includes <sqlite3.h>; nothing in it knows about terminals or completion.
 */
#ifndef SQLSH_DB_H
#define SQLSH_DB_H

#include "out.h"

#include <stdbool.h>
#include <stdio.h>

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

/* How results are formatted. Owned by the Db and never NULL; callers set the
 * mode, headers and separators through out.h rather than through db.h,
 * because formatting is not the executor's business. */
Out *db_out(Db *db);

/* --------------------------------------------------------------------------
 * Introspection
 *
 * Everything completion needs to know about the schema and about the library
 * we are linked against. Nothing here is a hardcoded list: keywords, functions
 * and pragmas are all enumerated from libsqlite3 at run time, so they cannot
 * drift from the library actually in use.
 *
 * Each list is owned and cached by the Db, so callers never free one. A list
 * stays valid until the next call that could invalidate it -- in practice,
 * until the schema changes.
 * ------------------------------------------------------------------------ */

typedef struct DbList DbList;

size_t db_list_count(const DbList *list);
const char *db_list_name(const DbList *list, size_t i);

/* A short description of entry I: a column's declared type, "table" or
 * "view", "function", "pragma". Never NULL; "" when there is nothing to say. */
const char *db_list_detail(const DbList *list, size_t i);

/* True when a cap or the time limit cut the list short, so the caller can say
 * so rather than quietly presenting a partial list as complete. */
bool db_list_truncated(const DbList *list);

/* Tables and views, temporary ones included, sqlite_* excluded. */
const DbList *db_tables(Db *db);

/* Columns of TABLE in declaration order, with their declared types. Returns
 * an empty list when there is no such table. */
const DbList *db_columns(Db *db, const char *table);

const DbList *db_keywords(Db *db);
const DbList *db_functions(Db *db);
const DbList *db_pragmas(Db *db);

/* Distinct values of TABLE.COLUMN, for completing the right-hand side of a
 * comparison. This is the only introspection that reads user data, so it is
 * bounded twice over: at most DB_VALUE_LIMIT rows, and abandoned after
 * DB_VALUE_MS milliseconds of wall clock. Either bound sets the truncated
 * flag. A query that cannot be answered inside the limits returns what it had
 * rather than making the terminal wait. */
#define DB_VALUE_LIMIT 200u
#define DB_VALUE_MS 150

const DbList *db_values(Db *db, const char *table, const char *column);

/* The schema cookie, which changes on any DDL against the main database. The
 * caches above check it and throw themselves away when it moves. Returns 0 on
 * failure, which simply means the caches are not invalidated. */
int db_schema_version(Db *db);

#endif /* SQLSH_DB_H */
