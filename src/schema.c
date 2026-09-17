/*
 * schema.c - the dot commands that render the database as text.
 *
 * These commands have a stricter contract than the rest: their output is
 * fed back into SQLite, or diffed, or pasted into a bug report, so it has to
 * match sqlite3(1) byte for byte. Everything here is therefore written
 * against what the 3.53.3 binary actually prints, measured rather than
 * assumed, and the transformations that upstream implements as SQL functions
 * (shell_add_schema, shell_format_schema) are ported here as plain string
 * functions so that the same text comes out.
 */
#include "schema.h"

#include "db.h"
#include "out.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --------------------------------------------------------------------------
 * A growable string
 * ------------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

static bool str_add(Str *s, const char *text, size_t n)
{
    if (s->len + n + 1u > s->cap) {
        size_t cap = s->cap != 0u ? s->cap : 64u;
        char *grown;

        while (cap < s->len + n + 1u) {
            cap *= 2u;
        }
        grown = realloc(s->data, cap);
        if (grown == NULL) {
            return false;
        }
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
    return true;
}

static bool str_puts(Str *s, const char *text)
{
    return str_add(s, text, strlen(text));
}

static bool str_putc(Str *s, char c)
{
    return str_add(s, &c, 1u);
}

static void str_free(Str *s)
{
    free(s->data);
    s->data = NULL;
    s->len = 0u;
    s->cap = 0u;
}

static const char *str_text(const Str *s)
{
    return s->data != NULL ? s->data : "";
}

static char *dup_str(const char *text)
{
    size_t n = strlen(text) + 1u;
    char *copy = malloc(n);

    if (copy != NULL) {
        memcpy(copy, text, n);
    }
    return copy;
}

/* --------------------------------------------------------------------------
 * Identifier quoting
 * ------------------------------------------------------------------------ */

static bool is_alpha(char c)
{
    return isalpha((unsigned char)c) != 0 || c == '_';
}

static bool is_alnum(char c)
{
    return isalnum((unsigned char)c) != 0 || c == '_';
}

static bool is_space(char c)
{
    return isspace((unsigned char)c) != 0;
}

/* '"' when NAME has to be quoted to survive reparsing, 0 when it does not.
 * Upstream's quoteChar, including its keyword check. */
static char quote_char(const char *name)
{
    size_t i;

    if (name == NULL) {
        return '"';
    }
    if (!is_alpha(name[0])) {
        return '"';
    }
    for (i = 0u; name[i] != '\0'; i++) {
        if (!is_alnum(name[i])) {
            return '"';
        }
    }
    return db_is_keyword(name) ? '"' : '\0';
}

/* Append TEXT, quoted with QUOTE if QUOTE is not 0. An embedded quote
 * character is doubled, which is what SQLite's own %w does. */
static bool append_quoted(Str *s, const char *text, char quote)
{
    size_t i;

    if (quote == '\0') {
        return str_puts(s, text);
    }
    if (!str_putc(s, quote)) {
        return false;
    }
    for (i = 0u; text[i] != '\0'; i++) {
        if (text[i] == quote && !str_putc(s, quote)) {
            return false;
        }
        if (!str_putc(s, text[i])) {
            return false;
        }
    }
    return str_putc(s, (char)(quote == '[' ? ']' : quote));
}

/* Run SQL with the output formatter temporarily in insert mode and headers
 * off, restoring both afterwards. .dump and .fullschema both need INSERT
 * statements, and the quoting of text and blobs as SQL literals already
 * lives in out.c's insert mode. */
static bool exec_insert_mode(Shell *sh, const char *sql, const char *table)
{
    Out *out = db_out(shell_db(sh));
    bool saved_headers = out_headers(out);
    char *saved_mode = dup_str(out_mode_name(out));
    bool ok;

    (void)out_set_mode(out, "insert");
    out_set_table_name(out, table);
    out_set_headers(out, false);
    ok = db_exec(shell_db(sh), sql, shell_out(sh), shell_err(sh));
    if (saved_mode != NULL) {
        (void)out_set_mode(out, saved_mode);
        free(saved_mode);
    }
    out_set_headers(out, saved_headers);
    return ok;
}

/* --------------------------------------------------------------------------
 * Split-column layout
 *
 * `.tables` and `.indexes` print one column of names packed side by side.
 * Upstream picks the widest layout that fits an 80-column screen, filling
 * down each column before moving right, then spreads the leftover space
 * between columns up to a five-space maximum. Reproduced exactly, because a
 * different arrangement is a visible difference in the commonest command in
 * the shell.
 * ------------------------------------------------------------------------ */

#define SPLIT_SCREEN 80u

/* Widths of NCOL columns under the column-major fill, or NULL when the
 * layout does not fit. */
static size_t *split_layout(char *const *item, size_t n, size_t ncol)
{
    size_t *width = calloc(ncol, sizeof(*width));
    size_t nrow = (n + ncol - 1u) / ncol;
    size_t total = 2u * (ncol - 1u);
    size_t i;

    if (width == NULL || item == NULL) {
        free(width);
        return NULL;
    }
    for (i = 0u; i < n; i++) {
        size_t col = i / nrow;
        size_t len = strlen(item[i]);

        if (col < ncol && len > width[col]) {
            width[col] = len;
        }
    }
    for (i = 0u; i < ncol; i++) {
        total += width[i];
    }
    if (total > SPLIT_SCREEN) {
        free(width);
        return NULL;
    }
    return width;
}

static void split_print(FILE *dest, char *const *item, size_t n)
{
    size_t *width = NULL;
    size_t ncol = 1u;
    size_t nrow = n;
    size_t next = 2u;
    size_t margin;
    size_t used = 0u;
    size_t row;
    size_t i;

    if (n == 0u || item == NULL) {
        return;
    }
    while (next <= n) {
        size_t *candidate = split_layout(item, n, next);

        if (candidate == NULL) {
            break;
        }
        free(width);
        width = candidate;
        ncol = next;
        nrow = (n + ncol - 1u) / ncol;
        if (nrow == 1u) {
            break;
        }
        next++;
        while (next <= n && (n + next - 1u) / next == nrow) {
            next++;
        }
    }
    if (ncol == 1u || width == NULL) {
        free(width);
        for (i = 0u; i < n; i++) {
            fprintf(dest, "%s\n", item[i]);
        }
        return;
    }

    for (i = 0u; i < ncol; i++) {
        used += width[i];
    }
    margin = (SPLIT_SCREEN - used) / (ncol - 1u);
    if (margin > 5u) {
        margin = 5u;
    }
    for (row = 0u; row < nrow; row++) {
        size_t col;

        for (col = 0u; col < ncol; col++) {
            size_t idx = (col * nrow) + row;
            const char *text = idx < n ? item[idx] : "";
            bool last = col + 1u == ncol;

            fputs(text, dest);
            if (!last) {
                size_t pad = width[col] - strlen(text) + margin;

                while (pad-- > 0u) {
                    fputc(' ', dest);
                }
            }
        }
        fputc('\n', dest);
    }
    free(width);
}

/* Collect the first column of every row of SQL. Returns NULL on failure. */
static char **collect(Shell *sh, const char *sql, size_t *count)
{
    DbStmt *stmt = db_prepare(shell_db(sh), sql, shell_err(sh));
    char **item = NULL;
    size_t n = 0u;
    size_t cap = 0u;

    *count = 0u;
    if (stmt == NULL) {
        return NULL;
    }
    while (db_step(stmt)) {
        const char *text = db_stmt_text(stmt, 0);

        if (text == NULL) {
            continue;
        }
        if (n == cap) {
            char **grown;

            cap = cap != 0u ? cap * 2u : 16u;
            grown = realloc(item, cap * sizeof(*item));
            if (grown == NULL) {
                break;
            }
            item = grown;
        }
        item[n] = dup_str(text);
        if (item[n] == NULL) {
            break;
        }
        n++;
    }
    (void)db_finalize(stmt, shell_err(sh));
    *count = n;
    return item;
}

static void collect_free(char **item, size_t n)
{
    size_t i;

    if (item == NULL) {
        return;
    }
    for (i = 0u; i < n; i++) {
        free(item[i]);
    }
    free(item);
}

/* --------------------------------------------------------------------------
 * Schema text transformations
 * ------------------------------------------------------------------------ */

/* "name(col,col,...)" for a view or virtual table, the comment `.schema`
 * appends so that the reader can see the column names a CREATE VIEW does not
 * spell out. Upstream's shellFakeSchema. */
static char *fake_schema(Shell *sh, const char *dbname, const char *name)
{
    Str sql = {NULL, 0u, 0u};
    Str out = {NULL, 0u, 0u};
    DbStmt *stmt;
    const char *div = "(";
    size_t nrow = 0u;

    (void)str_puts(&sql, "PRAGMA ");
    (void)append_quoted(&sql, dbname != NULL ? dbname : "main", '"');
    (void)str_puts(&sql, ".table_info=");
    (void)append_quoted(&sql, name, '\'');
    stmt = db_prepare(shell_db(sh), str_text(&sql), NULL);
    str_free(&sql);
    if (stmt == NULL) {
        return NULL;
    }
    if (dbname != NULL && strcmp(dbname, "temp") != 0) {
        (void)append_quoted(&out, dbname, quote_char(dbname));
        (void)str_puts(&out, ".");
    }
    (void)append_quoted(&out, name, quote_char(name));
    while (db_step(stmt)) {
        const char *col = db_stmt_text(stmt, 1);

        nrow++;
        (void)str_puts(&out, div);
        div = ",";
        (void)append_quoted(&out, col != NULL ? col : "", quote_char(col));
    }
    (void)str_puts(&out, ")");
    (void)db_finalize(stmt, NULL);
    if (nrow == 0u) {
        str_free(&out);
        return NULL;
    }
    return out.data;
}

/* Insert the schema name into a CREATE statement, and append the fake column
 * list for a view. Upstream's shellAddSchemaName. Returns a fresh string, or
 * NULL to mean "use SQL unchanged". */
static char *add_schema_name(Shell *sh, const char *sql, const char *dbname, const char *name)
{
    static const char *const prefix[] = {"TABLE", "INDEX",   "UNIQUE INDEX",
                                         "VIEW",  "TRIGGER", "VIRTUAL TABLE"};
    size_t i;

    if (sql == NULL || strncmp(sql, "CREATE ", 7u) != 0) {
        return NULL;
    }
    for (i = 0u; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
        size_t n = strlen(prefix[i]);
        Str out = {NULL, 0u, 0u};
        char *fake = NULL;
        bool changed = false;

        if (strncmp(sql + 7, prefix[i], n) != 0 || sql[n + 7u] != ' ') {
            continue;
        }
        if (dbname != NULL) {
            char quote = quote_char(dbname);

            (void)str_add(&out, sql, n + 7u);
            (void)str_putc(&out, ' ');
            (void)append_quoted(&out, dbname, (char)(strcmp(dbname, "temp") == 0 ? '\0' : quote));
            (void)str_putc(&out, '.');
            (void)str_puts(&out, sql + n + 8u);
            changed = true;
        }
        if (name != NULL && prefix[i][0] == 'V') {
            fake = fake_schema(sh, dbname, name);
        }
        if (fake != NULL) {
            if (!changed) {
                (void)str_puts(&out, sql);
            }
            (void)str_puts(&out, "\n/* ");
            (void)str_puts(&out, fake);
            (void)str_puts(&out, " */");
            free(fake);
            changed = true;
        }
        if (changed) {
            return out.data;
        }
        str_free(&out);
    }
    return NULL;
}

/* True when everything from TEXT to the end of the line is whitespace. */
static bool ws_to_eol(const char *text)
{
    size_t i;

    for (i = 0u; text[i] != '\0'; i++) {
        if (text[i] == '\n') {
            return true;
        }
        if (!is_space(text[i])) {
            return false;
        }
    }
    return true;
}

/* Upstream's shellFormatSchema: normalise whitespace and break a long
 * CREATE TABLE across lines. INDENT false just appends the semicolon. */
static char *format_schema(const char *sql, bool indent)
{
    Str out = {NULL, 0u, 0u};
    char *z;
    size_t i;
    size_t j;
    int nparen = 0;
    char cend = '\0';
    int nline = 0;
    bool is_index;
    bool is_where = false;

    if (sql == NULL || sql[0] == '\0') {
        return dup_str("");
    }
    if (!indent) {
        (void)str_puts(&out, sql);
        (void)str_putc(&out, ';');
        return out.data;
    }
    /* A view's or trigger's body is the author's own layout; reflowing it
     * would change the SQL inside, so upstream leaves both alone. */
    if (strncasecmp(sql, "CREATE VIEW", 11) == 0 || strncasecmp(sql, "CREATE TRIG", 11) == 0) {
        (void)str_puts(&out, sql);
        (void)str_putc(&out, ';');
        return out.data;
    }
    is_index = strncasecmp(sql, "CREATE INDEX", 12) == 0 ||
               strncasecmp(sql, "CREATE UNIQUE INDEX", 19) == 0;
    z = dup_str(sql);
    if (z == NULL) {
        return NULL;
    }

    j = 0u;
    for (i = 0u; is_space(z[i]); i++) {
        /* leading space */
    }
    for (; z[i] != '\0'; i++) {
        char c = z[i];

        if (is_space(c)) {
            if (j > 0u && z[j - 1u] == '\r') {
                z[j - 1u] = '\n';
            }
            if (j > 0u && (is_space(z[j - 1u]) || z[j - 1u] == '(')) {
                continue;
            }
        } else if ((c == '(' || c == ')') && j > 0u && is_space(z[j - 1u])) {
            j--;
        }
        z[j++] = c;
    }
    while (j > 0u && is_space(z[j - 1u])) {
        j--;
    }
    z[j] = '\0';

    if (strlen(z) < 79u) {
        (void)str_puts(&out, z);
        (void)str_putc(&out, ';');
        free(z);
        return out.data;
    }

    for (i = j = 0u; z[i] != '\0'; i++) {
        char c = z[i];

        if (c == cend) {
            cend = '\0';
        } else if (cend != '\0') {
            /* inside a quoted string or a comment */
        } else if (c == '"' || c == '\'' || c == '`') {
            cend = c;
        } else if (c == '[') {
            cend = ']';
        } else if (c == '-' && z[i + 1u] == '-') {
            cend = '\n';
        } else if (c == '(') {
            nparen++;
        } else if (c == ')') {
            nparen--;
            if (nline > 0 && nparen == 0 && j > 0u && !is_where) {
                (void)str_add(&out, z, j);
                (void)str_putc(&out, '\n');
                j = 0u;
            }
        } else if ((c == 'w' || c == 'W') && nparen == 0 && is_index &&
                   strncasecmp("WHERE", &z[i], 5) == 0 && !is_alnum(z[i + 5u])) {
            is_where = true;
        } else if (is_where && (c == 'a' || c == 'A') && nparen == 0 &&
                   strncasecmp("AND", &z[i], 3) == 0 && !is_alnum(z[i + 3u])) {
            (void)str_add(&out, z, j);
            (void)str_puts(&out, "\n    ");
            j = 0u;
        }
        z[j++] = c;
        if (nparen == 1 && cend == '\0' &&
            (c == '(' || c == '\n' || (c == ',' && !ws_to_eol(z + i + 1u))) && !is_where) {
            if (c == '\n') {
                j--;
            }
            (void)str_add(&out, z, j);
            (void)str_puts(&out, "\n  ");
            j = 0u;
            nline++;
            while (is_space(z[i + 1u])) {
                i++;
            }
        }
    }
    z[j] = '\0';
    (void)str_puts(&out, z);
    (void)str_putc(&out, ';');
    free(z);
    return out.data;
}

/* Emit one statement, turning "CREATE TABLE 'x'" into
 * "CREATE TABLE IF NOT EXISTS 'x'". Upstream rewrites only when the name is
 * quoted, which is exactly the case where the schema was generated rather
 * than typed -- .import's auto-created tables above all -- so a dump of one
 * of those replays into a database that already has it. */
static void emit_schema_line(FILE *dest, const char *sql, const char *tail)
{
    static const char prefix[] = "CREATE TABLE ";
    size_t n = sizeof(prefix) - 1u;

    if (strncmp(sql, prefix, n) == 0 && (sql[n] == '\'' || sql[n] == '"')) {
        fprintf(dest, "CREATE TABLE IF NOT EXISTS %s%s", sql + n, tail);
    } else {
        fprintf(dest, "%s%s", sql, tail);
    }
}

/* Print one schema line, making sure the trailing semicolon does not land
 * inside a comment the statement ends with. Upstream's printSchemaLine. */
static void print_schema_line(FILE *dest, const char *sql, const char *tail)
{
    static const char *const term[] = {"", "*/", "\n"};

    if (sql == NULL || tail == NULL) {
        return;
    }
    if (tail[0] == ';' && (strstr(sql, "/*") != NULL || strstr(sql, "--") != NULL)) {
        size_t i;

        for (i = 0u; i < sizeof(term) / sizeof(term[0]); i++) {
            Str probe = {NULL, 0u, 0u};

            (void)str_puts(&probe, sql);
            (void)str_puts(&probe, term[i]);
            (void)str_putc(&probe, ';');
            if (db_is_complete(str_text(&probe))) {
                probe.data[probe.len - 1u] = '\0';
                emit_schema_line(dest, str_text(&probe), tail);
                str_free(&probe);
                return;
            }
            str_free(&probe);
        }
    }
    emit_schema_line(dest, sql, tail);
}

/* --------------------------------------------------------------------------
 * .schema and .fullschema
 * ------------------------------------------------------------------------ */

/* Build "SELECT sql, type, tbl_name, name FROM <db>.sqlite_schema" over every
 * attached database, in database_list order. */
static bool schema_query(Shell *sh, Str *sql, const char *filter)
{
    DbStmt *stmt = db_prepare(shell_db(sh), "PRAGMA database_list", shell_err(sh));
    const char *div = "";

    if (stmt == NULL) {
        return false;
    }
    while (db_step(stmt)) {
        const char *dbname = db_stmt_text(stmt, 1);

        if (dbname == NULL) {
            continue;
        }
        (void)str_puts(sql, div);
        div = " UNION ALL ";
        (void)str_puts(sql, "SELECT sql, type, tbl_name, name, rowid, ");
        (void)append_quoted(sql, dbname, '\'');
        (void)str_puts(sql, " AS sname FROM ");
        (void)append_quoted(sql, dbname, '"');
        (void)str_puts(sql, ".sqlite_schema");
    }
    (void)db_finalize(stmt, shell_err(sh));
    if (filter != NULL) {
        (void)str_puts(sql, " ");
    }
    return true;
}

/* The schema table has no row describing itself, so .schema prints a
 * hand-written definition for it, as sqlite3(1) does. Returns true when the
 * pattern named it and the block has been printed. */
static bool schema_builtin(Shell *sh, const char *pattern)
{
    static const char *const body = " (\n"
                                    "  type text,\n"
                                    "  name text,\n"
                                    "  tbl_name text,\n"
                                    "  rootpage integer,\n"
                                    "  sql text\n"
                                    ");\n";
    bool temp;

    if (pattern == NULL) {
        return false;
    }
    temp = strcmp(pattern, "sqlite_temp_schema") == 0 || strcmp(pattern, "sqlite_temp_master") == 0;
    if (!temp && strcmp(pattern, "sqlite_schema") != 0 && strcmp(pattern, "sqlite_master") != 0) {
        return false;
    }
    fprintf(shell_out(sh), "CREATE TABLE %ssqlite_schema%s", temp ? "temp." : "", body);
    return true;
}

static bool schema_print(Shell *sh, const char *pattern, bool indent, bool nosys)
{
    Str sql = {NULL, 0u, 0u};
    Str inner = {NULL, 0u, 0u};
    DbStmt *stmt;
    FILE *dest = shell_out(sh);

    if (schema_builtin(sh, pattern)) {
        return true;
    }
    if (!schema_query(sh, &inner, NULL)) {
        str_free(&inner);
        return false;
    }
    (void)str_puts(&sql, "SELECT sql, type, tbl_name, name, sname FROM (");
    (void)str_puts(&sql, str_text(&inner));
    (void)str_puts(&sql, ") WHERE ");
    str_free(&inner);
    if (pattern != NULL) {
        bool glob = strchr(pattern, '*') != NULL || strchr(pattern, '?') != NULL ||
                    strchr(pattern, '[') != NULL;

        if (strchr(pattern, '.') != NULL) {
            (void)str_puts(&sql, "lower(sname||'.'||tbl_name)");
        } else {
            (void)str_puts(&sql, "lower(tbl_name)");
        }
        (void)str_puts(&sql, glob ? " GLOB " : " LIKE ");
        (void)append_quoted(&sql, pattern, '\'');
        (void)str_puts(&sql, glob ? " AND " : " ESCAPE '\\' AND ");
    }
    if (nosys) {
        (void)str_puts(&sql, "name NOT LIKE 'sqlite__%' ESCAPE '_' AND ");
    }
    (void)str_puts(&sql, "sql IS NOT NULL ORDER BY rowid");

    stmt = db_prepare(shell_db(sh), str_text(&sql), shell_err(sh));
    str_free(&sql);
    if (stmt == NULL) {
        return false;
    }
    while (db_step(stmt)) {
        const char *text = db_stmt_text(stmt, 0);
        const char *name = db_stmt_text(stmt, 3);
        const char *sname = db_stmt_text(stmt, 4);
        char *named;
        char *formatted;

        named = add_schema_name(sh, text,
                                (sname != NULL && strcmp(sname, "main") != 0) ? sname : NULL, name);
        formatted = format_schema(named != NULL ? named : text, indent);
        if (formatted != NULL) {
            fprintf(dest, "%s\n", formatted);
        }
        free(formatted);
        free(named);
    }
    return db_finalize(stmt, shell_err(sh));
}

bool schema_cmd_schema(Shell *sh, int argc, char **argv)
{
    const char *pattern = NULL;
    bool indent = false;
    bool nosys = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--indent") == 0 || strcmp(argv[i], "-indent") == 0) {
            indent = true;
        } else if (strcmp(argv[i], "--nosys") == 0 || strcmp(argv[i], "-nosys") == 0) {
            nosys = true;
        } else if (argv[i][0] == '-') {
            fprintf(shell_err(sh), "Unknown option: \"%s\"\n", argv[i]);
            return false;
        } else if (pattern == NULL) {
            pattern = argv[i];
        } else {
            fputs("Usage: .schema ?--indent? ?--nosys? ?LIKE-PATTERN?\n", shell_err(sh));
            return false;
        }
    }
    return schema_print(sh, pattern, indent, nosys);
}

