#include "db.h"

#include "out.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    Db *db;
    int rc;

    db = calloc(1u, sizeof(*db));
    if (db == NULL) {
        fprintf(err, "sqlsh: out of memory\n");
        return NULL;
    }

    db->out = out_new(stdout);
    db->path = dup_str(path != NULL ? path : ":memory:");

    if (db->out == NULL || db->path == NULL) {
        fprintf(err, "sqlsh: out of memory\n");
        db_close(db);
        return NULL;
    }

    rc = sqlite3_open(db->path, &db->handle);
    if (rc != SQLITE_OK) {
        /* sqlite3_open allocates a handle even on failure, so the error text
         * is available and db_close still has something to free. */
        fprintf(err, "sqlsh: cannot open %s: %s\n", db->path,
                db->handle != NULL ? sqlite3_errmsg(db->handle) : sqlite3_errstr(rc));
        db_close(db);
        return NULL;
    }

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

/* cppcheck-suppress constParameterPointer ; the Db is not const to the
 * caller: it hands out a formatter the caller is expected to reconfigure. */
Out *db_out(Db *db)
{
    return db->out;
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
            fprintf(err, "sqlsh: %s\n", sqlite3_errmsg(db->handle));
            sqlite3_finalize(stmt);
            return false;
        }
        if (stmt == NULL) {
            /* Whitespace or a comment: nothing to run, move on. */
            tail = next;
            continue;
        }

        ncol = sqlite3_column_count(stmt);
        if (ncol == 0) {
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        } else {
            ok = print_result(db, stmt, ncol);
        }

        if (sqlite3_finalize(stmt) != SQLITE_OK || !ok) {
            fprintf(err, "sqlsh: %s\n", sqlite3_errmsg(db->handle));
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
            fprintf(err, "sqlsh: write failed\n");
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
         * completable the moment it exists. */
        db->tables = list_query(db->handle,
                                "SELECT name, type FROM sqlite_master"
                                "  WHERE type IN ('table','view')"
                                " UNION ALL"
                                " SELECT name, type FROM sqlite_temp_master"
                                "  WHERE type IN ('table','view')"
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
    struct timespec start;
    long limit_ms;
} Deadline;

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return ((now.tv_sec - start->tv_sec) * 1000L) + ((now.tv_nsec - start->tv_nsec) / 1000000L);
}

/* Returning non-zero aborts the statement. This is the only thing standing
 * between a Tab press and a full scan of a very large table. The argument
 * cannot be const: sqlite3_progress_handler dictates the signature. */
/* cppcheck-suppress constParameterCallback */
static int value_progress(void *arg)
{
    const Deadline *dl = (const Deadline *)arg;

    return elapsed_ms(&dl->start) > dl->limit_ms ? 1 : 0;
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
    if (clock_gettime(CLOCK_MONOTONIC, &dl.start) != 0) {
        dl.start.tv_sec = 0;
        dl.start.tv_nsec = 0;
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
