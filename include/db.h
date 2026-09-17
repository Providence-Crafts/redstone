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

/* As db_open, but READONLY opens without SQLITE_OPEN_CREATE so that a
 * mistyped path fails instead of quietly creating an empty database. */
Db *db_open_mode(const char *path, bool readonly, FILE *err);

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
const char *db_sqlite_sourceid(void);

/* How results are formatted. Owned by the Db and never NULL; callers set the
 * mode, headers and separators through out.h rather than through db.h,
 * because formatting is not the executor's business. */
Out *db_out(Db *db);

/* Move the formatter from SRC to DST. `.connection` needs it: how results
 * look is a property of the session, not of which connection is current. */
void db_take_out(Db *dst, Db *src);

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

/* --------------------------------------------------------------------------
 * A cursor over one statement
 *
 * Phase 6's schema.c and import.c need to read rows without formatting them,
 * and without including <sqlite3.h>. This is the whole of what they get: a
 * prepared statement they can step and read as text, integers or bytes.
 * ------------------------------------------------------------------------ */

typedef struct DbStmt DbStmt;

/* Prepare SQL. Returns NULL after writing a message to ERR, which may be
 * NULL to stay silent. */
DbStmt *db_prepare(Db *db, const char *sql, FILE *err);

/* Bind the 1-based parameter I to TEXT (copied) or to NULL. */
bool db_bind_text(DbStmt *stmt, int i, const char *text);

/* True while a row is available. Stops at the first error; db_finalize
 * reports it. */
bool db_step(DbStmt *stmt);

int db_stmt_columns(const DbStmt *stmt);
const char *db_stmt_name(DbStmt *stmt, int i);

/* The value of column I of the current row. TEXT never returns NULL: use
 * db_stmt_is_null to tell an empty string from a NULL. */
const char *db_stmt_text(DbStmt *stmt, int i);
long long db_stmt_int(DbStmt *stmt, int i);
double db_stmt_real(DbStmt *stmt, int i);
const void *db_stmt_blob(DbStmt *stmt, int i, size_t *nbyte);
bool db_stmt_is_null(DbStmt *stmt, int i);
/* OUT_* from out.h, so a caller can quote a value the way its type needs. */
OutType db_stmt_type(DbStmt *stmt, int i);

/* Report the step just made, then put STMT back to its pre-bound state so the
 * same statement can be bound and stepped again. Returns false, after writing
 * the error to ERR (which may be NULL), when that step failed. Preparing once
 * and resetting per row is what keeps a large .import from re-compiling its
 * INSERT for every line of the file. Safe on NULL. */
bool db_stmt_reset(DbStmt *stmt, FILE *err);

/* Finalize and free. Returns false if the statement ended in an error, after
 * writing it to ERR (which may be NULL). Safe on NULL. */
bool db_finalize(DbStmt *stmt, FILE *err);

/* Run SQL for its effect, discarding any rows. Returns false after writing
 * the error to ERR. */
bool db_run(Db *db, const char *sql, FILE *err);

/* The first column of the first row of SQL, or NULL if there is none. The
 * result is owned by the caller. */
char *db_scalar(Db *db, const char *sql);

/* --------------------------------------------------------------------------
 * Connection control
 *
 * The dot commands that configure the connection rather than query it. Each
 * is a narrow accessor rather than a leaked sqlite3*, so the rule that only
 * db.c includes <sqlite3.h> survives the phase.
 * ------------------------------------------------------------------------ */

/* The last error text from this connection. Never NULL. */
const char *db_errmsg(Db *db);

int db_changes(Db *db);

/* False while a transaction is open on this connection. `.import` asks before
 * wrapping its load in one, so that it neither nests a transaction nor commits
 * one the user began. */
bool db_autocommit(Db *db);

/* `.stats`: the memory and lookaside counters, in sqlite3(1)'s layout. Here
 * because every one of them comes from sqlite3_status64 and
 * sqlite3_db_status. */
void db_print_stats(Db *db, FILE *out, bool reset);
long long db_total_changes(Db *db);