bool schema_cmd_fullschema(Shell *sh, int argc, char **argv)
{
    /* Upstream's .fullschema prints the raw `sql` column: unlike .schema it
     * does not qualify names with their schema or append a view's column
     * list, and although it accepts --indent it passes no flags to the
     * formatter, so the option has no effect. Matched deliberately. */
    static const char *const sql = "SELECT sql FROM"
                                   "  (SELECT sql sql, type type, name name, rowid x"
                                   "     FROM sqlite_schema UNION ALL"
                                   "   SELECT sql, type, name, rowid FROM sqlite_temp_schema)"
                                   " WHERE type!='meta' AND sql NOTNULL"
                                   "  AND name NOT LIKE 'sqlite__%' ESCAPE '_'"
                                   " ORDER BY x";
    DbStmt *stmt;
    bool stat1 = false;
    bool stat4 = false;

    if (argc == 2 && (strcmp(argv[1], "--indent") == 0 || strcmp(argv[1], "-indent") == 0)) {
        argc = 1;
    }
    if (argc != 1) {
        fputs("Usage: .fullschema ?--indent?\n", shell_err(sh));
        return false;
    }
    stmt = db_prepare(shell_db(sh), sql, shell_err(sh));
    if (stmt == NULL) {
        return false;
    }
    while (db_step(stmt)) {
        char *text = format_schema(db_stmt_text(stmt, 0), false);

        if (text != NULL) {
            fprintf(shell_out(sh), "%s\n", text);
            free(text);
        }
    }
    if (!db_finalize(stmt, shell_err(sh))) {
        return false;
    }

    stmt = db_prepare(shell_db(sh),
                      "SELECT substr(name,12,1) FROM sqlite_schema"
                      " WHERE name GLOB 'sqlite_stat[134]'",
                      NULL);
    if (stmt != NULL) {
        while (db_step(stmt)) {
            const char *k = db_stmt_text(stmt, 0);

            stat1 = stat1 || (k != NULL && k[0] == '1');
            stat4 = stat4 || (k != NULL && k[0] == '4');
        }
        (void)db_finalize(stmt, NULL);
    }
    if (!stat1 && !stat4) {
        fputs("/* No STAT tables available */\n", shell_out(sh));
        return true;
    }
    fputs("ANALYZE sqlite_schema;\n", shell_out(sh));
    if (stat1) {
        (void)exec_insert_mode(sh, "SELECT * FROM sqlite_stat1", "sqlite_stat1");
    }
    if (stat4) {
        (void)exec_insert_mode(sh, "SELECT * FROM sqlite_stat4", "sqlite_stat4");
    }
    fputs("ANALYZE sqlite_schema;\n", shell_out(sh));
    return true;
}

