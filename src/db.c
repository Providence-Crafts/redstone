#include "db.h"

#include "out.h"
#include "plat.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* An upper bound on the columns one statement may produce. sqlite's own
 * limit is lower, but a bound here keeps the per-row value array on the
 * stack. */
#define DB_MAX_COLUMNS 256

/* How many per-table and per-(table, column) lists to remember. Both are ring
 * buffers rather than anything cleverer: completion revisits the same handful
 * of tables over and over, and a ring gets that right in twenty lines. */
#define DB_COLUMN_CACHE 16u
#define DB_VALUE_CACHE 8u

struct DbList {
    char **name;
    char **detail;
    size_t n;
    size_t cap;
    bool truncated;
};

/* One slot of a name-keyed cache. KEY2 is NULL for the column cache and the
 * column name for the value cache. */
typedef struct {
    char *key;
    char *key2;
    DbList *list;
} CacheSlot;

struct Db {
    sqlite3 *handle;
    char *path;
    Out *out; /* how results are formatted; see out.h */

    /* Introspection caches. The schema-derived ones are dropped whenever the
     * cookie moves; keywords, functions and pragmas are properties of the
     * library and never change under us. */
    int schema_cookie;
    bool schema_known;
    DbList *tables;
    DbList *keywords;
    DbList *functions;
    DbList *pragmas;
    CacheSlot columns[DB_COLUMN_CACHE];
    size_t columns_next;
    CacheSlot values[DB_VALUE_CACHE];
    size_t values_next;

    /* Hooks that print. Each keeps the stream it was installed with, because
     * the shell can redirect output between `.trace on` and the statement
     * that fires it. */
    FILE *auth_stream;
    FILE *progress_stream;
    FILE *trace_stream;
    unsigned progress_count;
};

/* strdup is not C99, and the project builds with -std=c99 strictly. */
static char *dup_str(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1u;
    copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

Db *db_open(const char *path, FILE *err)
{
    return db_open_mode(path, false, err);
}

/* The connection settings sqlite3(1) applies the moment it opens a database.
 * They are behaviour, not decoration: with DQS off a double-quoted unknown
 * identifier is an error rather than a string, and with defensive on the
 * schema cannot be rewritten from SQL. A drop-in replacement has to start
 * from the same place, so this runs on the first open and on every .open.
 *
 * DQS is the one that is set here but not in upstream's shell.c: upstream
 * compiles its own copy of SQLite with -DSQLITE_DQS=0, while redstone links a
 * shared library that may have been built either way. Setting it explicitly
 * makes the behaviour the same whichever library is underneath. */
static void configure(sqlite3 *handle)
{
    int ignored = 0;

    (void)sqlite3_db_config(handle, SQLITE_DBCONFIG_DQS_DDL, 0, &ignored);
    (void)sqlite3_db_config(handle, SQLITE_DBCONFIG_DQS_DML, 0, &ignored);
    (void)sqlite3_db_config(handle, SQLITE_DBCONFIG_STMT_SCANSTATUS, 0, &ignored);
    (void)sqlite3_db_config(handle, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &ignored);
    (void)sqlite3_db_config(handle, SQLITE_DBCONFIG_DEFENSIVE, 1, &ignored);
    /* Loading is enabled but nothing is loaded: .load is still the only way
     * in, and -safe refuses it. */
    (void)sqlite3_enable_load_extension(handle, 1);
}

Db *db_open_mode(const char *path, bool readonly, FILE *err)
{
    Db *db;
    int rc;

    db = calloc(1u, sizeof(*db));
    if (db == NULL) {
        fprintf(err, "redstone: out of memory\n");
        return NULL;
    }

    db->out = out_new(stdout);
    db->path = dup_str(path != NULL ? path : ":memory:");

    if (db->out == NULL || db->path == NULL) {
        fprintf(err, "redstone: out of memory\n");
        db_close(db);
        return NULL;
    }

    rc = sqlite3_open_v2(
        db->path, &db->handle,
        readonly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE), NULL);
    if (rc != SQLITE_OK) {
        /* sqlite3_open allocates a handle even on failure, so the error text
         * is available and db_close still has something to free. */
        fprintf(err, "redstone: cannot open %s: %s\n", db->path,
                db->handle != NULL ? sqlite3_errmsg(db->handle) : sqlite3_errstr(rc));
        db_close(db);
        return NULL;
    }

    configure(db->handle);
    return db;
}

/* Defined with the introspection code at the end of the file. */
static void cache_dispose(Db *db);

void db_close(Db *db)
{
    if (db == NULL) {
        return;
    }
    cache_dispose(db);
    if (db->handle != NULL) {
        sqlite3_close(db->handle);
    }
    out_free(db->out);
    free(db->path);
    free(db);
}

const char *db_path(const Db *db)
{
    return db->path;
}

const char *db_sqlite_version(void)
{
    return sqlite3_libversion();
}

const char *db_sqlite_sourceid(void)
{
    return sqlite3_sourceid();
}

/* cppcheck-suppress constParameterPointer ; the Db is not const to the
 * caller: it hands out a formatter the caller is expected to reconfigure. */
Out *db_out(Db *db)
{
    return db->out;
}

void db_take_out(Db *dst, Db *src)
{
    Out *moved = src->out;

    if (dst == src) {
        return;
    }
    src->out = dst->out;
    dst->out = moved;
}

bool db_is_complete(const char *text)
{
    /* sqlite3_complete wants a NUL-terminated statement and understands
     * strings, comments and BEGIN...END blocks, which is exactly the rule the
     * REPL needs for deciding whether to keep reading. */
    return sqlite3_complete(text) != 0;
}

/* --------------------------------------------------------------------------
 * Feeding the formatter
 *
 * db.c decides nothing about appearance: it turns a stepped row into OutValue
 * records and hands them over. That is what lets tests/parity.sh treat
 * formatting as one testable module rather than a property of the executor.
 * ------------------------------------------------------------------------ */

static OutType type_of(int sqlite_type)
{
    switch (sqlite_type) {
    case SQLITE_NULL:
        return OUT_NULL;
    case SQLITE_INTEGER:
        return OUT_INT;
    case SQLITE_FLOAT:
        return OUT_REAL;
    case SQLITE_BLOB:
        return OUT_BLOB;
    case SQLITE_TEXT:
    default:
        return OUT_TEXT;
    }
}

