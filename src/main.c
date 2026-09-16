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
#include "out.h"
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

/* Split LINE into at most MAX arguments, honouring single and double quotes
 * so that `.separator " | "` and `.nullvalue '<null>'` work. The tokens point
 * into LINE, which is modified in place. Returns the count. */
static int split_args(char *line, char **argv, int max)
{
    int n = 0;

    while (n < max) {
        char quote = '\0';
        char *dst;

        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0') {
            break;
        }
        argv[n++] = dst = line;
        while (*line != '\0' && (quote != '\0' || (*line != ' ' && *line != '\t'))) {
            if (quote == '\0' && (*line == '\'' || *line == '"')) {
                quote = *line++;
            } else if (quote != '\0' && *line == quote) {
                quote = '\0';
                line++;
            } else {
                *dst++ = *line++;
            }
        }
        if (*line != '\0') {
            line++;
        }
        *dst = '\0';
    }
    return n;
}

/* The output dot commands. Phase 6 moves these into dot.c with the rest of
 * the command table; they live here now because Phase 5 cannot be tested
 * without a way to change the mode from a script. */
static bool handle_output_command(Db *db, const char *cmd, FILE *out, FILE *err)
{
    char buf[512];
    char *argv[32];
    const char *args;
    int argc;
    size_t n;

    if (strncmp(cmd, "mode", 4u) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        args = cmd + 4;
    } else if (strncmp(cmd, "headers", 7u) == 0 && (cmd[7] == '\0' || cmd[7] == ' ')) {
        args = cmd + 7;
    } else if (strncmp(cmd, "nullvalue", 9u) == 0 && (cmd[9] == '\0' || cmd[9] == ' ')) {
        args = cmd + 9;
    } else if (strncmp(cmd, "separator", 9u) == 0 && (cmd[9] == '\0' || cmd[9] == ' ')) {
        args = cmd + 9;
    } else if (strncmp(cmd, "width", 5u) == 0 && (cmd[5] == '\0' || cmd[5] == ' ')) {
        args = cmd + 5;
    } else {
        return false;
    }

    n = strlen(args);
    if (n >= sizeof(buf)) {
        fprintf(err, "sqlsh: command too long\n");
        return true;
    }
    memcpy(buf, args, n + 1u);
    argc = split_args(buf, argv, (int)(sizeof(argv) / sizeof(argv[0])));

    if (cmd[0] == 'm') {
        if (argc == 0) {
            out_describe(db_out(db), out);
        } else {
            (void)out_command(db_out(db), argc, (const char *const *)argv, err);
        }
    } else if (cmd[0] == 'h') {
        if (argc == 0) {
            fprintf(out, "%s\n", out_headers(db_out(db)) ? "on" : "off");
        } else {
            out_set_headers(db_out(db), strcmp(argv[0], "on") == 0 || strcmp(argv[0], "yes") == 0 ||
                                            strcmp(argv[0], "1") == 0);
        }
    } else if (cmd[0] == 'n') {
        out_set_null_text(db_out(db), argc > 0 ? argv[0] : "");
    } else if (cmd[0] == 's') {
        if (argc > 0) {
            out_set_colsep(db_out(db), argv[0]);
        }
        if (argc > 1) {
            out_set_rowsep(db_out(db), argv[1]);
        }
    } else {
        const char *flag[2];
        char joined[256];
        size_t used = 0u;
        int i;

        for (i = 0; i < argc && used + 1u < sizeof(joined); i++) {
            int wrote = snprintf(joined + used, sizeof(joined) - used, "%s%s", used > 0u ? "," : "",
                                 argv[i]);

            if (wrote < 0) {
                break;
            }
            used += (size_t)wrote;
        }
        joined[used] = '\0';
        flag[0] = "--widths";
        flag[1] = joined;
        (void)out_command(db_out(db), 2, flag, err);
    }
    return true;
}

/* Phase 6 moves this into dot.c with a proper command table.
 * Phase 0 recognises only what is needed to leave the shell. */