/* --------------------------------------------------------------------------
 * .tables and .indexes
 * ------------------------------------------------------------------------ */

/* Both commands list names from every attached schema, qualifying the ones
 * that are not in main, and both print through the split layout. */
static bool list_names(Shell *sh, const char *what, const char *pattern, const char *extra)
{
    Str sql = {NULL, 0u, 0u};
    DbStmt *stmt = db_prepare(shell_db(sh), "PRAGMA database_list", shell_err(sh));
    const char *div = "";
    char **item;
    size_t n;

    if (stmt == NULL) {
        return false;
    }
    while (db_step(stmt)) {
        const char *dbname = db_stmt_text(stmt, 1);

        if (dbname == NULL) {
            continue;
        }
        (void)str_puts(&sql, div);
        div = " UNION ALL ";
        if (strcmp(dbname, "main") == 0) {
            (void)str_puts(&sql, "SELECT name FROM ");
        } else {
            (void)str_puts(&sql, "SELECT ");
            (void)append_quoted(&sql, dbname, '\'');
            (void)str_puts(&sql, "||'.'||name FROM ");
        }
        (void)append_quoted(&sql, dbname, '"');
        (void)str_puts(&sql, ".sqlite_schema WHERE ");
        (void)str_puts(&sql, what);
        (void)str_puts(&sql, " AND name NOT LIKE 'sqlite__%' ESCAPE '_'");
        if (extra != NULL) {
            (void)str_puts(&sql, extra);
        }
        if (pattern != NULL) {
            (void)str_puts(&sql, " AND ");
            (void)str_puts(&sql, extra != NULL ? "tbl_name" : "name");
            (void)str_puts(&sql, " LIKE ");
            (void)append_quoted(&sql, pattern, '\'');
        }
    }
    (void)db_finalize(stmt, shell_err(sh));
    (void)str_puts(&sql, " ORDER BY 1");

    item = collect(sh, str_text(&sql), &n);
    str_free(&sql);
    if (item == NULL && n == 0u) {
        return true;
    }
    split_print(shell_out(sh), item, n);
    collect_free(item, n);
    return true;
}

