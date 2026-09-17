/*
 * import.c - reading tabular text in, and handing tabular text out.
 *
 * `.import` reads CSV or ASCII-delimited text and INSERTs it; `.excel` and
 * `.www` are the mirror image, writing a query's result to a temp file in a
 * format an external program can open. All three are "text in or out plus a
 * program", which is why they share this file.
 */
#include "import.h"

#include "db.h"
#include "out.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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

/* --------------------------------------------------------------------------
 * A tiny growable string, for building SQL text and messages. Nothing here
 * needs to be fast; .import runs once per file, not once per row.
 * ------------------------------------------------------------------------ */

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} Str;

static bool str_reserve(Str *s, size_t extra)
{
    size_t need = s->len + extra + 1u;
    char *grown;

    /* See out.c's str_grow: p is tested too, so that "already large enough"
     * also means "already allocated". */
    if (s->p != NULL && need <= s->cap) {
        return true;
    }
    if (need < s->cap * 2u) {
        need = s->cap * 2u;
    }
    grown = realloc(s->p, need);
    if (grown == NULL) {
        return false;
    }
    s->p = grown;
    s->cap = need;
    return true;
}

static bool str_append(Str *s, const char *text)
{
    size_t n = strlen(text);

    if (!str_reserve(s, n)) {
        return false;
    }
    memcpy(s->p + s->len, text, n + 1u);
    s->len += n;
    return true;
}

static bool str_appendc(Str *s, char c)
{
    char buf[2];

    buf[0] = c;
    buf[1] = '\0';
    return str_append(s, buf);
}

/* Quote NAME as an SQL identifier: wrap in double quotes, doubling any
 * embedded double quote, the way sqlite's "%w" does. */
static bool str_append_ident(Str *s, const char *name)
{
    size_t i;

    if (!str_appendc(s, '"')) {
        return false;
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] == '"' && !str_appendc(s, '"')) {
            return false;
        }
        if (!str_appendc(s, name[i])) {
            return false;
        }
    }
    return str_appendc(s, '"');
}

static void str_free(Str *s)
{
    free(s->p);
    s->p = NULL;
    s->len = 0u;
    s->cap = 0u;
}

/* --------------------------------------------------------------------------
 * Reading one field of CSV or ASCII-delimited text.
 *
 * This is a from-scratch, simplified port of upstream's csv_read_one_field /
 * ascii_read_one_field: RFC4180 quoting ("" for a literal quote, a quoted
 * field may hold the separators and newlines) with no backslash-escape
 * option (upstream's -esc/-qesc, which this port does not expose) and no
 * UTF-8 BOM skip on the first field (a real gap on Excel-exported CSV, noted
 * in the final report).
 * ------------------------------------------------------------------------ */

typedef struct {
    FILE *in;
    char *buf;
    size_t len;
    size_t cap;
    int line; /* 1-based line/row number, for diagnostics */
    int term; /* the byte that ended the last field, or EOF */
} Reader;

static void reader_init(Reader *rd, FILE *in)
{
    rd->in = in;
    rd->buf = NULL;
    rd->len = 0u;
    rd->cap = 0u;
    rd->line = 1;
    rd->term = 0;
}

static void reader_free(Reader *rd)
{
    free(rd->buf);
    rd->buf = NULL;
}

static void reader_append(Reader *rd, int c)
{
    if (rd->len + 1u >= rd->cap) {
        size_t newcap = rd->cap == 0u ? 128u : rd->cap * 2u;
        char *grown = realloc(rd->buf, newcap);

        if (grown == NULL) {
            return; /* best effort: drop the byte rather than crash on OOM */
        }
        rd->buf = grown;
        rd->cap = newcap;
    }
    rd->buf[rd->len++] = (char)c;
}

/* Trim one trailing byte, used to drop a bare '\r' that preceded the '\n'
 * row separator. */
static void reader_trim_last(Reader *rd)
{
    if (rd->len > 0u) {
        rd->len--;
    }
}