static bool handle_dot_command(Db *db, Line *ln, const char *line, bool *quit, FILE *out, FILE *err)
{
    const char *cmd = skip_space(line);

    if (*cmd != '.') {
        return false;
    }
    cmd++;

    if (handle_output_command(db, cmd, out, err)) {
        return true;
    }

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
              ".mode [MODE] [OPTIONS]  output format\n"
              ".headers on|off\n"
              ".nullvalue TEXT\n"
              ".separator COL [ROW]\n"
              ".width N ...\n"
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
          "  --compat       format results exactly as sqlite3(1) does\n"
          "  -MODE          start in MODE (box, list, csv, json, ...)\n"
          "  -header, -noheader\n"
          "  -separator SEP, -nullvalue TEXT\n"
          "\n"
          "With no database, an in-memory one is used.\n"
          "Any SQL arguments are executed, after which sqlsh exits.\n",
          out);
}

/* The command-line options that consume the following argument. */
static bool option_takes_value(const char *arg)
{
    return strcmp(arg, "-separator") == 0 || strcmp(arg, "--separator") == 0 ||
           strcmp(arg, "-nullvalue") == 0 || strcmp(arg, "--nullvalue") == 0;
}

/* sqlite3(1) accepts a -MODE flag on the command line for only this subset
 * of its .mode names -- notably not "insert", "tcl", "www" or "qbox", even
 * though .mode itself accepts all of them. Verified against the 3.53.3
 * binary: "sqlite3 -insert :memory: ..." is an unknown option. */
static bool is_cmdline_mode(const char *name)
{
    static const char *const modes[] = {"ascii", "box",  "column",   "csv",   "html",  "json",
                                        "line",  "list", "markdown", "quote", "table", "tabs"};
    size_t i;

    for (i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(name, modes[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Apply the output options. Anything that is not one is an unknown option:
 * Phase 6 adds the rest of sqlite3(1)'s command line, and rejecting here
 * keeps a typo from being silently ignored until then. */
static bool apply_options(Db *db, int argc, char *const *argv, int first_sql)
{
    int last = first_sql > 0 ? first_sql : argc;
    int i;

    /* --compat is a starting point, not a setting: it has to be applied
     * before the options that refine it, whatever order they were given in. */
    for (i = 1; i < last; i++) {
        if (strcmp(argv[i], "--compat") == 0 || strcmp(argv[i], "-compat") == 0) {
            out_set_compat(db_out(db));
        }
    }

    for (i = 1; i < last; i++) {
        const char *arg = argv[i];
        const char *name;

        if (arg[0] != '-' || arg[1] == '\0') {
            continue; /* the database path */
        }
        name = arg[1] == '-' ? arg + 2 : arg + 1;
        if (strcmp(name, "compat") == 0) {
            continue; /* applied above */
        }
        if (strcmp(name, "header") == 0) {
            out_set_headers(db_out(db), true);
        } else if (strcmp(name, "noheader") == 0) {
            out_set_headers(db_out(db), false);
        } else if (option_takes_value(arg)) {
            if (i + 1 >= last) {
                fprintf(stderr, "sqlsh: %s requires an argument\n", arg);
                return false;
            }
            if (name[0] == 's') {
                out_set_colsep(db_out(db), argv[i + 1]);
            } else {
                out_set_null_text(db_out(db), argv[i + 1]);
            }
            i++;
        } else if (!is_cmdline_mode(name) || !out_set_mode(db_out(db), name)) {
            fprintf(stderr, "sqlsh: unknown option: %s\n", arg);
            usage(stderr);
            return false;
        }
    }
    return true;
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

    /* Two passes: the database has to be open before an option can change
     * how its results are formatted, and the path is an argument like any
     * other. The first pass finds the path and the end of the options. */
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
            if (option_takes_value(arg)) {
                i++;
            }
            continue;
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
    theme_detect(stdout);
    out_set_colour(db_out(db), theme_colour());
    if (!apply_options(db, argc, argv, first_sql)) {
        db_close(db);
        return 2;
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
    if (line_interactive(ln)) {
        printf("sqlsh %s connected to %s\n", SQLSH_VERSION, db_path(db));
        fputs("Enter SQL, or .help for commands, or .quit to exit.\n", stdout);
    }

    status = run_repl(db, ln);
    line_free(ln);
    db_close(db);
    return status;
}
