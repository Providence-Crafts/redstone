/*
 * sqlsh - a minimal SQLite shell with zsh-style completion.
 *
 * Argument handling and the REPL. Input comes from line.c, which edits when a
 * terminal is attached and reads plainly otherwise. The completion engine
 * Completion is wired here because this is the only place that owns both the
 * database and the editor: line.c is handed a function, not a connection.
 */
#include "comp.h"
#include "db.h"
#include "line.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQLSH_VERSION "0.1.0"

/* The completion hook. Dot-command candidates arrive in Phase 6, when dot.c
 * owns the command table; until then the third argument is NULL and dot
 * contexts simply yield nothing. */
static CompList *complete_for(void *ctx, const char *text, size_t cursor)
{
    SqlContext sctx;

    sql_context(text, cursor, &sctx);
    return comp_generate((Db *)ctx, &sctx, NULL);
}

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
static bool handle_dot_command(Db *db, Line *ln, const char *line, bool *quit, FILE *out, FILE *err)
{
    const char *cmd = skip_space(line);

    if (*cmd != '.') {
        return false;
    }
    cmd++;

    if (strncmp(cmd, "editor", 6u) == 0 && (cmd[6] == '\0' || cmd[6] == ' ')) {
        const char *arg = skip_space(cmd + 6);

        if (strcmp(arg, "vi") == 0) {
            line_set_keymap(ln, EDIT_VI);
        } else if (strcmp(arg, "emacs") == 0) {
            line_set_keymap(ln, EDIT_EMACS);
        } else if (*arg == '\0') {
            fprintf(out, "%s\n", line_keymap(ln) == EDIT_VI ? "vi" : "emacs");
        } else {
            fprintf(err, "sqlsh: .editor takes emacs or vi\n");
        }
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
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
              ".editor emacs|vi  select the keymap\n"
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
static ReplStep repl_feed(Db *db, Line *ln, Buffer *stmt, const char *text, bool interactive)
{
    bool quit = false;

    if (stmt->len == 0u) {
        if (*skip_space(text) == '\0') {
            return REPL_CONTINUE;
        }
        if (handle_dot_command(db, ln, text, &quit, stdout, stderr)) {
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

/* In vi mode the prompt says which state the editor is in, because an editor
 * with hidden modes is the thing people hate about vi bindings. */
static const char *prompt_for(const Line *ln, const Buffer *stmt, bool interactive)
{
    if (!interactive) {
        return "";
    }
    if (stmt->len > 0u) {
        return "   ...> ";
    }
    if (line_keymap(ln) == EDIT_VI) {
        return line_vi_state(ln) == EDIT_VI_NORMAL ? "[n] sqlsh> " : "[i] sqlsh> ";
    }
    return "sqlsh> ";
}

/* Choose the keymap from $EDITOR, the way readline-based tools do, so a vi
 * user gets vi mode without configuring anything. `.editor` overrides it in a
 * later phase. */
static EditKeymap keymap_from_env(void)
{
    const char *editor = getenv("VISUAL");

    if (editor == NULL || *editor == '\0') {
        editor = getenv("EDITOR");
    }
    if (editor == NULL) {
        return EDIT_EMACS;
    }
    if (strstr(editor, "vi") != NULL || strstr(editor, "nvim") != NULL) {
        return EDIT_VI;
    }
    return EDIT_EMACS;
}

static int run_repl(Db *db, Line *ln)
{
    Buffer stmt = {NULL, 0u, 0u};
    bool interactive = line_interactive(ln);
    char *hist_path = interactive ? line_history_path() : NULL;
    int status = 0;

    if (hist_path != NULL) {
        (void)line_history_load(ln, hist_path);
    }

    for (;;) {
        LineStatus read_status = line_read(ln, prompt_for(ln, &stmt, interactive));
        ReplStep step;

        if (read_status == LINE_EOF) {
            break;
        }
        if (read_status == LINE_ERROR) {
            fputs("sqlsh: input error\n", stderr);
            status = 1;
            break;
        }
        if (read_status == LINE_INTR) {
            /* Ctrl-C abandons the statement under construction, not the
             * session; that is what every other shell does. */
            buffer_clear(&stmt);
            continue;
        }

        (void)line_history_add(ln, line_text(ln));
        step = repl_feed(db, ln, &stmt, line_text(ln), interactive);
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
    if (hist_path != NULL) {
        (void)line_history_save(ln, hist_path);
        free(hist_path);
    }

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
    Line *ln;

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

    ln = line_new(stdin, stdout);
    if (ln == NULL) {
        fputs("sqlsh: out of memory\n", stderr);
        db_close(db);
        return 1;
    }
    line_set_keymap(ln, keymap_from_env());
    {
        LineCompleter completer;

        completer.generate = complete_for;
        completer.ctx = db;
        line_set_completer(ln, &completer);
    }
    theme_detect(stdout);

    if (line_interactive(ln)) {
        printf("sqlsh %s connected to %s\n", SQLSH_VERSION, db_path(db));
        fputs("Enter SQL, or .help for commands, or .quit to exit.\n", stdout);
    }

    status = run_repl(db, ln);
    line_free(ln);
    db_close(db);
    return status;
}
