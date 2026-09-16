/*
 * sqlsh - a minimal SQLite shell with zsh-style completion.
 *
 * Phase 0: argument handling and a plain REPL. The raw-mode line editor
 * (line.c) and the completion engine (sqlctx.c, comp.c, menu.c) replace
 * read_line below in later phases; see PROJECT.md.
 */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SQLSH_VERSION "0.1.0"

/* Growable line accumulator for multi-line statements. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

static void buffer_free(Buffer *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->len = 0u;
    buf->cap = 0u;
}

static void buffer_clear(Buffer *buf)
{
    buf->len = 0u;
    if (buf->data != NULL) {
        buf->data[0] = '\0';
    }
}

static bool buffer_append(Buffer *buf, const char *text)
{
    size_t add = strlen(text);

    if (buf->len + add + 1u > buf->cap) {
        size_t new_cap = buf->cap == 0u ? 256u : buf->cap;
        char *data;

        while (new_cap < buf->len + add + 1u) {
            new_cap *= 2u;
        }
        data = realloc(buf->data, new_cap);
        if (data == NULL) {
            return false;
        }
        buf->data = data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, text, add + 1u);
    buf->len += add;
    return true;
}

/* Read one line from IN, without the trailing newline, into BUF.
 * Returns false at end of input. Phase 1 replaces this with line.c. */
static bool read_line(FILE *in, Buffer *out)
{
    char chunk[512];
    bool got_any = false;

    buffer_clear(out);
    while (fgets(chunk, (int)sizeof(chunk), in) != NULL) {
        size_t n = strlen(chunk);

        got_any = true;
        if (n > 0u && chunk[n - 1u] == '\n') {
            chunk[n - 1u] = '\0';
            return buffer_append(out, chunk);
        }
        if (!buffer_append(out, chunk)) {
            return false;
        }
    }
    return got_any;
}

/* Trim leading whitespace; used to spot dot commands and blank lines. */
static const char *skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') {
        text++;
    }
    return text;
}

/* Phase 5 moves this into dot.c with a proper command table.
 * Phase 0 recognises only what is needed to leave the shell. */
static bool handle_dot_command(Db *db, const char *line, bool *quit, FILE *out, FILE *err)
{
    const char *cmd = skip_space(line);

    if (*cmd != '.') {
        return false;
    }
    cmd++;

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        *quit = true;
    } else if (strcmp(cmd, "tables") == 0) {
        (void)db_exec(db,
                      "SELECT name FROM sqlite_schema "
                      "WHERE type IN ('table','view') AND name NOT LIKE 'sqlite_%' "
                      "ORDER BY name",
                      out, err);
    } else if (strcmp(cmd, "schema") == 0) {
        (void)db_exec(db,
                      "SELECT sql FROM sqlite_schema "
                      "WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' "
                      "ORDER BY rootpage",
                      out, err);
    } else if (strcmp(cmd, "help") == 0) {
        fputs(".help    this message\n"
              ".tables  list tables and views\n"
              ".schema  show the schema\n"
              ".quit    exit\n"
              "\nCompletion is not implemented yet; see PROJECT.md.\n",
              out);
    } else {
        fprintf(err, "sqlsh: unknown command: .%s\n", cmd);
    }
    return true;
}

static void usage(FILE *out)
{
    fputs("usage: sqlsh [options] [database] [sql...]\n"
          "\n"
          "  -h, --help     show this message\n"
          "  -v, --version  show version\n"
          "\n"
          "With no database, an in-memory one is used.\n"
          "Any SQL arguments are executed, after which sqlsh exits.\n",
          out);
}

/* What the REPL should do after consuming one line. */
typedef enum { REPL_CONTINUE, REPL_QUIT, REPL_ERROR } ReplStep;

/* Consume one input line. STMT accumulates lines until they form a complete
 * statement, which is then executed and cleared. A dot command is recognised
 * only at the start of a statement, so ".quit" inside a string is just text. */
static ReplStep repl_feed(Db *db, Buffer *stmt, const char *text, bool interactive)
{
    bool quit = false;

    if (stmt->len == 0u) {
        if (*skip_space(text) == '\0') {
            return REPL_CONTINUE;
        }
        if (handle_dot_command(db, text, &quit, stdout, stderr)) {
            return quit ? REPL_QUIT : REPL_CONTINUE;
        }
    }

    if (!buffer_append(stmt, text) || !buffer_append(stmt, "\n")) {
        fputs("sqlsh: out of memory\n", stderr);
        return REPL_ERROR;
    }
    if (!db_is_complete(stmt->data)) {
        return REPL_CONTINUE;
    }

    /* Interactively, a failed statement is reported and the session goes on;
     * in a pipe it is fatal, so a broken script does not run to completion. */
    if (!db_exec(db, stmt->data, stdout, stderr) && !interactive) {
        buffer_clear(stmt);
        return REPL_ERROR;
    }
    buffer_clear(stmt);
    return REPL_CONTINUE;
}

static void print_prompt(const Buffer *stmt, bool interactive)
{
    if (!interactive) {
        return;
    }
    fputs(stmt->len > 0u ? "   ...> " : "sqlsh> ", stdout);
    fflush(stdout);
}

static int run_repl(Db *db, bool interactive)
{
    Buffer line = {NULL, 0u, 0u};
    Buffer stmt = {NULL, 0u, 0u};
    int status = 0;

    for (;;) {
        ReplStep step;

        print_prompt(&stmt, interactive);
        if (!read_line(stdin, &line)) {
            break;
        }
        step = repl_feed(db, &stmt, line.data != NULL ? line.data : "", interactive);
        if (step == REPL_QUIT) {
            break;
        }
        if (step == REPL_ERROR) {
            status = 1;
            break;
        }
    }

    /* An unterminated statement at end of input is an error, not silence. */
    if (stmt.len > 0u && *skip_space(stmt.data) != '\0') {
        fputs("sqlsh: incomplete statement at end of input\n", stderr);
        status = 1;
    }
    if (interactive) {
        fputc('\n', stdout);
    }

    buffer_free(&line);
    buffer_free(&stmt);
    return status;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    Db *db;
    int i;
    int first_sql = 0;
    int status = 0;
    bool interactive;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            printf("sqlsh %s (sqlite %s)\n", SQLSH_VERSION, db_sqlite_version());
            return 0;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "sqlsh: unknown option: %s\n", arg);
            usage(stderr);
            return 2;
        }
        if (path == NULL) {
            path = arg;
        } else {
            first_sql = i;
            break;
        }
    }

    db = db_open(path, stderr);
    if (db == NULL) {
        return 1;
    }

    if (first_sql != 0) {
        /* Non-interactive: run the SQL arguments and exit. */
        for (i = first_sql; i < argc; i++) {
            if (!db_exec(db, argv[i], stdout, stderr)) {
                status = 1;
                break;
            }
        }
        db_close(db);
        return status;
    }

    interactive = isatty(STDIN_FILENO) == 1 && isatty(STDOUT_FILENO) == 1;
    if (interactive) {
        printf("sqlsh %s connected to %s\n", SQLSH_VERSION, db_path(db));
        fputs("Enter SQL, or .help for commands, or .quit to exit.\n", stdout);
    }

    status = run_repl(db, interactive);
    db_close(db);
    return status;
}