static const char *csv_read_field(Reader *rd, char colsep, char rowsep)
{
    int c;

    rd->len = 0u;
    c = fgetc(rd->in);
    if (c == EOF) {
        rd->term = EOF;
        return NULL;
    }
    if (c == '"') {
        int pc = 0;
        int ppc = 0;

        for (;;) {
            c = fgetc(rd->in);
            if (c == (unsigned char)rowsep) {
                rd->line++;
            }
            if (c == '"' && pc == '"') {
                /* Second half of a doubled quote: swallow it, and stop
                 * treating the run of quotes as significant. */
                pc = 0;
                continue;
            }
            if ((c == (unsigned char)colsep && pc == '"') ||
                (c == (unsigned char)rowsep && pc == '"') ||
                (c == (unsigned char)rowsep && pc == '\r' && ppc == '"') ||
                (c == EOF && pc == '"')) {
                /* The last byte appended was the quote that closes the
                 * field, not part of its content, so drop it -- preceded by
                 * the \r of a \r\n row separator when that is what ended
                 * the field. Exactly those one or two bytes: any earlier
                 * quote in the buffer is content that survived a doubling. */
                if (pc == '\r') {
                    reader_trim_last(rd);
                }
                reader_trim_last(rd);
                rd->term = c;
                break;
            }
            if (c == EOF) {
                /* Unterminated quoted field: take what there is. Upstream
                 * warns here too; this port does not (documented gap). */
                rd->term = c;
                break;
            }
            reader_append(rd, c);
            ppc = pc;
            pc = c;
        }
    } else {
        while (c != EOF && c != (unsigned char)colsep && c != (unsigned char)rowsep) {
            reader_append(rd, c);
            c = fgetc(rd->in);
        }
        if (c == (unsigned char)rowsep) {
            rd->line++;
            if (rd->len > 0u && rd->buf[rd->len - 1u] == '\r') {
                reader_trim_last(rd);
            }
        }
        rd->term = c;
    }
    reader_append(rd, 0);
    rd->len--; /* the NUL is not part of the field's length */
    return rd->buf != NULL ? rd->buf : "";
}

static const char *ascii_read_field(Reader *rd, char colsep, char rowsep)
{
    int c;

    rd->len = 0u;
    c = fgetc(rd->in);
    if (c == EOF) {
        rd->term = EOF;
        return NULL;
    }
    while (c != EOF && c != (unsigned char)colsep && c != (unsigned char)rowsep) {
        reader_append(rd, c);
        c = fgetc(rd->in);
    }
    if (c == (unsigned char)rowsep) {
        rd->line++;
    }
    rd->term = c;
    reader_append(rd, 0);
    rd->len--;
    return rd->buf != NULL ? rd->buf : "";
}

/* --------------------------------------------------------------------------
 * .import
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *file;
    const char *table;
    const char *schema;
    char colsep;
    char rowsep;
    bool ascii;
    bool ascii_set;
    bool csv_set;
    int verbose;
    long long skip;
} ImportArgs;

static bool import_parse_args(Shell *sh, int argc, char **argv, ImportArgs *a)
{
    int i;

    memset(a, 0, sizeof(*a));
    for (i = 1; i < argc; i++) {
        const char *z = argv[i];

        if (z[0] == '-' && z[1] == '-') {
            z++;
        }
        if (z[0] != '-') {
            if (a->file == NULL) {
                a->file = z;
            } else if (a->table == NULL) {
                a->table = z;
            } else {
                fprintf(shell_err(sh), "sqlsh: .import: unknown argument \"%s\"\n", argv[i]);
                return false;
            }
        } else if (strcmp(z, "-v") == 0) {
            a->verbose++;
        } else if (strcmp(z, "-schema") == 0) {
            if (i >= argc - 1) {
                fprintf(shell_err(sh), "sqlsh: .import: -schema requires an argument\n");
                return false;
            }
            a->schema = argv[++i];
        } else if (strcmp(z, "-skip") == 0) {
            if (i >= argc - 1) {
                fprintf(shell_err(sh), "sqlsh: .import: -skip requires an argument\n");
                return false;
            }
            a->skip = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(z, "-ascii") == 0) {
            a->ascii = true;
            a->ascii_set = true;
        } else if (strcmp(z, "-csv") == 0) {
            a->ascii = false;
            a->csv_set = true;
        } else {
            fprintf(shell_err(sh), "sqlsh: .import: unknown option \"%s\"\n", argv[i]);
            return false;
        }
    }
    if (a->table == NULL) {
        fprintf(shell_err(sh), "sqlsh: .import: missing %s argument\n",
                a->file == NULL ? "FILE" : "TABLE");
        return false;
    }
    return true;
}

/* Column names gathered from the first row, deduplicated the way upstream's
 * zAutoColumn does for CREATE TABLE: a case-insensitive collision gets an
 * "_N" suffix (N its 1-based column position, zero-padded to the width of
 * the column count). Upstream's actual algorithm is a small SQL program of
 * its own (see zAutoColumn in reference/shell.c) that also chops redundant
 * suffixes back off names that already end in one; this is a plainer
 * approximation, documented as a gap in the final report. */