bool schema_cmd_tables(Shell *sh, int argc, char **argv)
{
    return list_names(sh, "type IN ('table','view')", argc > 1 ? argv[1] : NULL, NULL);
}

bool schema_cmd_indexes(Shell *sh, int argc, char **argv)
{
    return list_names(sh, "type='index'", argc > 1 ? argv[1] : NULL, "");
}

/* --------------------------------------------------------------------------
 * .databases
 * ------------------------------------------------------------------------ */

bool schema_cmd_databases(Shell *sh, int argc, char **argv)
{
    DbStmt *stmt = db_prepare(shell_db(sh), "PRAGMA database_list", shell_err(sh));
    char **name;
    char **file;
    size_t n = 0u;
    size_t cap = 8u;
    size_t i;

    (void)argc;
    (void)argv;
    if (stmt == NULL) {
        return false;
    }
    name = malloc(cap * sizeof(*name));
    file = malloc(cap * sizeof(*file));
    if (name == NULL || file == NULL) {
        free(name);
        free(file);
        (void)db_finalize(stmt, NULL);
        return false;
    }
    /* The names are copied out before anything is printed: printing runs
     * arbitrary file-controls, and stepping a pragma while doing so is not
     * something to rely on. */
    while (db_step(stmt) && n < cap) {
        const char *schema = db_stmt_text(stmt, 1);
        const char *path = db_stmt_text(stmt, 2);

        if (schema == NULL || path == NULL) {
            continue;
        }
        name[n] = dup_str(schema);
        file[n] = dup_str(path);
        if (name[n] == NULL || file[n] == NULL) {
            break;
        }
        n++;
    }
    (void)db_finalize(stmt, shell_err(sh));

    for (i = 0u; i < n; i++) {
        int txn = db_txn_state(shell_db(sh), name[i]);

        static const char *const state[] = {"", " read-txn", " write-txn"};

        fprintf(shell_out(sh), "%s: %s %s%s\n", name[i], file[i][0] != '\0' ? file[i] : "\"\"",
                db_schema_readonly(shell_db(sh), name[i]) ? "r/o" : "r/w",
                txn >= 0 && txn <= 2 ? state[txn] : "");
        free(name[i]);
        free(file[i]);
    }
    free(name);
    free(file);
    return true;
}