static bool print_result(Db *db, sqlite3_stmt *stmt, int ncol)
{
    /* Zeroed so a zero-column result hands out_begin a defined array. */
    const char *name[DB_MAX_COLUMNS] = {NULL};
    OutValue value[DB_MAX_COLUMNS];
    int i;
    int rc;
    bool ok;

    if (ncol > DB_MAX_COLUMNS) {
        ncol = DB_MAX_COLUMNS;
    }
    for (i = 0; i < ncol; i++) {
        const char *n = sqlite3_column_name(stmt, i);

        name[i] = n != NULL ? n : "";
    }
    if (!out_begin(db->out, ncol, name)) {
        return false;
    }
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (i = 0; i < ncol; i++) {
            value[i].type = type_of(sqlite3_column_type(stmt, i));
            value[i].blob = NULL;
            value[i].nbyte = 0u;
            if (value[i].type == OUT_BLOB) {
                value[i].text = NULL;
                value[i].blob = (const unsigned char *)sqlite3_column_blob(stmt, i);
                value[i].nbyte = (size_t)sqlite3_column_bytes(stmt, i);
            } else {
                const unsigned char *text = sqlite3_column_text(stmt, i);

                value[i].text = text != NULL ? (const char *)text : "";
                value[i].nbyte = strlen(value[i].text);
            }
        }
        if (!out_row(db->out, value)) {
            (void)out_end(db->out);
            return false;
        }
    }
    ok = out_end(db->out);
    return ok && rc == SQLITE_DONE;
}

/* Bind any named parameters from TEMP.sqlite_parameters, as upstream does,
 * so that `.parameter set $id 7` reaches the next statement that mentions
 * $id. A parameter with no stored value is left NULL, which is also what
 * sqlite would do; the point is only that a stored one is not ignored. */
static void bind_parameters(Db *db, sqlite3_stmt *stmt)
{
    sqlite3_stmt *lookup = NULL;
    int n = sqlite3_bind_parameter_count(stmt);
    int i;

    if (n == 0) {
        return;
    }
    if (sqlite3_prepare_v2(db->handle, "SELECT value FROM temp.sqlite_parameters WHERE key=?1", -1,
                           &lookup, NULL) != SQLITE_OK) {
        sqlite3_finalize(lookup);
        return; /* no parameter table: nothing to bind */
    }
    for (i = 1; i <= n; i++) {
        const char *name = sqlite3_bind_parameter_name(stmt, i);

        if (name == NULL) {
            continue;
        }
        sqlite3_reset(lookup);
        sqlite3_bind_text(lookup, 1, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(lookup) == SQLITE_ROW) {
            sqlite3_bind_value(stmt, i, sqlite3_column_value(lookup, 0));
        }
    }
    sqlite3_finalize(lookup);
}

bool db_exec(Db *db, const char *text, FILE *out, FILE *err)
{
    const char *tail = text;

    out_set_stream(db->out, out);
    while (tail != NULL && *tail != '\0') {
        sqlite3_stmt *stmt = NULL;
        const char *next = NULL;
        int ncol;
        bool ok;

        if (sqlite3_prepare_v2(db->handle, tail, -1, &stmt, &next) != SQLITE_OK) {
            fprintf(err, "redstone: %s\n", sqlite3_errmsg(db->handle));
            sqlite3_finalize(stmt);
            return false;
        }
        if (stmt == NULL) {
            /* Whitespace or a comment: nothing to run, move on. */
            tail = next;
            continue;
        }

        bind_parameters(db, stmt);
        ncol = sqlite3_column_count(stmt);
        if (ncol == 0) {
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        } else {
            ok = print_result(db, stmt, ncol);
        }

        if (sqlite3_finalize(stmt) != SQLITE_OK || !ok) {
            fprintf(err, "redstone: %s\n", sqlite3_errmsg(db->handle));
            return false;
        }

        /* Individual fputc/fprintf returns are deliberately not checked; a
         * stream error is sticky, so one check per statement catches it
         * without burying the formatting code in error handling. The flush is
         * part of the check, not a convenience: on a buffered stream the
         * write syscall that fails (ENOSPC, EPIPE) may not have happened yet,
         * so ferror alone would report success and the caller would exit 0
         * having written nothing. */
        if (fflush(out) != 0 || ferror(out)) {
            fprintf(err, "redstone: write failed\n");
            clearerr(out);
            return false;
        }

        tail = next;
    }

    return true;
}

/* --------------------------------------------------------------------------
 * Introspection
 * ------------------------------------------------------------------------ */

static DbList *list_new(void)
{
    return calloc(1u, sizeof(DbList));
}

static void list_free(DbList *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0u; i < list->n; i++) {
        free(list->name[i]);
        free(list->detail[i]);
    }
    free((void *)list->name);
    free((void *)list->detail);
    free(list);
}

static bool list_grow(DbList *list)
{
    size_t cap = (list->cap == 0u) ? 32u : list->cap * 2u;
    char **names = (char **)realloc((void *)list->name, cap * sizeof(*names));
    char **details;

    if (names == NULL) {
        return false;
    }
    list->name = names;
    details = (char **)realloc((void *)list->detail, cap * sizeof(*details));
    if (details == NULL) {
        return false;
    }
    list->detail = details;
    list->cap = cap;
    return true;
}

static bool list_add(DbList *list, const char *name, const char *detail)
{
    char *dup_name;
    char *dup_detail;

    if (name == NULL) {
        return true; /* a NULL value is nothing to complete to */
    }
    if (list->n == list->cap && !list_grow(list)) {
        return false;
    }
    dup_name = dup_str(name);
    dup_detail = dup_str(detail != NULL ? detail : "");
    if (dup_name == NULL || dup_detail == NULL) {
        free(dup_name);
        free(dup_detail);
        return false;
    }
    list->name[list->n] = dup_name;
    list->detail[list->n] = dup_detail;
    list->n++;
    return true;
}

size_t db_list_count(const DbList *list)
{
    return list != NULL ? list->n : 0u;
}

const char *db_list_name(const DbList *list, size_t i)
{
    if (list == NULL || i >= list->n) {
        return NULL;
    }
    return list->name[i];
}

const char *db_list_detail(const DbList *list, size_t i)
{
    if (list == NULL || i >= list->n) {
        return "";
    }
    return list->detail[i];
}

bool db_list_truncated(const DbList *list)
{
    return list != NULL && list->truncated;
}

/* Run SQL, taking column 0 as the name and column 1, when present, as the
 * detail. BIND, when not NULL, is bound to parameter 1. A query the library
 * cannot even prepare yields an empty list rather than an error: a build
 * without the introspection pragmas should lose those candidates, not fail. */