static bool dedup_names(char **names, int n, Str *coldefs, Str *renamed)
{
    int width = 1;
    int tmp = n;
    int i;

    while (tmp >= 10) {
        width++;
        tmp /= 10;
    }
    for (i = 0; i < n; i++) {
        int count = 0;
        int j;
        bool dup;
        const char *final_name = names[i];
        char *built = NULL;

        for (j = 0; j < n; j++) {
            if (strcasecmp(names[i], names[j]) == 0) {
                count++;
            }
        }
        dup = count > 1;
        if (dup) {
            char suffix[32];
            Str full = {NULL, 0u, 0u};

            snprintf(suffix, sizeof(suffix), "_%0*d", width, i + 1);
            if (!str_append(&full, names[i]) || !str_append(&full, suffix)) {
                str_free(&full);
                return false;
            }
            built = full.p; /* ownership moves here */
            final_name = built;
        }

        /* Upstream gives the generated columns no type and lays them out four
         * to a line, continuation lines indented by one space (zCollectVar in
         * reference/shell.c). The layout is visible in every later .schema and
         * .dump of the table, so it is parity, not formatting. */
        if ((i > 0 && !str_append(coldefs, i % 4 > 0 ? ", " : ",\n ")) ||
            !str_append_ident(coldefs, final_name)) {
            free(built);
            return false;
        }
        if (dup && renamed != NULL) {
            if ((renamed->len > 0u && !str_append(renamed, ",\n")) || !str_append(renamed, "\"") ||
                !str_append(renamed, names[i]) || !str_append(renamed, "\" to \"") ||
                !str_append(renamed, final_name) || !str_append(renamed, "\"")) {
                free(built);
                return false;
            }
        }
        free(built);
    }
    return true;
}

/* Number of columns TABLE (optionally schema-qualified) has, or 0 if it does
 * not exist. Works for views too, which is what upstream's existence check
 * (sqlite3_table_column_metadata) also tolerates: a view being "found" means
 * .import skips CREATE and goes straight to the INSERT, which then fails on
 * its own if the view has no INSTEAD OF trigger. */
static int table_column_count(Db *db, const char *table, const char *schema)
{
    DbStmt *stmt = db_prepare(db, "SELECT count(*) FROM pragma_table_info(?1,?2)", NULL);
    long long n = 0;

    if (stmt == NULL) {
        return 0;
    }
    (void)db_bind_text(stmt, 1, table);
    (void)db_bind_text(stmt, 2, schema);
    if (db_step(stmt)) {
        n = db_stmt_int(stmt, 0);
    }
    (void)db_finalize(stmt, NULL);
    return (int)n;
}

static void qualified_name(Str *s, const char *schema, const char *table)
{
    if (schema != NULL) {
        (void)str_append_ident(s, schema);
        (void)str_append(s, ".");
    }
    (void)str_append_ident(s, table);
}

/* Read the header row and CREATE the table from it. Returns false (after
 * reporting) on an empty file or a failed CREATE. */