/* --------------------------------------------------------------------------
 * .dump
 * ------------------------------------------------------------------------ */

/* The column list for an INSERT, quoted where a name needs it. */
static bool dump_columns(Shell *sh, const char *table, Str *cols)
{
    Str sql = {NULL, 0u, 0u};
    DbStmt *stmt;
    const char *div = "";

    (void)str_puts(&sql, "PRAGMA table_info=");
    (void)append_quoted(&sql, table, '\'');
    stmt = db_prepare(shell_db(sh), str_text(&sql), shell_err(sh));
    str_free(&sql);
    if (stmt == NULL) {
        return false;
    }
    while (db_step(stmt)) {
        const char *col = db_stmt_text(stmt, 1);

        (void)str_puts(cols, div);
        div = ",";
        (void)append_quoted(cols, col != NULL ? col : "", quote_char(col));
    }
    return db_finalize(stmt, shell_err(sh));
}

/* Emit the INSERT statements for one table, by running a SELECT through the
 * formatter in insert mode -- which is where the SQL-literal quoting of text
 * and blobs already lives. */
static bool dump_rows(Shell *sh, const char *table)
{
    Str select = {NULL, 0u, 0u};
    Str cols = {NULL, 0u, 0u};
    bool ok;

    if (!dump_columns(sh, table, &cols)) {
        str_free(&cols);
        return false;
    }
    (void)str_puts(&select, "SELECT ");
    (void)str_puts(&select, str_text(&cols));
    (void)str_puts(&select, " FROM ");
    (void)append_quoted(&select, table, quote_char(table));
    str_free(&cols);
    ok = exec_insert_mode(sh, str_text(&select), table);
    str_free(&select);
    return ok;
}

bool schema_cmd_dump(Shell *sh, int argc, char **argv)
{
    Str like = {NULL, 0u, 0u};
    Str sql = {NULL, 0u, 0u};
    FILE *dest = shell_out(sh);
    DbStmt *stmt;
    bool data_only = false;
    bool nosys = false;
    bool failed = false;
    int i;

    for (i = 1; i < argc; i++) {
        const char *z = argv[i];

        if (z[0] == '-') {
            z += z[1] == '-' ? 2 : 1;
            if (strcmp(z, "data-only") == 0) {
                data_only = true;
            } else if (strcmp(z, "nosys") == 0) {
                nosys = true;
            } else if (strcmp(z, "preserve-rowids") != 0 && strcmp(z, "newlines") != 0) {
                fprintf(shell_err(sh), "Unknown option \"%s\" on \".dump\"\n", argv[i]);
                return false;
            }
        } else {
            (void)str_puts(&like, like.len != 0u ? " OR name LIKE " : "name LIKE ");
            (void)append_quoted(&like, z, '\'');
            (void)str_puts(&like, " ESCAPE '\\'");
        }
    }
    if (like.len == 0u) {
        (void)str_puts(&like, "true");
    }

    if (!data_only) {
        fputs("PRAGMA foreign_keys=OFF;\n", dest);
        fputs("BEGIN TRANSACTION;\n", dest);
    }

    (void)str_puts(&sql, "SELECT name, type, sql FROM sqlite_schema AS o WHERE (");
    (void)str_puts(&sql, str_text(&like));
    (void)str_puts(&sql, ") AND type=='table' AND sql NOT NULL"
                         " ORDER BY tbl_name='sqlite_sequence', rowid");
    stmt = db_prepare(shell_db(sh), str_text(&sql), shell_err(sh));
    str_free(&sql);
    if (stmt == NULL) {
        str_free(&like);
        return false;
    }
    while (db_step(stmt)) {
        const char *table = db_stmt_text(stmt, 0);
        const char *type = db_stmt_text(stmt, 1);
        const char *text = db_stmt_text(stmt, 2);

        if (table == NULL || type == NULL) {
            continue;
        }
        if (strncmp(table, "sqlite_", 7u) == 0) {
            if (nosys || strncmp(table, "sqlite_stat", 11u) != 0) {
                continue;
            }
            if (!data_only) {
                fputs("ANALYZE sqlite_schema;\n", dest);
            }
        } else if (!data_only) {
            print_schema_line(dest, text, ";\n");
        }
        if (strcmp(type, "table") == 0 && !dump_rows(sh, table)) {
            failed = true;
        }
    }
    (void)db_finalize(stmt, shell_err(sh));

    if (!data_only) {
        (void)str_puts(&sql, "SELECT sql FROM sqlite_schema AS o WHERE (");
        (void)str_puts(&sql, str_text(&like));
        (void)str_puts(&sql, ") AND sql NOT NULL AND type IN ('index','trigger','view')"
                             " ORDER BY type COLLATE NOCASE DESC");
        stmt = db_prepare(shell_db(sh), str_text(&sql), shell_err(sh));
        str_free(&sql);
        if (stmt != NULL) {
            while (db_step(stmt)) {
                print_schema_line(dest, db_stmt_text(stmt, 0), ";\n");
            }
            (void)db_finalize(stmt, shell_err(sh));
        }
        fputs(failed ? "ROLLBACK; -- due to errors\n" : "COMMIT;\n", dest);
    }
    str_free(&like);
    return !failed;
}