static DbList *list_query(sqlite3 *handle, const char *sql, const char *bind)
{
    DbList *list = list_new();
    sqlite3_stmt *stmt = NULL;

    if (list == NULL) {
        return NULL;
    }
    if (sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return list;
    }
    if (bind != NULL) {
        (void)sqlite3_bind_text(stmt, 1, bind, -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *detail = NULL;

        if (sqlite3_column_count(stmt) > 1) {
            detail = (const char *)sqlite3_column_text(stmt, 1);
        }
        if (!list_add(list, name, detail)) {
            list->truncated = true;
            break;
        }
    }
    (void)sqlite3_finalize(stmt);
    return list;
}

static void slot_clear(CacheSlot *slot)
{
    free(slot->key);
    free(slot->key2);
    list_free(slot->list);
    slot->key = NULL;
    slot->key2 = NULL;
    slot->list = NULL;
}

/* Find a slot by one or two keys. KEY2 is matched only when non-NULL. */
static const DbList *slot_find(const CacheSlot *slots, size_t n, const char *key, const char *key2)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (slots[i].key == NULL || strcmp(slots[i].key, key) != 0) {
            continue;
        }
        if (key2 == NULL) {
            return slots[i].list;
        }
        if (slots[i].key2 != NULL && strcmp(slots[i].key2, key2) == 0) {
            return slots[i].list;
        }
    }
    return NULL;
}

/* Install LIST at the next slot of a ring, taking ownership of it. Returns the
 * stored list, or NULL if it could not be stored -- in which case LIST has
 * been freed and the caller has nothing to return. */
static const DbList *slot_store(CacheSlot *slots, size_t n, size_t *next, const char *key,
                                const char *key2, DbList *list)
{
    CacheSlot *slot = &slots[*next % n];

    if (list == NULL) {
        return NULL;
    }
    slot_clear(slot);
    slot->key = dup_str(key);
    slot->key2 = dup_str(key2);
    if (slot->key == NULL || (key2 != NULL && slot->key2 == NULL)) {
        slot_clear(slot);
        list_free(list);
        return NULL;
    }
    slot->list = list;
    *next = (*next + 1u) % n;
    return list;
}

int db_schema_version(Db *db)
{
    sqlite3_stmt *stmt = NULL;
    int version = 0;

    if (db == NULL || db->handle == NULL) {
        return 0;
    }
    if (sqlite3_prepare_v2(db->handle, "PRAGMA schema_version", -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    (void)sqlite3_finalize(stmt);
    return version;
}

static void cache_drop_schema(Db *db)
{
    size_t i;

    list_free(db->tables);
    db->tables = NULL;
    for (i = 0u; i < DB_COLUMN_CACHE; i++) {
        slot_clear(&db->columns[i]);
    }
    for (i = 0u; i < DB_VALUE_CACHE; i++) {
        slot_clear(&db->values[i]);
    }
    db->columns_next = 0u;
    db->values_next = 0u;
}

static void cache_dispose(Db *db)
{
    cache_drop_schema(db);
    list_free(db->keywords);
    list_free(db->functions);
    list_free(db->pragmas);
    db->keywords = NULL;
    db->functions = NULL;
    db->pragmas = NULL;
}

/* Called at the top of every schema-derived lookup. Any DDL moves the cookie,
 * so a completion taken straight after an ALTER TABLE sees the new column. */
static void cache_check(Db *db)
{
    int cookie = db_schema_version(db);

    if (!db->schema_known) {
        db->schema_known = true;
        db->schema_cookie = cookie;
        return;
    }
    if (cookie != db->schema_cookie) {
        db->schema_cookie = cookie;
        cache_drop_schema(db);
    }
}

const DbList *db_tables(Db *db)
{
    if (db == NULL || db->handle == NULL) {
        return NULL;
    }
    cache_check(db);
    if (db->tables == NULL) {
        /* sqlite_temp_master is unioned in so that a temporary table is
         * completable the moment it exists. The four schema tables are not
         * rows of any schema -- sqlite_master does not describe itself -- yet
         * they are the first thing anyone querying a database's structure
         * types, so they are listed by hand. */
        db->tables = list_query(db->handle,
                                "SELECT name, type FROM sqlite_master"
                                "  WHERE type IN ('table','view')"
                                " UNION ALL"
                                " SELECT name, type FROM sqlite_temp_master"
                                "  WHERE type IN ('table','view')"
                                " UNION ALL"
                                " SELECT column1, 'table' FROM (VALUES"
                                "  ('sqlite_master'), ('sqlite_schema'),"
                                "  ('sqlite_temp_master'), ('sqlite_temp_schema'))"
                                " ORDER BY 1",
                                NULL);
    }
    return db->tables;
}

const DbList *db_columns(Db *db, const char *table)
{
    const DbList *cached;
    DbList *list;

    if (db == NULL || db->handle == NULL || table == NULL) {
        return NULL;
    }
    cache_check(db);
    cached = slot_find(db->columns, DB_COLUMN_CACHE, table, NULL);
    if (cached != NULL) {
        return cached;
    }
    /* pragma_table_info rather than table_xinfo: hidden columns of a virtual
     * table are not something the user can select by name. */
    list = list_query(db->handle, "SELECT name, type FROM pragma_table_info(?1)", table);
    return slot_store(db->columns, DB_COLUMN_CACHE, &db->columns_next, table, NULL, list);
}

const DbList *db_keywords(Db *db)
{
    int count;
    int i;

    if (db == NULL) {
        return NULL;
    }
    if (db->keywords != NULL) {
        return db->keywords;
    }
    db->keywords = list_new();
    if (db->keywords == NULL) {
        return NULL;
    }
    count = sqlite3_keyword_count();
    for (i = 0; i < count; i++) {
        const char *name = NULL;
        int len = 0;
        char buf[64];

        if (sqlite3_keyword_name(i, &name, &len) != SQLITE_OK || name == NULL) {
            continue;
        }
        if (len <= 0 || (size_t)len >= sizeof(buf)) {
            continue;
        }
        memcpy(buf, name, (size_t)len);
        buf[len] = '\0';
        if (!list_add(db->keywords, buf, "keyword")) {
            db->keywords->truncated = true;
            break;
        }
    }
    return db->keywords;
}

const DbList *db_functions(Db *db)
{
    if (db == NULL || db->handle == NULL) {
        return NULL;
    }
    if (db->functions == NULL) {
        /* DISTINCT because overloads differing only in arity are one name to
         * the person typing. */
        db->functions = list_query(
            db->handle, "SELECT DISTINCT name, 'function' FROM pragma_function_list ORDER BY 1",
            NULL);
    }
    return db->functions;
}

const DbList *db_pragmas(Db *db)
{
    if (db == NULL || db->handle == NULL) {
        return NULL;
    }
    if (db->pragmas == NULL) {
        db->pragmas = list_query(db->handle,
                                 "SELECT name, 'pragma' FROM pragma_pragma_list ORDER BY 1", NULL);
    }
    return db->pragmas;
}

/* --- value completion --------------------------------------------------- */

typedef struct {
    long long start_ms;
    long long limit_ms;
} Deadline;

static long long elapsed_ms(long long start_ms)
{
    long long now;

    return plat_monotonic_ms(&now) ? now - start_ms : 0;
}

/* Returning non-zero aborts the statement. This is the only thing standing
 * between a Tab press and a full scan of a very large table. The argument
 * cannot be const: sqlite3_progress_handler dictates the signature. */
/* cppcheck-suppress constParameterCallback */
static int value_progress(void *arg)
{
    const Deadline *dl = (const Deadline *)arg;

    return elapsed_ms(dl->start_ms) > dl->limit_ms ? 1 : 0;
}

static DbList *value_query(sqlite3 *handle, const char *table, const char *column)
{
    DbList *list = list_new();
    sqlite3_stmt *stmt = NULL;
    Deadline dl;
    char *sql;
    int rc;

    if (list == NULL) {
        return NULL;
    }
    /* %w doubles any embedded quote, which is what makes a column literally
     * named `order` or `total amount` safe to interpolate here. */
    sql = sqlite3_mprintf("SELECT DISTINCT \"%w\" FROM \"%w\""
                          " WHERE \"%w\" IS NOT NULL ORDER BY 1 LIMIT %d",
                          column, table, column, (int)DB_VALUE_LIMIT + 1);
    if (sql == NULL) {
        return list;
    }
    rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
        return list; /* no such table or column: nothing to offer */
    }

    dl.limit_ms = DB_VALUE_MS;
    if (!plat_monotonic_ms(&dl.start_ms)) {
        dl.start_ms = 0;
        dl.limit_ms = -1; /* no usable clock: refuse rather than run unbounded */
    }
    sqlite3_progress_handler(handle, 200, value_progress, &dl);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (list->n >= DB_VALUE_LIMIT) {
            list->truncated = true;
            break;
        }
        if (!list_add(list, (const char *)sqlite3_column_text(stmt, 0), "value")) {
            list->truncated = true;
            break;
        }
    }
    sqlite3_progress_handler(handle, 0, NULL, NULL);
    if (sqlite3_finalize(stmt) == SQLITE_INTERRUPT) {
        list->truncated = true; /* the deadline cut it off */
    }
    return list;
}