static bool import_create_table(Shell *sh, Reader *rd, const ImportArgs *a,
                                const char *(*read_field)(Reader *, char, char))
{
    char **names = NULL;
    size_t n = 0;
    size_t cap = 0;
    Str coldefs = {NULL, 0u, 0u};
    Str renamed = {NULL, 0u, 0u};
    Str sql = {NULL, 0u, 0u};
    bool ok;

    for (;;) {
        const char *z = read_field(rd, a->colsep, a->rowsep);

        if (z == NULL) {
            break;
        }
        if (n == cap) {
            size_t newcap = cap == 0u ? 8u : cap * 2u;
            char **grown = realloc(names, newcap * sizeof(*names));

            if (grown == NULL) {
                break;
            }
            names = grown;
            cap = newcap;
        }
        names[n++] = dup_str(z[0] != '\0' ? z : "?");
        if (rd->term != a->colsep) {
            break;
        }
    }
    if (n == 0u) {
        fprintf(shell_err(sh), "%s: empty file\n", a->file);
        return false;
    }

    ok = dedup_names(names, (int)n, &coldefs, &renamed);
    if (ok && renamed.len > 0u) {
        fprintf(shell_out(sh), "Columns renamed during .import %s due to duplicates:\n%s\n",
                a->file, renamed.p);
    }
    if (ok) {
        ok = str_append(&sql, "CREATE TABLE ");
        if (ok) {
            qualified_name(&sql, a->schema, a->table);
        }
        ok = ok && str_append(&sql, "(\n") && str_append(&sql, coldefs.p) && str_append(&sql, ")");
    }
    if (ok) {
        if (a->verbose >= 1) {
            fprintf(shell_out(sh), "%s\n", sql.p);
        }
        ok = db_run(shell_db(sh), sql.p, shell_err(sh));
    }

    {
        size_t i;

        for (i = 0; i < n; i++) {
            free(names[i]);
        }
        free(names);
    }
    str_free(&coldefs);
    str_free(&renamed);
    str_free(&sql);
    return ok;
}