/* --------------------------------------------------------------------------
 * .dbinfo and .dbtotxt
 *
 * Both read the database file directly rather than through the
 * sqlite_dbpage virtual table, which is an optional build feature: the file
 * is the same either way, and this works against any libsqlite3.
 * ------------------------------------------------------------------------ */

static unsigned get2(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static unsigned get4(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* Open the file backing DBNAME, complaining the way upstream does. */
static FILE *open_db_file(Shell *sh, const char *dbname, long *size)
{
    const char *path = db_filename(shell_db(sh), dbname);
    FILE *fp;

    if (path == NULL || path[0] == '\0') {
        fputs("unable to read database header\n", shell_err(sh));
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(shell_err(sh), "cannot open file: %s\n", path);
        return NULL;
    }
    if (size != NULL) {
        if (fseek(fp, 0L, SEEK_END) != 0) {
            (void)fclose(fp);
            return NULL;
        }
        *size = ftell(fp);
        if (fseek(fp, 0L, SEEK_SET) != 0) {
            (void)fclose(fp);
            return NULL;
        }
    }
    return fp;
}

bool schema_cmd_dbinfo(Shell *sh, int argc, char **argv)
{
    /* clang-format off */
    /* Header fields, in the order sqlite3(1) prints them -- which is not the
     * order they appear in the file, so the table keeps the offsets. */
    static const struct { const char *name; size_t ofst; } field[] = {
        {"file change counter:", 24}, {"database page count:", 28},
        {"freelist page count:", 36}, {"schema cookie:",       40},
        {"schema format:",       44}, {"default cache size:",  48},
        {"autovacuum top root:", 52}, {"incremental vacuum:",  64},
        {"text encoding:",       56}, {"user version:",        60},
        {"application id:",      68}, {"software version:",    96},
    };
    static const struct { const char *name; const char *sql; } query[] = {
        {"number of tables:",   "SELECT count(*) FROM %s WHERE type='table'"},
        {"number of indexes:",  "SELECT count(*) FROM %s WHERE type='index'"},
        {"number of triggers:", "SELECT count(*) FROM %s WHERE type='trigger'"},
        {"number of views:",    "SELECT count(*) FROM %s WHERE type='view'"},
        {"schema size:",        "SELECT total(length(sql)) FROM %s"},
    };
    static const char *const encoding[] = {"", " (utf8)", " (utf16le)", " (utf16be)"};
    /* clang-format on */
    const char *dbname = argc >= 2 ? argv[1] : "main";
    FILE *dest = shell_out(sh);
    unsigned char hdr[100];
    Str tab = {NULL, 0u, 0u};
    FILE *fp;
    unsigned page;
    unsigned version = 0u;
    size_t i;

    fp = open_db_file(sh, dbname, NULL);
    if (fp == NULL) {
        return false;
    }
    if (fread(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
        fputs("unable to read database header\n", shell_err(sh));
        (void)fclose(fp);
        return false;
    }
    (void)fclose(fp);

    page = get2(hdr + 16);
    fprintf(dest, "%-20s %u\n", "database page size:", page == 1u ? 65536u : page);
    fprintf(dest, "%-20s %u\n", "write format:", hdr[18]);
    fprintf(dest, "%-20s %u\n", "read format:", hdr[19]);
    fprintf(dest, "%-20s %u\n", "reserved bytes:", hdr[20]);
    for (i = 0u; i < sizeof(field) / sizeof(field[0]); i++) {
        unsigned val = get4(hdr + field[i].ofst);

        fprintf(dest, "%-20s %u", field[i].name, val);
        if (field[i].ofst == 56u) {
            fputs(val < sizeof(encoding) / sizeof(encoding[0]) ? encoding[val] : "", dest);
        }
        fputc('\n', dest);
    }

    if (strcmp(dbname, "temp") == 0) {
        (void)str_puts(&tab, "sqlite_temp_schema");
    } else {
        (void)append_quoted(&tab, dbname, '"');
        (void)str_puts(&tab, ".sqlite_schema");
    }
    for (i = 0u; i < sizeof(query) / sizeof(query[0]); i++) {
        /* The %s in the upstream table is always at the end of the FROM
         * clause, so a split around it is not needed for the four counting
         * queries; the WHERE tail is appended after the table name. */
        const char *pct = strstr(query[i].sql, "%s");
        Str sql = {NULL, 0u, 0u};
        char *text;

        (void)str_add(&sql, query[i].sql, (size_t)(pct - query[i].sql));
        (void)str_puts(&sql, str_text(&tab));
        (void)str_puts(&sql, pct + 2);
        text = db_scalar(shell_db(sh), str_text(&sql));
        str_free(&sql);
        fprintf(dest, "%-20s %lld\n", query[i].name, text != NULL ? strtoll(text, NULL, 10) : 0);
        free(text);
    }
    str_free(&tab);
    (void)db_data_version(shell_db(sh), dbname, &version);
    fprintf(dest, "%-20s %u\n", "data version", version);
    return true;
}

bool schema_cmd_dbtotxt(Shell *sh, int argc, char **argv)
{
    const char *path = db_filename(shell_db(sh), "main");
    FILE *dest = shell_out(sh);
    unsigned char *page;
    const char *base;
    unsigned pagesize;
    long size = 0L;
    long pgno;
    FILE *fp;

    (void)argc;
    (void)argv;
    fp = open_db_file(sh, "main", &size);
    if (fp == NULL) {
        return false;
    }
    {
        unsigned char hdr[20];

        if (fread(hdr, 1u, sizeof(hdr), fp) != sizeof(hdr)) {
            fputs("unable to read database header\n", shell_err(sh));
            (void)fclose(fp);
            return false;
        }
        pagesize = get2(hdr + 16);
        if (pagesize == 1u) {
            pagesize = 65536u;
        }
    }
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    fprintf(dest, "| size %ld pagesize %u filename %s\n", size, pagesize, base);

    page = malloc(pagesize);
    if (page == NULL) {
        (void)fclose(fp);
        return false;
    }
    if (fseek(fp, 0L, SEEK_SET) != 0) {
        free(page);
        (void)fclose(fp);
        return false;
    }
    for (pgno = 1; (long)((size_t)pgno * pagesize) <= size; pgno++) {
        long offset = (pgno - 1) * (long)pagesize;
        unsigned i;

        if (fread(page, 1u, pagesize, fp) != pagesize) {
            break;
        }
        fprintf(dest, "| page %ld offset %ld\n", pgno, offset);
        for (i = 0u; i < pagesize; i += 16u) {
            char text[17];
            unsigned j;
            bool blank = true;

            for (j = 0u; j < 16u; j++) {
                if (page[i + j] != 0u) {
                    blank = false;
                }
            }
            if (blank) {
                continue;
            }
            fprintf(dest, "| %6u:", i);
            for (j = 0u; j < 16u; j++) {
                fprintf(dest, " %02x", page[i + j]);
                /* '"' and '\\' become dots too: the output is meant to be
                 * pasted into a C string literal. */
                text[j] = (char)((isprint(page[i + j]) && page[i + j] != '"' && page[i + j] != '\\')
                                     ? page[i + j]
                                     : '.');
            }
            text[16] = '\0';
            fprintf(dest, "   %s\n", text);
        }
    }
    free(page);
    (void)fclose(fp);
    fprintf(dest, "| end %s\n", base);
    return true;
}

/* --------------------------------------------------------------------------
 * .clone
 * ------------------------------------------------------------------------ */

bool schema_cmd_clone(Shell *sh, int argc, char **argv)
{
    Str sql = {NULL, 0u, 0u};
    char **table;
    size_t n;
    size_t i;
    bool ok = true;

    if (shell_unsafe(sh, "clone")) {
        return false;
    }
    if (argc < 2) {
        fputs("Usage: .clone FILENAME\n", shell_err(sh));
        return false;
    }
    (void)str_puts(&sql, "ATTACH ");
    (void)append_quoted(&sql, argv[1], '\'');
    (void)str_puts(&sql, " AS clone_target");
    if (!db_run(shell_db(sh), str_text(&sql), shell_err(sh))) {
        str_free(&sql);
        return false;
    }
    str_free(&sql);

    /* The schema first, so that every CREATE lands before any row does, then
     * the data. A failure anywhere still detaches. */
    table = collect(sh,
                    "SELECT sql FROM main.sqlite_schema WHERE sql NOT NULL"
                    " AND name NOT LIKE 'sqlite_%' ORDER BY rowid",
                    &n);
    for (i = 0u; i < n; i++) {
        char *named = add_schema_name(sh, table[i], "clone_target", NULL);

        if (named != NULL) {
            ok = db_run(shell_db(sh), named, shell_err(sh)) && ok;
            free(named);
        }
    }
    collect_free(table, n);

    table = collect(sh,
                    "SELECT name FROM main.sqlite_schema WHERE type='table'"
                    " AND name NOT LIKE 'sqlite_%' ORDER BY rowid",
                    &n);
    for (i = 0u; i < n; i++) {
        Str copy = {NULL, 0u, 0u};

        (void)str_puts(&copy, "INSERT INTO clone_target.");
        (void)append_quoted(&copy, table[i], quote_char(table[i]));
        (void)str_puts(&copy, " SELECT * FROM main.");
        (void)append_quoted(&copy, table[i], quote_char(table[i]));
        ok = db_run(shell_db(sh), str_text(&copy), shell_err(sh)) && ok;
        str_free(&copy);
    }
    collect_free(table, n);

    return db_run(shell_db(sh), "DETACH clone_target", shell_err(sh)) && ok;
}

/* --------------------------------------------------------------------------
 * .lint
 * ------------------------------------------------------------------------ */

/* The one check upstream ships: a foreign key whose child columns have no
 * usable index, which turns every parent-row delete or key update into a
 * table scan.
 *
 * Upstream builds the whole report in one SQL statement with a helper
 * function registered on the connection. That would mean handing a
 * sqlite3_context callback down from here, so the same thing is assembled in
 * C instead: one group per foreign key, then EXPLAIN QUERY PLAN for the
 * lookup the FK implementation would run, matched against the plan that says
 * an index was used. */

/* " COLLATE seq" when the parent column's collation differs from the child's,
 * because an index on the child has to name it to be usable. */
static void collate_clause(Shell *sh, const char *parent, const char *parent_col, const char *child,
                           const char *child_col, Str *out)
{
    char pseq[64];
    char cseq[64];

    if (!db_column_collation(shell_db(sh), parent, parent_col, pseq, sizeof(pseq)) ||
        !db_column_collation(shell_db(sh), child, child_col, cseq, sizeof(cseq))) {
        return;
    }
    if (strcasecmp(pseq, cseq) != 0) {
        (void)str_puts(out, " COLLATE ");
        (void)str_puts(out, pseq);
    }
}

/* The parent-side column of one foreign key entry: f.to, or, when the key
 * refers to the parent's primary key implicitly, the parent column whose pk
 * position is SEQ + 1. */
static char *parent_column(Shell *sh, const char *parent, const char *to, int seq)
{
    Str sql = {NULL, 0u, 0u};
    DbStmt *stmt;
    char *name = NULL;

    if (to != NULL) {
        return dup_str(to);
    }
    (void)str_puts(&sql, "SELECT name FROM pragma_table_info(");
    (void)append_quoted(&sql, parent, '\'');
    (void)str_puts(&sql, ") WHERE pk-1=");
    {
        char num[32];

        (void)snprintf(num, sizeof(num), "%d", seq);
        (void)str_puts(&sql, num);
    }
    stmt = db_prepare(shell_db(sh), str_text(&sql), NULL);
    str_free(&sql);
    if (stmt == NULL) {
        return NULL;
    }
    if (db_step(stmt)) {
        const char *text = db_stmt_text(stmt, 0);

        name = text != NULL ? dup_str(text) : NULL;
    }
    (void)db_finalize(stmt, NULL);
    return name;
}

/* Report on one foreign key of CHILD. */
static void lint_one_fkey(Shell *sh, const char *child, int fkid, bool verbose)
{
    static const char *const glob_ipk = "SEARCH * USING INTEGER PRIMARY KEY (rowid=?)";
    Str sql = {NULL, 0u, 0u};
    Str eqp = {NULL, 0u, 0u};
    Str glob = {NULL, 0u, 0u};
    Str from = {NULL, 0u, 0u};
    Str target = {NULL, 0u, 0u};
    Str ddl = {NULL, 0u, 0u};
    Str cols = {NULL, 0u, 0u};
    Str index_name = {NULL, 0u, 0u};
    char *parent = NULL;
    DbStmt *stmt;
    const char *div = "";
    char num[32];
    bool any = false;

    (void)snprintf(num, sizeof(num), "%d", fkid);
    (void)str_puts(&sql, "SELECT \"table\", \"from\", \"to\", seq FROM pragma_foreign_key_list(");
    (void)append_quoted(&sql, child, '\'');
    (void)str_puts(&sql, ") WHERE id=");
    (void)str_puts(&sql, num);
    (void)str_puts(&sql, " ORDER BY seq");
    stmt = db_prepare(shell_db(sh), str_text(&sql), shell_err(sh));
    str_free(&sql);
    if (stmt == NULL) {
        return;
    }

    (void)str_puts(&eqp, "EXPLAIN QUERY PLAN SELECT 1 FROM ");
    (void)append_quoted(&eqp, child, '\'');
    (void)str_puts(&eqp, " WHERE ");
    (void)str_puts(&glob, "SEARCH ");
    (void)str_puts(&glob, child);
    (void)str_puts(&glob, " USING COVERING INDEX*(");
    (void)str_puts(&index_name, child);

    while (db_step(stmt)) {
        const char *ptable = db_stmt_text(stmt, 0);
        const char *fcol = db_stmt_text(stmt, 1);
        const char *tcol = db_stmt_text(stmt, 2);
        const char *seq = db_stmt_text(stmt, 3);
        Str collate = {NULL, 0u, 0u};
        char *pcol;

        if (ptable == NULL || fcol == NULL) {
            continue;
        }
        if (parent == NULL) {
            parent = dup_str(ptable);
        }
        pcol = parent_column(sh, ptable, tcol, seq != NULL ? (int)strtol(seq, NULL, 10) : 0);
        collate_clause(sh, ptable, pcol != NULL ? pcol : "", child, fcol, &collate);

        (void)str_puts(&eqp, any ? " AND " : "");
        (void)append_quoted(&eqp, child, '\'');
        (void)str_puts(&eqp, ".");
        (void)append_quoted(&eqp, fcol, '\'');
        (void)str_puts(&eqp, "=?");
        (void)str_puts(&eqp, str_text(&collate));

        (void)str_puts(&glob, any ? " AND *=?" : "*=?");
        (void)str_puts(&from, any ? ", " : "");
        (void)str_puts(&from, fcol);
        (void)str_puts(&target, div);
        div = ",";
        (void)str_puts(&target, pcol != NULL ? pcol : "");

        (void)str_puts(&index_name, "_");
        (void)str_puts(&index_name, fcol);
        (void)str_puts(&cols, any ? ", " : "");
        (void)append_quoted(&cols, fcol, '\'');
        (void)str_puts(&cols, str_text(&collate));

        str_free(&collate);
        free(pcol);
        any = true;
    }
    (void)db_finalize(stmt, shell_err(sh));
    (void)str_puts(&glob, ")");

    if (any) {
        DbStmt *plan = db_prepare(shell_db(sh), str_text(&eqp), shell_err(sh));
        bool indexed = false;

        if (plan != NULL) {
            if (db_step(plan)) {
                const char *detail = db_stmt_text(plan, 3);

                indexed = db_glob(str_text(&glob), detail) || db_glob(glob_ipk, detail);
            }
            (void)db_finalize(plan, NULL);
        }
        if (!indexed) {
            (void)str_puts(&ddl, "CREATE INDEX ");
            (void)append_quoted(&ddl, str_text(&index_name), '\'');
            (void)str_puts(&ddl, " ON ");
            (void)append_quoted(&ddl, child, '\'');
            (void)str_puts(&ddl, "(");
            (void)str_puts(&ddl, str_text(&cols));
            (void)str_puts(&ddl, ");");
            fprintf(shell_out(sh), "%s --> %s(%s)\n", str_text(&ddl), parent, str_text(&target));
        } else if (verbose) {
            fprintf(shell_out(sh), "/* no extra indexes required for %s(%s) -> %s(%s) */\n", child,
                    str_text(&from), parent, str_text(&target));
        }
    }

    free(parent);
    str_free(&eqp);
    str_free(&glob);
    str_free(&from);
    str_free(&target);
    str_free(&ddl);
    str_free(&cols);
    str_free(&index_name);
}

static bool lint_fkey_indexes(Shell *sh, bool verbose)
{
    char **table;
    size_t n;
    size_t i;

    table = collect(sh, "SELECT name FROM sqlite_schema ORDER BY name", &n);
    for (i = 0u; i < n; i++) {
        Str sql = {NULL, 0u, 0u};
        DbStmt *stmt;

        (void)str_puts(&sql, "SELECT DISTINCT id FROM pragma_foreign_key_list(");
        (void)append_quoted(&sql, table[i], '\'');
        (void)str_puts(&sql, ") ORDER BY id");
        stmt = db_prepare(shell_db(sh), str_text(&sql), NULL);
        str_free(&sql);
        if (stmt == NULL) {
            continue;
        }
        while (db_step(stmt)) {
            const char *id = db_stmt_text(stmt, 0);

            lint_one_fkey(sh, table[i], id != NULL ? (int)strtol(id, NULL, 10) : 0, verbose);
        }
        (void)db_finalize(stmt, NULL);
    }
    collect_free(table, n);
    return true;
}

bool schema_cmd_lint(Shell *sh, int argc, char **argv)
{
    bool verbose = false;
    int i;

    if (argc < 2 || strcmp(argv[1], "fkey-indexes") != 0) {
        fputs("Usage .lint sub-command ?switches...?\n"
              "Where sub-commands are:\n"
              "    fkey-indexes\n",
              shell_err(sh));
        return false;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-verbose") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else {
            fprintf(shell_err(sh), "Usage: %s %s ?-verbose? ?-groupbyparent?\n", argv[0], argv[1]);
            return false;
        }
    }
    return lint_fkey_indexes(sh, verbose);
}