const DbList *db_values(Db *db, const char *table, const char *column)
{
    const DbList *cached;
    DbList *list;

    if (db == NULL || db->handle == NULL || table == NULL || column == NULL) {
        return NULL;
    }
    cache_check(db);
    cached = slot_find(db->values, DB_VALUE_CACHE, table, column);
    if (cached != NULL) {
        return cached;
    }
    list = value_query(db->handle, table, column);
    return slot_store(db->values, DB_VALUE_CACHE, &db->values_next, table, column, list);
}

/* --------------------------------------------------------------------------
 * A cursor over one statement
 *
 * schema.c and import.c read rows without formatting them. They get this and
 * nothing else: the rule that only db.c includes <sqlite3.h> is what keeps
 * `--compat` a property of one module rather than of the whole program.
 * ------------------------------------------------------------------------ */

struct DbStmt {
    Db *db;
    sqlite3_stmt *handle;
    int rc;
};

DbStmt *db_prepare(Db *db, const char *sql, FILE *err)
{
    DbStmt *stmt = calloc(1u, sizeof(*stmt));

    if (stmt == NULL) {
        if (err != NULL) {
            fprintf(err, "redstone: out of memory\n");
        }
        return NULL;
    }
    stmt->db = db;
    stmt->rc = SQLITE_OK;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt->handle, NULL) != SQLITE_OK) {
        if (err != NULL) {
            fprintf(err, "redstone: %s\n", sqlite3_errmsg(db->handle));
        }
        sqlite3_finalize(stmt->handle);
        free(stmt);
        return NULL;
    }
    return stmt;
}

bool db_bind_text(DbStmt *stmt, int i, const char *text)
{
    int rc = text == NULL ? sqlite3_bind_null(stmt->handle, i)
                          : sqlite3_bind_text(stmt->handle, i, text, -1, SQLITE_TRANSIENT);

    return rc == SQLITE_OK;
}

bool db_step(DbStmt *stmt)
{
    if (stmt == NULL || stmt->handle == NULL) {
        return false;
    }
    stmt->rc = sqlite3_step(stmt->handle);
    return stmt->rc == SQLITE_ROW;
}

int db_stmt_columns(const DbStmt *stmt)
{
    return sqlite3_column_count(stmt->handle);
}

const char *db_stmt_name(DbStmt *stmt, int i)
{
    const char *name = sqlite3_column_name(stmt->handle, i);

    return name != NULL ? name : "";
}

const char *db_stmt_text(DbStmt *stmt, int i)
{
    const unsigned char *text = sqlite3_column_text(stmt->handle, i);

    return text != NULL ? (const char *)text : "";
}

long long db_stmt_int(DbStmt *stmt, int i)
{
    return (long long)sqlite3_column_int64(stmt->handle, i);
}

double db_stmt_real(DbStmt *stmt, int i)
{
    return sqlite3_column_double(stmt->handle, i);
}

const void *db_stmt_blob(DbStmt *stmt, int i, size_t *nbyte)
{
    const void *data = sqlite3_column_blob(stmt->handle, i);

    *nbyte = (size_t)sqlite3_column_bytes(stmt->handle, i);
    return data;
}

/* cppcheck-suppress staticFunction ; one of db.h's uniform column
 * accessors. out.c reaches NULL values through db_stmt_value today, but the
 * family stays complete. */
bool db_stmt_is_null(DbStmt *stmt, int i)
{
    return sqlite3_column_type(stmt->handle, i) == SQLITE_NULL;
}

OutType db_stmt_type(DbStmt *stmt, int i)
{
    return type_of(sqlite3_column_type(stmt->handle, i));
}