/* Reopen on PATH, or in memory when PATH is NULL. On failure the previous
 * connection is left untouched and false is returned, so `.open /nope` does
 * not cost the user their session. */
bool db_reopen(Db *db, const char *path, bool readonly, bool create, FILE *err);

/* True when the main database was opened read-only. */
bool db_is_readonly(Db *db);

/* `.backup` and `.restore`. Copies the named attached database (NULL for
 * "main") to or from FILE. */
bool db_backup_to(Db *db, const char *dbname, const char *file, FILE *err);
bool db_restore_from(Db *db, const char *dbname, const char *file, FILE *err);

/* `.limit`. NAME is a limit name without the SQLITE_LIMIT_ prefix. With
 * VALUE < 0 the limit is only read. Returns false for an unknown name. */
bool db_limit(Db *db, const char *name, int value, int *current);

/* Enumerate the limit names, NULL past the end. */
const char *db_limit_name(size_t i);

/* `.dbconfig`. NAME is a config name without the SQLITE_DBCONFIG_ prefix.
 * VALUE is 0, 1, or -1 to query. Returns false for an unknown name. */
bool db_dbconfig(Db *db, const char *name, int value, int *current);
const char *db_dbconfig_name(size_t i);

/* `.load`. ENTRY may be NULL for the default entry point. */
bool db_load_extension(Db *db, const char *file, const char *entry, FILE *err);

/* `.timeout`. */
void db_busy_timeout(Db *db, int ms);

/* `.auth`, `.progress`, `.trace`, `.changes`: switches whose implementation
 * has to touch the handle. TRACE_FLAGS is a bitwise OR of DB_TRACE_*. */
#define DB_TRACE_STMT 0x01u
#define DB_TRACE_PROFILE 0x02u
#define DB_TRACE_ROW 0x04u
#define DB_TRACE_CLOSE 0x08u

void db_set_authorizer(Db *db, bool on, FILE *stream);
void db_set_progress(Db *db, int nops, bool quiet, FILE *stream);
void db_set_trace(Db *db, unsigned flags, FILE *stream);

/* `.filectrl`. Returns false for an unknown opcode name; otherwise writes a
 * human-readable result to OUT. */
bool db_file_control(Db *db, const char *dbname, const char *op, const char *arg, FILE *out,
                     FILE *err);
const char *db_file_control_name(size_t i);

/* `.vfslist`, `.vfsname`, `.vfsinfo`. VFS I by registration order, NULL past
 * the end. db_vfs_current names the VFS backing the named database. */
const char *db_vfs_at(size_t i);
const char *db_vfs_current(Db *db, const char *dbname);

/* `.parameter`. Bound parameters live in a temp table, as upstream's do, so
 * they survive a reprepare and can be listed with SQL. */
bool db_parameter_set(Db *db, const char *key, const char *value, FILE *err);
bool db_parameter_unset(Db *db, const char *key, FILE *err);
bool db_parameter_clear(Db *db, FILE *err);

/* The open file path of the named attached database, or NULL when it has
 * none (a temporary or in-memory database). */
const char *db_filename(Db *db, const char *dbname);

/* Narrow per-schema queries that `.databases` and `.dbinfo` need. TXN state
 * is 0 none, 1 read, 2 write. */
bool db_schema_readonly(Db *db, const char *dbname);
int db_txn_state(Db *db, const char *dbname);
bool db_data_version(Db *db, const char *dbname, unsigned *value);

/* True when NAME is an SQL keyword in the library we are linked against, so
 * that schema output quotes an identifier that would otherwise reparse as
 * syntax. */
bool db_is_keyword(const char *name);

/* The declared collating sequence of TABLE.COLUMN in "main", copied into BUF.
 * False when the column does not exist. `.lint fkey-indexes` needs it to
 * decide whether a suggested index has to name a collation. */
bool db_column_collation(Db *db, const char *table, const char *column, char *buf, size_t size);

/* SQLite's own GLOB, on plain strings. Used to recognise the query plan that
 * says a foreign key already has a usable index. */
bool db_glob(const char *pattern, const char *text);

#endif /* SQLSH_DB_H */