bool import_cmd_import(Shell *sh, int argc, char **argv)
{
    ImportArgs a;
    FILE *file;
    Reader rd;
    const char *(*read_field)(Reader *, char, char);
    int ncol;
    Str insert_sql = {NULL, 0u, 0u};
    long long nskip;
    long long row_count = 0;
    long long err_count = 0;
    bool ok = true;

    if (shell_unsafe(sh, "import")) {
        return false;
    }
    if (!import_parse_args(sh, argc, argv, &a)) {
        return false;
    }

    /* The reading function defaults to whatever the current output mode
     * implies; -ascii/-csv override it explicitly. */
    read_field = strcmp(out_mode_name(db_out(shell_db(sh))), "ascii") == 0 ? ascii_read_field
                                                                           : csv_read_field;
    if (a.ascii_set) {
        read_field = ascii_read_field;
    } else if (a.csv_set) {
        read_field = csv_read_field;
    }

    a.colsep = 0;
    a.rowsep = 0;
    if (a.ascii_set) {
        a.colsep = '\x1f';
        a.rowsep = '\x1e';
    } else if (a.csv_set) {
        a.colsep = ',';
        a.rowsep = '\n';
    }
    if (a.colsep == 0) {
        const char *sep = out_colsep(db_out(shell_db(sh)));

        a.colsep = (char)((sep != NULL && sep[0] != '\0') ? sep[0] : ',');
    }
    if (a.rowsep == 0) {
        const char *sep = out_rowsep(db_out(shell_db(sh)));

        a.rowsep = (char)((sep != NULL && sep[0] != '\0') ? sep[0] : '\n');
    }
    if (a.rowsep == '\r' && read_field != ascii_read_field) {
        a.rowsep = '\n';
    }
    if (((unsigned char)a.colsep & 0x80u) != 0u) {
        fprintf(shell_err(sh), "sqlsh: .import column separator must be ASCII\n");
        return false;
    }
    if (((unsigned char)a.rowsep & 0x80u) != 0u) {
        fprintf(shell_err(sh), "sqlsh: .import row separator must be ASCII\n");
        return false;
    }

    file = fopen(a.file, "rb");
    if (file == NULL) {
        fprintf(shell_err(sh), "sqlsh: .import: cannot open \"%s\"\n", a.file);
        return false;
    }
    reader_init(&rd, file);

    if (a.verbose >= 1) {
        fprintf(shell_out(sh), "Column separator \"%c\", row separator \"%c\"\n", a.colsep,
                a.rowsep);
    }

    for (nskip = a.skip; nskip > 0; nskip--) {
        while (read_field(&rd, a.colsep, a.rowsep) != NULL && rd.term == a.colsep) {
        }
    }

    ncol = table_column_count(shell_db(sh), a.table, a.schema);
    if (ncol == 0) {
        if (!import_create_table(sh, &rd, &a, read_field)) {
            reader_free(&rd);
            (void)fclose(file);
            return false;
        }
        ncol = table_column_count(shell_db(sh), a.table, a.schema);
    }
    if (ncol == 0) {
        /* Table exists with no columns, or the CREATE somehow produced
         * none: upstream treats this as nothing to do, not an error. */
        reader_free(&rd);
        (void)fclose(file);
        return true;
    }

    ok = str_append(&insert_sql, "INSERT INTO ");
    if (ok) {
        qualified_name(&insert_sql, a.schema, a.table);
        ok = str_append(&insert_sql, " VALUES(?");
    }
    {
        int i;

        for (i = 1; ok && i < ncol; i++) {
            ok = str_append(&insert_sql, ",?");
        }
    }
    ok = ok && str_append(&insert_sql, ")");
    if (!ok) {
        fprintf(shell_err(sh), "sqlsh: out of memory\n");
        str_free(&insert_sql);
        reader_free(&rd);
        (void)fclose(file);
        return false;
    }
    if (a.verbose >= 2) {
        fprintf(shell_out(sh), "Insert using: %s\n", insert_sql.p);
    }

    /* One transaction for the whole load, for speed -- but only when the
     * caller has none open, so .import neither nests a transaction nor commits
     * one it did not start. The INSERT is prepared once and reset per row:
     * re-compiling it for every line dominates the cost of a large file. */
    {
        bool needs_commit = db_autocommit(shell_db(sh)) && db_run(shell_db(sh), "BEGIN", NULL);
        DbStmt *ins = db_prepare(shell_db(sh), insert_sql.p, shell_err(sh));

        if (ins == NULL) {
            ok = false;
        }
        while (ok) {
            int start_line = rd.line;
            int i;

            for (i = 0; i < ncol; i++) {
                const char *z = read_field(&rd, a.colsep, a.rowsep);

                if (z == NULL && i == 0) {
                    break;
                }
                if (read_field == ascii_read_field && i == 0 && (z == NULL || z[0] == '\0')) {
                    break;
                }
                if (z == NULL && read_field == csv_read_field && i == ncol - 1 && i > 0) {
                    z = "";
                }
                (void)db_bind_text(ins, i + 1, z);
                if (i < ncol - 1 && rd.term != a.colsep) {
                    if (i == 0 && z != NULL && (strcmp(z, "\n") == 0 || strcmp(z, "\r\n") == 0)) {
                        break;
                    }
                    fprintf(shell_err(sh),
                            "%s:%d: expected %d columns but found %d"
                            " - filling the rest with NULL\n",
                            a.file, start_line, ncol, i + 1);
                    {
                        int j;

                        for (j = i + 1; j < ncol; j++) {
                            (void)db_bind_text(ins, j + 1, NULL);
                        }
                    }
                    i = ncol;
                    break;
                }
            }
            if (rd.term == a.colsep) {
                int extra = i;

                while (rd.term == a.colsep) {
                    (void)read_field(&rd, a.colsep, a.rowsep);
                    extra++;
                }
                fprintf(shell_err(sh), "%s:%d: expected %d columns but found %d - extras ignored\n",
                        a.file, start_line, ncol, extra);
            }
            if (i >= ncol) {
                (void)db_step(ins);
                if (!db_stmt_reset(ins, shell_err(sh))) {
                    err_count++;
                    if (shell_flag(sh, SHELL_BAIL)) {
                        break;
                    }
                } else {
                    row_count++;
                }
            } else {
                (void)db_stmt_reset(ins, NULL);
            }
            if (rd.term == EOF) {
                break;
            }
        }
        (void)db_finalize(ins, NULL);

        if (needs_commit) {
            (void)db_run(shell_db(sh), "COMMIT", shell_err(sh));
        }
    }

    if (a.verbose > 0) {
        fprintf(shell_out(sh), "Added %lld rows with %lld errors using %d lines of input\n",
                row_count, err_count, rd.line - 1);
    }

    str_free(&insert_sql);
    reader_free(&rd);
    (void)fclose(file);
    return ok && err_count == 0;
}