bool db_stmt_reset(DbStmt *stmt, FILE *err)
{
    bool ok;

    if (stmt == NULL) {
        return true;
    }
    /* sqlite3_reset returns the error code of the step that just ran. On a
     * reused statement that is the only place a failed INSERT is reported:
     * unlike db_finalize below, nothing else looks at it afterwards. */
    ok = sqlite3_reset(stmt->handle) == SQLITE_OK &&
         (stmt->rc == SQLITE_DONE || stmt->rc == SQLITE_OK);
    if (!ok && err != NULL) {
        fprintf(err, "redstone: %s\n", sqlite3_errmsg(stmt->db->handle));
    }
    /* Bindings survive a reset, so clear them: a row with fewer fields than
     * the last one would otherwise inherit the missing values. */
    (void)sqlite3_clear_bindings(stmt->handle);
    stmt->rc = SQLITE_OK;
    return ok;
}

bool db_finalize(DbStmt *stmt, FILE *err)
{
    bool ok;
    int rc;

    if (stmt == NULL) {
        return true;
    }
    rc = sqlite3_finalize(stmt->handle);
    ok = rc == SQLITE_OK && (stmt->rc == SQLITE_DONE || stmt->rc == SQLITE_OK);
    if (!ok && err != NULL) {
        fprintf(err, "redstone: %s\n", sqlite3_errmsg(stmt->db->handle));
    }
    free(stmt);
    return ok;
}

bool db_run(Db *db, const char *sql, FILE *err)
{
    char *msg = NULL;

    if (sqlite3_exec(db->handle, sql, NULL, NULL, &msg) != SQLITE_OK) {
        if (err != NULL) {
            fprintf(err, "redstone: %s\n", msg != NULL ? msg : sqlite3_errmsg(db->handle));
        }
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

char *db_scalar(Db *db, const char *sql)
{
    DbStmt *stmt = db_prepare(db, sql, NULL);
    char *value = NULL;

    if (stmt == NULL) {
        return NULL;
    }
    if (db_step(stmt) && !db_stmt_is_null(stmt, 0)) {
        value = dup_str(db_stmt_text(stmt, 0));
    }
    (void)db_finalize(stmt, NULL);
    return value;
}

/* --------------------------------------------------------------------------
 * Connection control
 * ------------------------------------------------------------------------ */

const char *db_errmsg(Db *db)
{
    const char *msg = sqlite3_errmsg(db->handle);

    return msg != NULL ? msg : "";
}

int db_changes(Db *db)
{
    return sqlite3_changes(db->handle);
}

bool db_autocommit(Db *db)
{
    return sqlite3_get_autocommit(db->handle) != 0;
}

/* One counter, in the two-column layout sqlite3(1) uses for .stats. */
static void stat_line(FILE *out, const char *label, long long current, long long highwater)
{
    if (highwater < 0) {
        fprintf(out, "%-36s %lld\n", label, current);
    } else {
        fprintf(out, "%-36s %lld (max %lld)\n", label, current, highwater);
    }
}

void db_print_stats(Db *db, FILE *out, bool reset)
{
    static const struct {
        const char *label;
        int op;
        bool global;
    } counter[] = {
        {"Memory Used:", SQLITE_STATUS_MEMORY_USED, true},
        {"Number of Outstanding Allocations:", SQLITE_STATUS_MALLOC_COUNT, true},
        {"Number of Pcache Pages Used:", SQLITE_STATUS_PAGECACHE_USED, true},
        {"Number of Pcache Overflow Bytes:", SQLITE_STATUS_PAGECACHE_OVERFLOW, true},
        {"Largest Allocation:", SQLITE_STATUS_MALLOC_SIZE, true},
        {"Largest Pcache Allocation:", SQLITE_STATUS_PAGECACHE_SIZE, true},
        {"Lookaside Slots Used:", SQLITE_DBSTATUS_LOOKASIDE_USED, false},
        {"Successful lookaside attempts:", SQLITE_DBSTATUS_LOOKASIDE_HIT, false},
        {"Lookaside failures due to size:", SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE, false},
        {"Lookaside failures due to OOM:", SQLITE_DBSTATUS_LOOKASIDE_MISS_FULL, false},
        {"Pager Heap Usage:", SQLITE_DBSTATUS_CACHE_USED, false},
        {"Schema Heap Usage:", SQLITE_DBSTATUS_SCHEMA_USED, false},
        {"Statement Heap/Lookaside Usage:", SQLITE_DBSTATUS_STMT_USED, false},
    };
    size_t i;

    for (i = 0u; i < sizeof(counter) / sizeof(counter[0]); i++) {
        int current = 0;
        int high = 0;

        if (counter[i].global) {
            sqlite3_int64 c64 = 0;
            sqlite3_int64 h64 = 0;

            if (sqlite3_status64(counter[i].op, &c64, &h64, reset ? 1 : 0) != SQLITE_OK) {
                continue;
            }
            stat_line(out, counter[i].label, (long long)c64, (long long)h64);
            continue;
        }
        if (sqlite3_db_status(db->handle, counter[i].op, &current, &high, reset ? 1 : 0) !=
            SQLITE_OK) {
            continue;
        }
        stat_line(out, counter[i].label, current, high);
    }
}

long long db_total_changes(Db *db)
{
    return (long long)sqlite3_total_changes(db->handle);
}

bool db_reopen(Db *db, const char *path, bool readonly, bool create, FILE *err)
{
    sqlite3 *handle = NULL;
    char *copy;
    int flags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;

    if (!readonly && create) {
        flags |= SQLITE_OPEN_CREATE;
    }
    copy = dup_str(path != NULL ? path : ":memory:");
    if (copy == NULL) {
        fprintf(err, "redstone: out of memory\n");
        return false;
    }
    /* Opened before anything is torn down: a failed `.open` must leave the
     * session exactly as it was, not drop the user into no database at all. */
    if (sqlite3_open_v2(copy, &handle, flags, NULL) != SQLITE_OK) {
        fprintf(err, "redstone: cannot open %s: %s\n", copy,
                handle != NULL ? sqlite3_errmsg(handle) : "unknown error");
        sqlite3_close(handle);
        free(copy);
        return false;
    }
    cache_dispose(db);
    sqlite3_close(db->handle);
    free(db->path);
    db->handle = handle;
    db->path = copy;
    db->schema_known = false;
    configure(db->handle);
    return true;
}

bool db_is_readonly(Db *db)
{
    return sqlite3_db_readonly(db->handle, "main") == 1;
}

/* One direction of sqlite3_backup, shared by .backup/.save and .restore. */
static bool backup_copy(Db *db, const char *dbname, const char *file, bool to_file, FILE *err)
{
    sqlite3 *other = NULL;
    sqlite3_backup *backup;
    sqlite3 *src;
    sqlite3 *dst;
    const char *srcname;
    const char *dstname;
    int flags = to_file ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) : SQLITE_OPEN_READONLY;
    int rc;

    if (dbname == NULL) {
        dbname = "main";
    }
    if (sqlite3_open_v2(file, &other, flags, NULL) != SQLITE_OK) {
        fprintf(err, "redstone: cannot open %s: %s\n", file,
                other != NULL ? sqlite3_errmsg(other) : "unknown error");
        sqlite3_close(other);
        return false;
    }
    src = to_file ? db->handle : other;
    dst = to_file ? other : db->handle;
    srcname = to_file ? dbname : "main";
    dstname = to_file ? "main" : dbname;

    backup = sqlite3_backup_init(dst, dstname, src, srcname);
    if (backup == NULL) {
        fprintf(err, "redstone: %s\n", sqlite3_errmsg(dst));
        sqlite3_close(other);
        return false;
    }
    while ((rc = sqlite3_backup_step(backup, 100)) == SQLITE_OK) {
        /* copying */
    }
    sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) {
        fprintf(err, "redstone: %s\n", sqlite3_errmsg(dst));
        sqlite3_close(other);
        return false;
    }
    sqlite3_close(other);
    if (!to_file) {
        cache_dispose(db);
        db->schema_known = false;
    }
    return true;
}

