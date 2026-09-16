#include "db.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Column mode buffers the whole result set so widths can be computed before
 * anything is printed. Cap it: an unbounded SELECT should not be able to
 * exhaust memory just because the user forgot a LIMIT. Beyond the cap we keep
 * printing, using the widths learned so far. */
#define DB_WIDTH_SAMPLE_ROWS 200
#define DB_MAX_COLUMNS 128

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
    DbMode mode;
    bool headers;
    char *separator;
    char *null_text;

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

/* strdup is POSIX, not C99; spell it out so -std=c99 stays honest. */
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

static bool replace_str(char **slot, const char *value)
{
    char *copy = dup_str(value);

    if (copy == NULL && value != NULL) {
        return false;
    }
    free(*slot);
    *slot = copy;
    return true;
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

    db->mode = DB_MODE_COLUMN;
    db->headers = true;
    db->separator = dup_str("|");
    db->null_text = dup_str("");
    db->path = dup_str(path != NULL ? path : ":memory:");

    if (db->separator == NULL || db->null_text == NULL || db->path == NULL) {
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
    free(db->path);
    free(db->separator);
    free(db->null_text);
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

void db_set_mode(Db *db, DbMode mode)
{
    db->mode = mode;
}
DbMode db_mode(const Db *db)
{
    return db->mode;
}

void db_set_headers(Db *db, bool on)
{
    db->headers = on;
}
bool db_headers(const Db *db)
{
    return db->headers;
}

void db_set_separator(Db *db, const char *sep)
{
    (void)replace_str(&db->separator, sep);
}

void db_set_null_text(Db *db, const char *text)
{
    (void)replace_str(&db->null_text, text);
}

const char *db_null_text(const Db *db)
{
    return db->null_text;
}

bool db_is_complete(const char *text)
{
    /* sqlite3_complete wants a NUL-terminated statement and understands
     * strings, comments and BEGIN...END blocks, which is exactly the rule the
     * REPL needs for deciding whether to keep reading. */
    return sqlite3_complete(text) != 0;
}

/* --------------------------------------------------------------------------
 * Value rendering
 * ------------------------------------------------------------------------ */

/* Render column I of STMT as text. NULL columns yield db->null_text.
 * The returned pointer is owned by sqlite3 or by db, and is valid until the
 * next step of the statement. */
static const char *column_text(const Db *db, sqlite3_stmt *stmt, int i)
{
    const unsigned char *text;

    if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
        return db->null_text;
    }
    text = sqlite3_column_text(stmt, i);
    return text != NULL ? (const char *)text : db->null_text;
}

static void print_csv_field(FILE *out, const char *value)
{
    bool needs_quotes = strpbrk(value, ",\"\n\r") != NULL;
    size_t i;

    if (!needs_quotes) {
        fputs(value, out);
        return;
    }
    fputc('"', out);
    for (i = 0u; value[i] != '\0'; i++) {
        if (value[i] == '"') {
            fputc('"', out);
        }
        fputc(value[i], out);
    }
    fputc('"', out);
}

static void print_json_string(FILE *out, const char *value)
{
    size_t i;

    fputc('"', out);
    for (i = 0u; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];

        switch (c) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20u) {
                fprintf(out, "\\u%04x", (unsigned)c);
            } else {
                fputc((int)c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

/* --------------------------------------------------------------------------
 * Result printing, one function per mode
 * ------------------------------------------------------------------------ */

typedef struct {
    char **cells; /* ncol strings per row, row-major */
    size_t nrows;
    size_t cap;
} RowBuffer;

static void rowbuf_free(RowBuffer *buf, int ncol)
{
    size_t i;
    size_t total = buf->nrows * (size_t)ncol;

    for (i = 0u; i < total; i++) {
        free(buf->cells[i]);
    }
    free((void *)buf->cells);
    buf->cells = NULL;
    buf->nrows = 0u;
    buf->cap = 0u;
}

static bool rowbuf_push(RowBuffer *buf, int ncol, const Db *db, sqlite3_stmt *stmt)
{
    size_t i;
    size_t base;

    if (buf->nrows == buf->cap) {
        size_t new_cap = buf->cap == 0u ? 32u : buf->cap * 2u;
        char **cells;

        /* new_cap * ncol * sizeof cannot overflow with ncol bounded by
         * DB_MAX_COLUMNS, but the buffer grows without bound, so check. */
        if (new_cap > SIZE_MAX / ((size_t)ncol * sizeof(*cells))) {
            return false;
        }
        cells = (char **)realloc((void *)buf->cells, new_cap * (size_t)ncol * sizeof(*cells));
        if (cells == NULL) {
            return false;
        }
        buf->cells = cells;
        buf->cap = new_cap;
    }

    base = buf->nrows * (size_t)ncol;
    for (i = 0u; i < (size_t)ncol; i++) {
        buf->cells[base + i] = dup_str(column_text(db, stmt, (int)i));
        if (buf->cells[base + i] == NULL) {
            /* Zero the rest so rowbuf_free does not walk uninitialised slots. */
            while (++i < (size_t)ncol) {
                buf->cells[base + i] = NULL;
            }
            buf->nrows++;
            return false;
        }
    }
    buf->nrows++;
    return true;
}

/* Collect every row into BUF, tracking the widest cell per column over the
 * first DB_WIDTH_SAMPLE_ROWS rows. WIDTHS starts at the header widths. */
static bool collect_rows(const Db *db, sqlite3_stmt *stmt, int ncol, RowBuffer *buf, size_t *widths)
{
    int rc;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        size_t base;
        size_t i;

        if (!rowbuf_push(buf, ncol, db, stmt)) {
            return false;
        }
        if (buf->nrows > DB_WIDTH_SAMPLE_ROWS) {
            continue;
        }
        base = (buf->nrows - 1u) * (size_t)ncol;
        for (i = 0u; i < (size_t)ncol; i++) {
            size_t len = strlen(buf->cells[base + i]);

            if (len > widths[i]) {
                widths[i] = len;
            }
        }
    }
    return rc == SQLITE_DONE;
}

/* The gap between columns, and the newline that ends the last one. */
static void print_column_gap(FILE *out, size_t i, int ncol)
{
    fputs(i + 1u < (size_t)ncol ? "  " : "\n", out);
}

static void print_column_header(sqlite3_stmt *stmt, int ncol, FILE *out, const size_t *widths)
{
    size_t i;

    for (i = 0u; i < (size_t)ncol; i++) {
        const char *name = sqlite3_column_name(stmt, (int)i);

        fprintf(out, "%-*s", (int)widths[i], name != NULL ? name : "");
        print_column_gap(out, i, ncol);
    }
    for (i = 0u; i < (size_t)ncol; i++) {
        size_t d;

        for (d = 0u; d < widths[i]; d++) {
            fputc('-', out);
        }
        print_column_gap(out, i, ncol);
    }
}

static void print_column_rows(const RowBuffer *buf, int ncol, FILE *out, const size_t *widths)
{
    size_t r;

    for (r = 0u; r < buf->nrows; r++) {
        size_t base = r * (size_t)ncol;
        size_t i;

        for (i = 0u; i < (size_t)ncol; i++) {
            fprintf(out, "%-*s", (int)widths[i], buf->cells[base + i]);
            print_column_gap(out, i, ncol);
        }
    }
}

/* Column mode buffers the whole result: widths are not known until the rows
 * are. Every other mode streams. */
static bool print_column_mode(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out)
{
    RowBuffer buf = {NULL, 0u, 0u};
    size_t widths[DB_MAX_COLUMNS];
    size_t i;
    bool ok;

    /* db_exec never calls this with no columns, but the row buffer indexes on
     * ncol, so do not rely on a caller's guard for memory safety. */
    if (ncol <= 0) {
        return true;
    }
    if (ncol > DB_MAX_COLUMNS) {
        ncol = DB_MAX_COLUMNS; /* refuse to align absurdly wide results */
    }

    for (i = 0u; i < (size_t)ncol; i++) {
        const char *name = sqlite3_column_name(stmt, (int)i);

        widths[i] = name != NULL ? strlen(name) : 0u;
    }

    ok = collect_rows(db, stmt, ncol, &buf, widths);
    if (ok) {
        if (db->headers) {
            print_column_header(stmt, ncol, out, widths);
        }
        print_column_rows(&buf, ncol, out, widths);
    }

    rowbuf_free(&buf, ncol);
    return ok;
}

static void print_json_row(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out, size_t row)
{
    size_t i;

    fputs(row == 0u ? "\n  {" : ",\n  {", out);
    for (i = 0u; i < (size_t)ncol; i++) {
        const char *name = sqlite3_column_name(stmt, (int)i);
        int type = sqlite3_column_type(stmt, (int)i);

        if (i > 0u) {
            fputc(',', out);
        }
        fputc(' ', out);
        print_json_string(out, name != NULL ? name : "");
        fputs(": ", out);
        if (type == SQLITE_NULL) {
            fputs("null", out);
        } else if (type == SQLITE_INTEGER || type == SQLITE_FLOAT) {
            /* Numeric text from sqlite is already valid JSON. */
            fputs((const char *)sqlite3_column_text(stmt, (int)i), out);
        } else {
            print_json_string(out, column_text(db, stmt, (int)i));
        }
    }
    fputs(" }", out);
}

static void print_line_row(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out, size_t row)
{
    size_t i;

    if (row > 0u) {
        fputc('\n', out);
    }
    for (i = 0u; i < (size_t)ncol; i++) {
        const char *name = sqlite3_column_name(stmt, (int)i);

        fprintf(out, "%s = %s\n", name != NULL ? name : "", column_text(db, stmt, (int)i));
    }
}

static void print_csv_row(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out)
{
    size_t i;

    for (i = 0u; i < (size_t)ncol; i++) {
        if (i > 0u) {
            fputc(',', out);
        }
        print_csv_field(out, column_text(db, stmt, (int)i));
    }
    fputc('\n', out);
}

static void print_list_row(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out)
{
    size_t i;

    for (i = 0u; i < (size_t)ncol; i++) {
        if (i > 0u) {
            fputs(db->separator, out);
        }
        fputs(column_text(db, stmt, (int)i), out);
    }
    fputc('\n', out);
}

/* Only list and csv carry a header row; json names each field per row and line
 * mode prints "name = value". */
static void print_streaming_header(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out)
{
    size_t i;

    if (!db->headers || (db->mode != DB_MODE_LIST && db->mode != DB_MODE_CSV)) {
        return;
    }
    for (i = 0u; i < (size_t)ncol; i++) {
        const char *name = sqlite3_column_name(stmt, (int)i);
        const char *safe = name != NULL ? name : "";

        if (i > 0u) {
            fputs(db->mode == DB_MODE_CSV ? "," : db->separator, out);
        }
        if (db->mode == DB_MODE_CSV) {
            print_csv_field(out, safe);
        } else {
            fputs(safe, out);
        }
    }
    fputc('\n', out);
}

static bool print_streaming_mode(const Db *db, sqlite3_stmt *stmt, int ncol, FILE *out)
{
    int rc;
    size_t row = 0u;

    print_streaming_header(db, stmt, ncol, out);
    if (db->mode == DB_MODE_JSON) {
        fputs("[", out);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        switch (db->mode) {
        case DB_MODE_JSON:
            print_json_row(db, stmt, ncol, out, row);
            break;
        case DB_MODE_LINE:
            print_line_row(db, stmt, ncol, out, row);
            break;
        case DB_MODE_CSV:
            print_csv_row(db, stmt, ncol, out);
            break;
        case DB_MODE_LIST:
        default:
            print_list_row(db, stmt, ncol, out);
            break;
        }
        row++;
    }

    if (db->mode == DB_MODE_JSON) {
        fputs(row > 0u ? "\n]\n" : "]\n", out);
    }

    return rc == SQLITE_DONE;
}

bool db_exec(Db *db, const char *text, FILE *out, FILE *err)
{
    const char *tail = text;

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
        } else if (db->mode == DB_MODE_COLUMN) {
            ok = print_column_mode(db, stmt, ncol, out);
        } else {
            ok = print_streaming_mode(db, stmt, ncol, out);
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
