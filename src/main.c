/*
 * sqlsh - a minimal SQLite shell with zsh-style completion.
 *
 * Nothing but the command line lives here. Everything the session does --
 * dot commands, redirects, init files, the REPL -- belongs to shell.c, so
 * that a statement behaves the same whether it arrives from a terminal, from
 * `.read`, from `~/.sqliterc` or from a `-cmd` argument.
 */
#include "comp.h"
#include "db.h"
#include "dot.h"
#include "line.h"
#include "out.h"
#include "shell.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SQLSH_VERSION "0.1.0"

static CompList *complete_for(void *ctx, const char *text, size_t cursor)
{
    Shell *sh = ctx;
    SqlContext sctx;

    sql_context(text, cursor, &sctx);
    return comp_generate(shell_db(sh), &sctx, dot_comp_source());
}

static void usage(FILE *out)
{
    fputs("usage: sqlsh [options] [database] [sql...]\n"
          "\n"
          "  -h, --help          show this message\n"
          "  -v, --version       show version\n"
          "  --compat            format results exactly as sqlite3(1) does\n"
          "  -MODE               start in MODE (box, list, csv, json, ...)\n"
          "  -init FILE          read FILE instead of the default init files\n"
          "  -noinit             read no init file\n"
          "  -cmd COMMAND        run COMMAND before reading stdin\n"
          "  -batch, -interactive\n"
          "  -echo, -header, -noheader, -bail, -stats\n"
          "  -readonly, -safe, -nonce STRING\n"
          "  -separator SEP, -nullvalue TEXT, -newline SEP\n"
          "\n"
          "With no database, an in-memory one is used.\n"
          "Any SQL arguments are executed, after which sqlsh exits.\n",
          out);
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

/* Both spellings, because sqlite3(1) takes either. */
static bool is_opt(const char *arg, const char *name)
{
    if (arg[0] != '-') {
        return false;
    }
    arg++;
    if (arg[0] == '-') {
        arg++;
    }
    return strcmp(arg, name) == 0;
}

/* The options that consume the following argument. Needed by the first pass,
 * which has to know where the database name is without interpreting
 * anything. */
static bool option_takes_value(const char *arg)
{
    static const char *const with_value[] = {
        "separator", "nullvalue", "newline",   "init", "cmd",  "vfs",       "nonce",
        "maxsize",   "lookaside", "pagecache", "mmap", "heap", "sorterref", "multiplex"};
    size_t i;

    for (i = 0u; i < sizeof(with_value) / sizeof(with_value[0]); i++) {
        if (is_opt(arg, with_value[i])) {
            return true;
        }
    }
    return false;
}

/* Options sqlsh recognises but cannot honour, each naming what it would
 * need. Silently ignoring them would make a script look like it ran with
 * settings it did not get. */
static const char *refused_option(const char *arg)
{
    static const struct {
        const char *name;
        const char *needs;
    } refused[] = {
        {"A", "sqlar.c and zipfile.c"},
        {"archive", "sqlar.c and zipfile.c"},
        {"zip", "zipfile.c"},
        {"append", "appendvfs.c"},
        {"deserialize", "sqlite3_deserialize() in this build"},
        {"maxsize", "sqlite3_deserialize() in this build"},
        {"memtrace", "an instrumented memory allocator"},
        {"multiplex", "test_multiplex.c"},
        {"lookaside", "start-up configuration sqlsh does not expose"},
        {"pagecache", "start-up configuration sqlsh does not expose"},
        {"mmap", "start-up configuration sqlsh does not expose"},
        {"heap", "start-up configuration sqlsh does not expose"},
        {"sorterref", "start-up configuration sqlsh does not expose"},
    };
    size_t i;

    for (i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (is_opt(arg, refused[i].name)) {
            return refused[i].needs;
        }
    }
    return NULL;
}

/* One option. Returns the number of argv entries consumed, or -1 on error.
 * The database path and the SQL arguments are found by the first pass and
 * are not seen here. */
static int apply_option(Shell *sh, char *const *argv, int i, int argc)
{
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    Out *out = db_out(shell_db(sh));
    const char *needs;

    if (option_takes_value(arg) && value == NULL) {
        fprintf(stderr, "sqlsh: %s requires an argument\n", arg);
        return -1;
    }
    needs = refused_option(arg);
    if (needs != NULL) {
        fprintf(stderr, "sqlsh: %s is not supported: it needs %s.\n", arg, needs);
        return option_takes_value(arg) ? 2 : 1;
    }

    if (is_opt(arg, "compat")) {
        out_set_compat(out);
        out_set_colour(out, false);
    } else if (is_opt(arg, "header")) {
        out_set_headers(out, true);
    } else if (is_opt(arg, "noheader")) {
        out_set_headers(out, false);
    } else if (is_opt(arg, "echo")) {
        shell_set_flag(sh, SHELL_ECHO, true);
    } else if (is_opt(arg, "bail")) {
        shell_set_flag(sh, SHELL_BAIL, true);
    } else if (is_opt(arg, "stats")) {
        shell_set_flag(sh, SHELL_STATS, true);
    } else if (is_opt(arg, "safe")) {
        shell_set_flag(sh, SHELL_SAFE, true);
    } else if (is_opt(arg, "nonce")) {
        shell_set_nonce(sh, value);
        return 2;
    } else if (is_opt(arg, "separator")) {
        out_set_colsep(out, value);
        return 2;
    } else if (is_opt(arg, "newline")) {
        out_set_rowsep(out, value);
        return 2;
    } else if (is_opt(arg, "nullvalue")) {
        out_set_null_text(out, value);
        return 2;
    } else if (is_opt(arg, "cmd")) {
        /* Deferred: -cmd runs after the init files, in argv order, which is
         * why the caller makes a second pass for these alone. */
        return 2;
    } else if (is_opt(arg, "vfs")) {
        const char *current = db_vfs_current(shell_db(sh), "main");

        if (current == NULL || value == NULL || strcmp(current, value) != 0) {
            fprintf(stderr, "sqlsh: only the \"%s\" VFS is available; ignoring -vfs %s\n",
                    current != NULL ? current : "default", value != NULL ? value : "");
        }
        return 2;
    } else if (arg[0] == '-' && is_cmdline_mode(arg + (arg[1] == '-' ? 2 : 1))) {
        (void)out_set_mode(out, arg + (arg[1] == '-' ? 2 : 1));
    } else {
        fprintf(stderr, "sqlsh: unknown option: %s\n", arg);
        return -1;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *init_file = NULL;
    bool noinit = false;
    bool readonly = false;
    bool force_batch = false;
    bool force_interactive = false;
    int first_sql = 0;
    int status = 0;
    int i;
    Db *db;
    Line *ln;
    Shell *sh;
    LineCompleter completer;

    /* First pass: the flags that have to be known before the database is
     * opened, plus the position of the database name and of the first SQL
     * argument. Nothing here can depend on the connection. */
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (is_opt(arg, "h") || is_opt(arg, "help") || is_opt(arg, "?")) {
            usage(stdout);
            return 0;
        }
        if (is_opt(arg, "v") || is_opt(arg, "version")) {
            printf("sqlsh %s (sqlite %s)\n", SQLSH_VERSION, db_sqlite_version());
            return 0;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            if (is_opt(arg, "readonly")) {
                readonly = true;
            } else if (is_opt(arg, "batch")) {
                force_batch = true;
            } else if (is_opt(arg, "interactive")) {
                force_interactive = true;
            } else if (is_opt(arg, "noinit")) {
                noinit = true;
            } else if (is_opt(arg, "init")) {
                init_file = i + 1 < argc ? argv[i + 1] : NULL;
            }
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

    db = db_open_mode(path, readonly, stderr);
    if (db == NULL) {
        return 1;
    }
    theme_detect(stdout);
    out_set_colour(db_out(db), theme_colour());

    ln = line_new(stdin, stdout);
    if (ln == NULL) {
        fputs("sqlsh: out of memory\n", stderr);
        db_close(db);
        return 1;
    }
    sh = shell_new(stdin, stdout, stderr);
    if (sh == NULL) {
        fputs("sqlsh: out of memory\n", stderr);
        line_free(ln);
        db_close(db);
        return 1;
    }
    shell_adopt(sh, db, ln);

    completer.generate = complete_for;
    completer.ctx = sh;
    line_set_completer(ln, &completer);
    if (force_batch) {
        line_set_interactive(ln, false);
    } else if (force_interactive) {
        line_set_interactive(ln, true);
    }

    /* Second pass: the settings, in the order they were written, so a later
     * option wins over an earlier one exactly as it does upstream. */
    for (i = 1; i < argc && (first_sql == 0 || i < first_sql); i++) {
        int used;

        if (argv[i][0] != '-' || argv[i][1] == '\0') {
            continue; /* the database name */
        }
        if (is_opt(argv[i], "readonly") || is_opt(argv[i], "batch") ||
            is_opt(argv[i], "interactive") || is_opt(argv[i], "noinit")) {
            continue; /* handled in the first pass */
        }
        if (is_opt(argv[i], "init")) {
            i++;
            continue;
        }
        used = apply_option(sh, argv, i, argc);
        if (used < 0) {
            shell_free(sh);
            return 2;
        }
        i += used - 1;
    }

    /* The init files come after the options so that -init's file can rely on
     * them, and before -cmd so that -cmd can override what they set. */
    if (!noinit) {
        shell_load_init(sh, init_file);
    }

    for (i = 1; i < argc && !shell_quitting(sh) && (first_sql == 0 || i < first_sql); i++) {
        if (is_opt(argv[i], "cmd") && i + 1 < argc) {
            if (!shell_feed(sh, argv[++i]) && shell_flag(sh, SHELL_BAIL)) {
                status = 1;
                break;
            }
        } else if (option_takes_value(argv[i])) {
            i++;
        }
    }

    if (status == 0 && first_sql != 0) {
        /* Non-interactive: run the SQL arguments and exit. */
        line_set_interactive(ln, false);
        for (i = first_sql; i < argc && !shell_quitting(sh); i++) {
            if (!shell_feed(sh, argv[i])) {
                status = 1;
                break;
            }
        }
    } else if (status == 0 && !shell_quitting(sh)) {
        if (line_interactive(ln)) {
            printf("sqlsh %s connected to %s\n", SQLSH_VERSION, db_path(db));
            fputs("Enter SQL, or .help for commands, or .quit to exit.\n", stdout);
        }
        status = shell_run(sh);
    }

    if (status == 0) {
        status = shell_status(sh);
    }
    shell_free(sh);
    return status;
}