bool db_backup_to(Db *db, const char *dbname, const char *file, FILE *err)
{
    return backup_copy(db, dbname, file, true, err);
}

bool db_restore_from(Db *db, const char *dbname, const char *file, FILE *err)
{
    return backup_copy(db, dbname, file, false, err);
}

/* The limit and dbconfig tables are the only places a name maps to a
 * <sqlite3.h> constant, which is why they live here rather than in dot.c. */
static const struct {
    const char *name;
    int id;
} g_limit[] = {
    {"length", SQLITE_LIMIT_LENGTH},
    {"sql_length", SQLITE_LIMIT_SQL_LENGTH},
    {"column", SQLITE_LIMIT_COLUMN},
    {"expr_depth", SQLITE_LIMIT_EXPR_DEPTH},
    {"parser_depth", SQLITE_LIMIT_PARSER_DEPTH},
    {"compound_select", SQLITE_LIMIT_COMPOUND_SELECT},
    {"vdbe_op", SQLITE_LIMIT_VDBE_OP},
    {"function_arg", SQLITE_LIMIT_FUNCTION_ARG},
    {"attached", SQLITE_LIMIT_ATTACHED},
    {"like_pattern_length", SQLITE_LIMIT_LIKE_PATTERN_LENGTH},
    {"variable_number", SQLITE_LIMIT_VARIABLE_NUMBER},
    {"trigger_depth", SQLITE_LIMIT_TRIGGER_DEPTH},
    {"worker_threads", SQLITE_LIMIT_WORKER_THREADS},
};

const char *db_limit_name(size_t i)
{
    return i < sizeof(g_limit) / sizeof(g_limit[0]) ? g_limit[i].name : NULL;
}

bool db_limit(Db *db, const char *name, int value, int *current)
{
    size_t i;

    for (i = 0u; i < sizeof(g_limit) / sizeof(g_limit[0]); i++) {
        if (strcmp(name, g_limit[i].name) == 0) {
            /* -1 is sqlite's own "report without changing". */
            *current = sqlite3_limit(db->handle, g_limit[i].id, value);
            if (value >= 0) {
                *current = sqlite3_limit(db->handle, g_limit[i].id, -1);
            }
            return true;
        }
    }
    return false;
}

static const struct {
    const char *name;
    int id;
} g_dbconfig[] = {
    {"attach_create", SQLITE_DBCONFIG_ENABLE_ATTACH_CREATE},
    {"attach_write", SQLITE_DBCONFIG_ENABLE_ATTACH_WRITE},
    {"comments", SQLITE_DBCONFIG_ENABLE_COMMENTS},
    {"defensive", SQLITE_DBCONFIG_DEFENSIVE},
    {"dqs_ddl", SQLITE_DBCONFIG_DQS_DDL},
    {"dqs_dml", SQLITE_DBCONFIG_DQS_DML},
    {"enable_fkey", SQLITE_DBCONFIG_ENABLE_FKEY},
    {"enable_qpsg", SQLITE_DBCONFIG_ENABLE_QPSG},
    {"enable_trigger", SQLITE_DBCONFIG_ENABLE_TRIGGER},
    {"enable_view", SQLITE_DBCONFIG_ENABLE_VIEW},
    {"fts3_tokenizer", SQLITE_DBCONFIG_ENABLE_FTS3_TOKENIZER},
    {"fp_digits", SQLITE_DBCONFIG_FP_DIGITS},
    {"legacy_alter_table", SQLITE_DBCONFIG_LEGACY_ALTER_TABLE},
    {"legacy_file_format", SQLITE_DBCONFIG_LEGACY_FILE_FORMAT},
    {"load_extension", SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION},
    {"no_ckpt_on_close", SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE},
    {"reset_database", SQLITE_DBCONFIG_RESET_DATABASE},
    {"reverse_scanorder", SQLITE_DBCONFIG_REVERSE_SCANORDER},
    {"stmt_scanstatus", SQLITE_DBCONFIG_STMT_SCANSTATUS},
    {"trigger_eqp", SQLITE_DBCONFIG_TRIGGER_EQP},
    {"trusted_schema", SQLITE_DBCONFIG_TRUSTED_SCHEMA},
    {"writable_schema", SQLITE_DBCONFIG_WRITABLE_SCHEMA},
};

const char *db_dbconfig_name(size_t i)
{
    return i < sizeof(g_dbconfig) / sizeof(g_dbconfig[0]) ? g_dbconfig[i].name : NULL;
}

bool db_dbconfig(Db *db, const char *name, int value, int *current)
{
    size_t i;

    for (i = 0u; i < sizeof(g_dbconfig) / sizeof(g_dbconfig[0]); i++) {
        if (strcmp(name, g_dbconfig[i].name) == 0) {
            int ignored = 0;

            if (value >= 0) {
                /* The reporting argument is not optional: these dbconfig ops
                 * are variadic and always read an int*. */
                (void)sqlite3_db_config(db->handle, g_dbconfig[i].id, value, &ignored);
            }
            *current = 0;
            (void)sqlite3_db_config(db->handle, g_dbconfig[i].id, -1, current);
            return true;
        }
    }
    return false;
}