/* --------------------------------------------------------------------------
 * .excel and .www
 *
 * Both are "write the next result set to a temp file in a fixed format and
 * hand it to the system's opener". Unlike upstream, which makes them pure
 * `.once` shorthands that always defer to whatever statement the user types
 * next, this port also accepts an optional QUERY argument so the two can be
 * exercised (and tested) without a REPL. That is a deliberate extension
 * beyond upstream, called out in the final report.
 * ------------------------------------------------------------------------ */

/* A path under $TMPDIR (or /tmp) that includes the pid, per the house rule
 * against tmpnam. Not using mkstemp: upstream's own newTempFile does not
 * either, and the opener needs a stable, predictable name to launch against
 * (and possibly an extension it sniffs, for .csv/.html). */
static bool make_temp_path(char *buf, size_t bufsize, const char *tag, const char *ext)
{
    const char *dir = getenv("TMPDIR");
    int n;

    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    n = snprintf(buf, bufsize, "%s/sqlsh-%s-%ld.%s", dir, tag, (long)getpid(), ext);
    return n > 0 && (size_t)n < bufsize;
}

typedef struct {
    char *mode;
    char *colsep;
    char *rowsep;
    bool headers;
} OutSaved;

static bool out_save(const Out *out, OutSaved *saved)
{
    saved->mode = dup_str(out_mode_name(out));
    saved->colsep = dup_str(out_colsep(out));
    saved->rowsep = dup_str(out_rowsep(out));
    saved->headers = out_headers(out);
    if (saved->mode == NULL || saved->colsep == NULL || saved->rowsep == NULL) {
        /* Nothing was changed yet, so the caller will not call out_restore:
         * release the partial copy here rather than leak it. */
        free(saved->mode);
        free(saved->colsep);
        free(saved->rowsep);
        saved->mode = NULL;
        saved->colsep = NULL;
        saved->rowsep = NULL;
        return false;
    }
    return true;
}

static void out_restore(Out *out, OutSaved *saved)
{
    if (saved->mode != NULL) {
        (void)out_set_mode(out, saved->mode);
    }
    if (saved->colsep != NULL) {
        out_set_colsep(out, saved->colsep);
    }
    if (saved->rowsep != NULL) {
        out_set_rowsep(out, saved->rowsep);
    }
    out_set_headers(out, saved->headers);
    free(saved->mode);
    free(saved->colsep);
    free(saved->rowsep);
}

static void launch_opener(Shell *sh, const char *path)
{
    char cmd[600];
    int n = snprintf(cmd, sizeof(cmd), "xdg-open '%s'", path);

    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        fprintf(shell_err(sh), "sqlsh: temp file path too long to open\n");
        return;
    }
    /* .excel and .www exist to hand the file to the desktop's opener; -safe
     * refuses both before a temporary file is even named. The path is one we
     * built ourselves under the temp directory, not user text. */
    /* NOLINTNEXTLINE(cert-env33-c,clang-analyzer-optin.taint.GenericTaint) */
    if (system(cmd) != 0) {
        fprintf(shell_err(sh), "sqlsh: failed: [%s]\n", cmd);
    }
}