bool db_load_extension(Db *db, const char *file, const char *entry, FILE *err)
{
    char *msg = NULL;
    int rc;

    /* Loading was enabled when the connection was opened, exactly as
     * sqlite3(1) does it, so .dbconfig load_extension reports the truth and
     * turning it off there really turns it off. */
    rc = sqlite3_load_extension(db->handle, file, entry, &msg);
    if (rc != SQLITE_OK) {
        fprintf(err, "redstone: %s\n", msg != NULL ? msg : "cannot load extension");
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

void db_busy_timeout(Db *db, int ms)
{
    (void)sqlite3_busy_timeout(db->handle, ms);
}

/* The authorizer, progress and trace hooks all print; each remembers the
 * stream it was installed with, because the shell can redirect between the
 * `.trace` that turned it on and the statement that fires it. */
/* Upstream's shellAuth format, action names included, so that a script that
 * greps `.auth on` output keeps working. sqlite exposes no name-for-code
 * function in 3.53, hence the table. */
static int auth_hook(void *ctx, int op, const char *a, const char *b, const char *c, const char *d)
{
    static const char *const action[] = {"UNKNOWN",
                                         "CREATE_INDEX",
                                         "CREATE_TABLE",
                                         "CREATE_TEMP_INDEX",
                                         "CREATE_TEMP_TABLE",
                                         "CREATE_TEMP_TRIGGER",
                                         "CREATE_TEMP_VIEW",
                                         "CREATE_TRIGGER",
                                         "CREATE_VIEW",
                                         "DELETE",
                                         "DROP_INDEX",
                                         "DROP_TABLE",
                                         "DROP_TEMP_INDEX",
                                         "DROP_TEMP_TABLE",
                                         "DROP_TEMP_TRIGGER",
                                         "DROP_TEMP_VIEW",
                                         "DROP_TRIGGER",
                                         "DROP_VIEW",
                                         "INSERT",
                                         "PRAGMA",
                                         "READ",
                                         "SELECT",
                                         "TRANSACTION",
                                         "UPDATE",
                                         "ATTACH",
                                         "DETACH",
                                         "ALTER_TABLE",
                                         "REINDEX",
                                         "ANALYZE",
                                         "CREATE_VTABLE",
                                         "DROP_VTABLE",
                                         "FUNCTION",
                                         "SAVEPOINT",
                                         "RECURSIVE"};
    Db *db = (Db *)ctx;
    const char *arg[4];
    size_t i;

    if (db->auth_stream == NULL) {
        return SQLITE_OK;
    }
    arg[0] = a;
    arg[1] = b;
    arg[2] = c;
    arg[3] = d;
    fprintf(db->auth_stream, "authorizer: %s",
            (op >= 0 && (size_t)op < sizeof(action) / sizeof(action[0])) ? action[op] : "UNKNOWN");
    for (i = 0u; i < 4u; i++) {
        if (arg[i] == NULL) {
            fputs(" NULL", db->auth_stream);
        } else {
            fprintf(db->auth_stream, " \"%s\"", arg[i]);
        }
    }
    fputc('\n', db->auth_stream);
    return SQLITE_OK;
}

void db_set_authorizer(Db *db, bool on, FILE *stream)
{
    db->auth_stream = on ? stream : NULL;
    (void)sqlite3_set_authorizer(db->handle, on ? auth_hook : NULL, db);
}

static int progress_hook(void *ctx)
{
    Db *db = (Db *)ctx;

    if (db->progress_stream != NULL) {
        fprintf(db->progress_stream, "Progress %u\n", ++db->progress_count);
    }
    return 0;
}

void db_set_progress(Db *db, int nops, bool quiet, FILE *stream)
{
    db->progress_stream = quiet ? NULL : stream;
    db->progress_count = 0u;
    if (nops > 0) {
        sqlite3_progress_handler(db->handle, nops, progress_hook, db);
    } else {
        sqlite3_progress_handler(db->handle, 0, NULL, NULL);
    }
}

static int trace_hook(unsigned type, void *ctx, void *p, void *x)
{
    Db *db = (Db *)ctx;

    if (db->trace_stream == NULL) {
        return 0;
    }
    if (type == SQLITE_TRACE_STMT) {
        const char *sql = (const char *)x;

        fprintf(db->trace_stream, "%s%s\n", sql != NULL ? sql : "",
                sql != NULL && sql[0] != '-' ? ";" : "");
    } else if (type == SQLITE_TRACE_PROFILE) {
        sqlite3_int64 ns = *(sqlite3_int64 *)x;

        fprintf(db->trace_stream, "%s; -- %lld ns\n", sqlite3_sql((sqlite3_stmt *)p),
                (long long)ns);
    } else if (type == SQLITE_TRACE_ROW) {
        fputs("-- row\n", db->trace_stream);
    } else if (type == SQLITE_TRACE_CLOSE) {
        fputs("-- closing\n", db->trace_stream);
    }
    return 0;
}

void db_set_trace(Db *db, unsigned flags, FILE *stream)
{
    unsigned mask = 0u;

    db->trace_stream = flags != 0u ? stream : NULL;
    if ((flags & DB_TRACE_STMT) != 0u) {
        mask |= SQLITE_TRACE_STMT;
    }
    if ((flags & DB_TRACE_PROFILE) != 0u) {
        mask |= SQLITE_TRACE_PROFILE;
    }
    if ((flags & DB_TRACE_ROW) != 0u) {
        mask |= SQLITE_TRACE_ROW;
    }
    if ((flags & DB_TRACE_CLOSE) != 0u) {
        mask |= SQLITE_TRACE_CLOSE;
    }
    (void)sqlite3_trace_v2(db->handle, mask, mask != 0u ? trace_hook : NULL, db);
}

/* The file controls whose argument and result are plain enough to report as
 * text. Upstream's list is longer; the rest are diagnostics for sqlite's own
 * test suite and report pointers, not values. */
static const struct {
    const char *name;
    int op;
    char kind; /* 'i' int in/out, 'p' printable text out, 'v' void */
} g_filectrl[] = {
    {"chunk_size", SQLITE_FCNTL_CHUNK_SIZE, 'i'},
    {"data_version", SQLITE_FCNTL_DATA_VERSION, 'i'},
    {"has_moved", SQLITE_FCNTL_HAS_MOVED, 'i'},
    {"lock_timeout", SQLITE_FCNTL_LOCK_TIMEOUT, 'i'},
    {"persist_wal", SQLITE_FCNTL_PERSIST_WAL, 'i'},
    {"psow", SQLITE_FCNTL_POWERSAFE_OVERWRITE, 'i'},
    {"reserve_bytes", SQLITE_FCNTL_RESERVE_BYTES, 'i'},
    {"size_limit", SQLITE_FCNTL_SIZE_LIMIT, 'i'},
    {"tempfilename", SQLITE_FCNTL_TEMPFILENAME, 'p'},
};

const char *db_file_control_name(size_t i)
{
    return i < sizeof(g_filectrl) / sizeof(g_filectrl[0]) ? g_filectrl[i].name : NULL;
}

bool db_file_control(Db *db, const char *dbname, const char *op, const char *arg, FILE *out,
                     FILE *err)
{
    size_t i;

    for (i = 0u; i < sizeof(g_filectrl) / sizeof(g_filectrl[0]); i++) {
        int rc;

        if (strcmp(op, g_filectrl[i].name) != 0) {
            continue;
        }
        if (g_filectrl[i].kind == 'p') {
            char *text = NULL;

            rc = sqlite3_file_control(db->handle, dbname, g_filectrl[i].op, &text);
            if (rc == SQLITE_OK) {
                fprintf(out, "%s\n", text != NULL ? text : "");
            }
            sqlite3_free(text);
        } else if (strcmp(g_filectrl[i].name, "size_limit") == 0) {
            sqlite3_int64 value = arg != NULL ? (sqlite3_int64)strtoll(arg, NULL, 0) : -1;

            rc = sqlite3_file_control(db->handle, dbname, g_filectrl[i].op, &value);
            if (rc == SQLITE_OK) {
                fprintf(out, "%lld\n", (long long)value);
            }
        } else {
            int value = arg != NULL ? (int)strtol(arg, NULL, 0) : -1;

            rc = sqlite3_file_control(db->handle, dbname, g_filectrl[i].op, &value);
            if (rc == SQLITE_OK) {
                fprintf(out, "%d\n", value);
            }
        }
        if (rc != SQLITE_OK) {
            fprintf(err, "redstone: %s failed: %s\n", op, sqlite3_errstr(rc));
            return true; /* the name was known; the call was not */
        }
        return true;
    }
    return false;
}

const char *db_vfs_at(size_t i)
{
    const sqlite3_vfs *vfs = sqlite3_vfs_find(NULL);

    while (vfs != NULL && i > 0u) {
        vfs = vfs->pNext;
        i--;
    }
    return vfs != NULL ? vfs->zName : NULL;
}

const char *db_vfs_current(Db *db, const char *dbname)
{
    sqlite3_vfs *vfs = NULL;

    if (sqlite3_file_control(db->handle, dbname, SQLITE_FCNTL_VFS_POINTER, &vfs) != SQLITE_OK) {
        return NULL;
    }
    return vfs != NULL ? vfs->zName : NULL;
}

/* Upstream keeps bound parameters in TEMP.sqlite_parameters so that they
 * survive a reprepare and can be inspected with ordinary SQL. Matching that
 * is what makes `.parameter list` output comparable. */
static bool parameter_table(Db *db, FILE *err)
{
    return db_run(db,
                  "CREATE TABLE IF NOT EXISTS temp.sqlite_parameters"
                  "(key TEXT PRIMARY KEY, value)",
                  err);
}

bool db_parameter_set(Db *db, const char *key, const char *value, FILE *err)
{
    DbStmt *stmt;
    bool ok;

    if (!parameter_table(db, err)) {
        return false;
    }
    stmt = db_prepare(db,
                      "REPLACE INTO temp.sqlite_parameters(key,value)"
                      " VALUES(?1, ?2)",
                      err);
    if (stmt == NULL) {
        return false;
    }
    /* The value is stored as the result of evaluating it as SQL when that
     * works, so `.parameter set $n 5` binds an integer, as upstream does. */
    ok = db_bind_text(stmt, 1, key) && db_bind_text(stmt, 2, value);
    (void)db_step(stmt);
    return db_finalize(stmt, err) && ok;
}

bool db_parameter_unset(Db *db, const char *key, FILE *err)
{
    DbStmt *stmt;

    if (!parameter_table(db, err)) {
        return false;
    }
    stmt = db_prepare(db, "DELETE FROM temp.sqlite_parameters WHERE key=?1", err);
    if (stmt == NULL) {
        return false;
    }
    (void)db_bind_text(stmt, 1, key);
    (void)db_step(stmt);
    return db_finalize(stmt, err);
}

bool db_parameter_clear(Db *db, FILE *err)
{
    return db_run(db, "DROP TABLE IF EXISTS temp.sqlite_parameters", err);
}

const char *db_filename(Db *db, const char *dbname)
{
    const char *name = sqlite3_db_filename(db->handle, dbname != NULL ? dbname : "main");

    return (name != NULL && name[0] != '\0') ? name : NULL;
}

bool db_schema_readonly(Db *db, const char *dbname)
{
    return sqlite3_db_readonly(db->handle, dbname) > 0;
}

int db_txn_state(Db *db, const char *dbname)
{
    int state = sqlite3_txn_state(db->handle, dbname);

    return state < 0 ? 0 : state;
}

bool db_data_version(Db *db, const char *dbname, unsigned *value)
{
    return sqlite3_file_control(db->handle, dbname, SQLITE_FCNTL_DATA_VERSION, value) == SQLITE_OK;
}

bool db_is_keyword(const char *name)
{
    return sqlite3_keyword_check(name, (int)strlen(name)) != 0;
}

bool db_column_collation(Db *db, const char *table, const char *column, char *buf, size_t size)
{
    const char *seq = NULL;
    int rc;

    if (db == NULL || db->handle == NULL || size == 0u) {
        return false;
    }
    rc = sqlite3_table_column_metadata(db->handle, "main", table, column, NULL, &seq, NULL, NULL,
                                       NULL);
    if (rc != SQLITE_OK || seq == NULL) {
        return false;
    }
    /* The string belongs to the schema, which a later statement may reload,
     * so the caller gets a copy rather than a borrowed pointer. */
    (void)snprintf(buf, size, "%s", seq);
    return true;
}

bool db_glob(const char *pattern, const char *text)
{
    return pattern != NULL && text != NULL && sqlite3_strglob(pattern, text) == 0;
}