/* Join argv[1..argc) with single spaces back into one query string, so a
 * caller does not have to quote it. Returns NULL (nothing to run) for
 * argc<=1. */
static char *join_query(int argc, char **argv)
{
    Str s = {NULL, 0u, 0u};
    int i;

    if (argc <= 1) {
        return NULL;
    }
    for (i = 1; i < argc; i++) {
        if ((i > 1 && !str_append(&s, " ")) || !str_append(&s, argv[i])) {
            str_free(&s);
            return NULL;
        }
    }
    return s.p;
}

/* Shared machinery for .excel and .www: redirect to a fresh temp file,
 * switch the output mode, run QUERY if one was given (restoring the prior
 * mode once it is done, and only then launching the opener), and otherwise
 * launch the opener immediately since nothing here is told when the next
 * REPL statement, the one the redirect and mode change are actually for,
 * finishes. That immediate-launch path is the one real behavioural gap from
 * upstream: it opens the viewer on a file that is still empty, whereas
 * upstream (in output_reset) opens it only once that next statement's
 * output has been written. It is also left in whatever mode this call set,
 * since there is no hook to restore it after a statement this function does
 * not run itself. */
static void apply_output_mode(Out *out, bool is_excel)
{
    if (is_excel) {
        /* SEP_Comma / SEP_CrLf in upstream: Excel's CSV reader wants CRLF
         * rows regardless of the platform, so this is fixed, not inherited
         * from whatever the csv preset's own default row separator is. */
        (void)out_set_mode(out, "csv");
        out_set_colsep(out, ",");
        out_set_rowsep(out, "\r\n");
    } else {
        (void)out_set_mode(out, "html");
    }
}

static bool excel_or_www(Shell *sh, int argc, char **argv, const char *name, const char *ext,
                         bool is_excel)
{
    char path[512];
    char *query;
    Out *out;
    OutSaved saved;
    bool ok = true;

    if (shell_unsafe(sh, name)) {
        return false;
    }
    if (!make_temp_path(path, sizeof(path), name, ext)) {
        fprintf(shell_err(sh), "sqlsh: .%s: cannot build a temporary file name\n", name);
        return false;
    }

    out = db_out(shell_db(sh));
    query = join_query(argc, argv);

    if (query != NULL) {
        /* We control both ends here, so bracket the mode change and the
         * redirect around exactly this one statement, and launch the
         * opener only once the file holds real content. */
        if (!out_save(out, &saved)) {
            free(query);
            fprintf(shell_err(sh), "sqlsh: out of memory\n");
            return false;
        }
        ok = shell_redirect(sh, path, false);
        if (ok) {
            apply_output_mode(out, is_excel);
            if (!is_excel) {
                fprintf(shell_out(sh), "<!DOCTYPE html>\n<HTML><BODY><PRE>\n");
            }
            ok = shell_exec(sh, query);
            if (!is_excel) {
                fprintf(shell_out(sh), "</PRE></BODY></HTML>\n");
            }
            (void)shell_redirect(sh, NULL, false);
        }
        out_restore(out, &saved);
        free(query);
        if (ok) {
            launch_opener(sh, path);
        }
        return ok;
    }

    /* No query: set up for whatever statement the user types next, the way
     * upstream's `.once -x`/`.once -w` do, and accept the launch-timing gap
     * documented above. */
    ok = shell_redirect(sh, path, true);
    if (!ok) {
        return false;
    }
    apply_output_mode(out, is_excel);
    if (!is_excel) {
        fprintf(shell_out(sh), "<!DOCTYPE html>\n<HTML><BODY><PRE>\n");
    }
    launch_opener(sh, path);
    return true;
}

bool import_cmd_excel(Shell *sh, int argc, char **argv)
{
    return excel_or_www(sh, argc, argv, "excel", "csv", true);
}

bool import_cmd_www(Shell *sh, int argc, char **argv)
{
    return excel_or_www(sh, argc, argv, "www", "html", false);
}
