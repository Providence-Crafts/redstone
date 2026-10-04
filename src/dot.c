/*
 * dot.c - the dot-command table and its dispatch.
 *
 * One table, one lookup, one place to add a command. Every command that
 * sqlite3(1) documents appears in it: the ones redstone implements and the
 * eleven it refuses because they are backed by extension sources this
 * project deliberately does not vendor. A refusal is a table entry like any
 * other, so `.help` lists it and completion offers it -- the gap is
 * documented rather than discovered.
 *
 * The commands that render the database as text live in schema.c, and the
 * ones that move tabular data in and out live in import.c. What stays here
 * is everything that configures the session.
 */
#include "dot.h"

#include "import.h"
#include "out.h"
#include "plat.h"
#include "schema.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Argument handling
 * ------------------------------------------------------------------------ */

int dot_split(char *line, char **argv, int max)
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
            } else if (*line == '\\' && quote != '\'' && line[1] != '\0') {
                /* Backslash escapes, as upstream's parser has them, so that
                 * `.separator \t` and `.print a\nb` behave. */
                char c = line[1];

                line += 2;
                switch (c) {
                case 'n':
                    *dst++ = '\n';
                    break;
                case 't':
                    *dst++ = '\t';
                    break;
                case 'r':
                    *dst++ = '\r';
                    break;
                case '\\':
                    *dst++ = '\\';
                    break;
                default:
                    *dst++ = c;
                    break;
                }
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

/* cppcheck-suppress staticFunction ; dot.h exports the on/off parser so
 * that every command spells a boolean argument the same way */
bool dot_boolean(Shell *sh, const char *text)
{
    if (strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "1") == 0) {
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "no") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "0") == 0) {
        return false;
    }
    fprintf(shell_err(sh), "ERROR: Not a boolean value: \"%s\". Assuming \"no\".\n", text);
    return false;
}

/* A switch command: one argument, on or off, and no argument reports. */
static bool flag_command(Shell *sh, int argc, char **argv, ShellFlag flag)
{
    if (argc < 2) {
        fprintf(shell_out(sh), "%s: %s\n", argv[0], shell_flag(sh, flag) ? "on" : "off");
        return true;
    }
    shell_set_flag(sh, flag, dot_boolean(sh, argv[1]));
    return true;
}

static void usage_error(Shell *sh, const char *usage)
{
    fprintf(shell_err(sh), "Usage: %s\n", usage);
}

/* Join ARGV[from..argc) with single spaces into a fresh string. `.shell`,
 * `.system` and `.print` all want the rest of the line back. */
static char *join_args(int argc, char **argv, int from)
{
    size_t need = 1u;
    size_t at = 0u;
    char *text;
    int i;

    for (i = from; i < argc; i++) {
        need += strlen(argv[i]) + 1u;
    }
    text = malloc(need);
    if (text == NULL) {
        return NULL;
    }
    /* Copied at a tracked offset rather than with strcat: the length of every
     * piece is already known, so rescanning the buffer each time buys nothing
     * and hides the bound from the reader. */
    for (i = from; i < argc; i++) {
        size_t len = strlen(argv[i]);

        if (i > from) {
            text[at++] = ' ';
        }
        memcpy(text + at, argv[i], len);
        at += len;
    }
    text[at] = '\0';
    return text;
}

/* --------------------------------------------------------------------------
 * Output settings
 * ------------------------------------------------------------------------ */

static bool cmd_mode(Shell *sh, int argc, char **argv)
{
    Out *out = db_out(shell_db(sh));

    if (argc < 2) {
        out_describe(out, shell_out(sh));
        return true;
    }
    return out_command(out, argc - 1, (const char *const *)(argv + 1), shell_err(sh));
}

static bool cmd_headers(Shell *sh, int argc, char **argv)
{
    Out *out = db_out(shell_db(sh));

    if (argc < 2) {
        fprintf(shell_out(sh), "%s\n", out_headers(out) ? "on" : "off");
        return true;
    }
    out_set_headers(out, dot_boolean(sh, argv[1]));
    return true;
}

static bool cmd_nullvalue(Shell *sh, int argc, char **argv)
{
    out_set_null_text(db_out(shell_db(sh)), argc > 1 ? argv[1] : "");
    return true;
}

static bool cmd_separator(Shell *sh, int argc, char **argv)
{
    Out *out = db_out(shell_db(sh));

    if (argc < 2) {
        usage_error(sh, ".separator COL ?ROW?");
        return false;
    }
    if (argc > 3) {
        usage_error(sh, ".separator COL ?ROW?");
        return false;
    }
    /* Only the column separator is set. The ROW argument is accepted and
     * ignored, which is what the 3.53.3 binary does -- "-newline" and
     * ".mode --rowsep" are the ways to change the row separator there. */
    out_set_colsep(out, argv[1]);
    return true;
}

static bool cmd_width(Shell *sh, int argc, char **argv)
{
    char joined[256];
    const char *flag[2];
    size_t used = 0u;
    int i;

    for (i = 1; i < argc && used + 1u < sizeof(joined); i++) {
        int wrote =
            snprintf(joined + used, sizeof(joined) - used, "%s%s", used > 0u ? "," : "", argv[i]);

        if (wrote < 0) {
            break;
        }
        used += (size_t)wrote;
    }
    joined[used] = '\0';
    flag[0] = "--widths";
    flag[1] = joined;
    return out_command(db_out(shell_db(sh)), 2, flag, shell_err(sh));
}

static bool cmd_crlf(Shell *sh, int argc, char **argv)
{
    bool on = argc > 1 ? dot_boolean(sh, argv[1]) : shell_flag(sh, SHELL_CRLF);

    if (argc < 2) {
        fprintf(shell_out(sh), "crlf is %s\n", on ? "ON" : "OFF");
        return true;
    }
    shell_set_flag(sh, SHELL_CRLF, on);
    out_set_rowsep(db_out(shell_db(sh)), on ? "\r\n" : "\n");
    return true;
}

/* `.binary` predates the encodings out.c has; every file redstone opens is
 * already opened in binary mode, so the flag is recorded and reported and
 * changes nothing. Saying so is better than pretending the command is
 * unknown to a script that sets it. */
static bool cmd_binary(Shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(shell_out(sh), "binary: on\n");
        return true;
    }
    (void)dot_boolean(sh, argv[1]);
    return true;
}

static bool cmd_explain(Shell *sh, int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : "on";

    if (strcmp(arg, "auto") == 0) {
        shell_set_explain(sh, SHELL_EXPLAIN_AUTO);
    } else {
        shell_set_explain(sh, dot_boolean(sh, arg) ? SHELL_EXPLAIN_ON : SHELL_EXPLAIN_OFF);
    }
    return true;
}

static bool cmd_eqp(Shell *sh, int argc, char **argv)
{
    const char *arg = argc > 1 ? argv[1] : NULL;

    if (arg == NULL) {
        fprintf(shell_out(sh), "eqp: %s\n", shell_flag(sh, SHELL_EQP) ? "on" : "off");
        return true;
    }
    /* `full` and `trigger` add VDBE detail upstream gets from an internal
     * flag; redstone treats them as `on` and says so in the docs rather than
     * rejecting a script that uses them. */
    shell_set_flag(sh, SHELL_EQP,
                   strcmp(arg, "full") == 0 || strcmp(arg, "trigger") == 0 || dot_boolean(sh, arg));
    return true;
}

/* --------------------------------------------------------------------------
 * Output destination
 * ------------------------------------------------------------------------ */

static bool redirect_command(Shell *sh, int argc, char **argv, bool once)
{
    if (shell_unsafe(sh, argv[0])) {
        return false;
    }
    if (argc < 2) {
        return shell_redirect(sh, NULL, false);
    }
    return shell_redirect(sh, argv[1], once);
}

static bool cmd_output(Shell *sh, int argc, char **argv)
{
    return redirect_command(sh, argc, argv, false);
}

static bool cmd_once(Shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        usage_error(sh, ".once ?FILE?|?|COMMAND?");
        return false;
    }
    return redirect_command(sh, argc, argv, true);
}

static bool cmd_log(Shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(shell_out(sh), "%s\n", shell_log_name(sh));
        return true;
    }
    if (shell_unsafe(sh, "log")) {
        return false;
    }
    return shell_set_log(sh, argv[1]);
}

static bool cmd_print(Shell *sh, int argc, char **argv)
{
    char *text = join_args(argc, argv, 1);

    if (text == NULL) {
        return false;
    }
    fprintf(shell_out(sh), "%s\n", text);
    free(text);
    return true;
}

/* --------------------------------------------------------------------------
 * The session
 * ------------------------------------------------------------------------ */

static bool cmd_quit(Shell *sh, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_quit(sh, 0);
    return true;
}

static bool cmd_exit(Shell *sh, int argc, char **argv)
{
    shell_quit(sh, argc > 1 ? (int)strtol(argv[1], NULL, 10) : 0);
    return true;
}

static bool cmd_version(Shell *sh, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    fprintf(shell_out(sh), "SQLite %s %s\n", db_sqlite_version(), db_sqlite_sourceid());
    return true;
}

static bool cmd_prompt(Shell *sh, int argc, char **argv)
{
    shell_set_prompt(sh, argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
    return true;
}

static bool cmd_cd(Shell *sh, int argc, char **argv)
{
    if (shell_unsafe(sh, "cd")) {
        return false;
    }
    if (argc < 2) {
        usage_error(sh, ".cd DIRECTORY");
        return false;
    }
    if (plat_chdir(argv[1]) != 0) {
        fprintf(shell_err(sh), "redstone: cannot change to %s\n", argv[1]);
        return false;
    }
    return true;
}

/* Writes straight to the base stream, ignoring any .output/.once redirect:
 * clearing the terminal is meaningless aimed at a file. */
static bool cmd_clear(Shell *sh, int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        usage_error(sh, ".clear");
        return false;
    }
    fputs("\x1b[H\x1b[2J", shell_base_out(sh));
    (void)fflush(shell_base_out(sh));
    return true;
}

static bool cmd_shell(Shell *sh, int argc, char **argv)
{
    char *text;
    int rc;

    if (shell_unsafe(sh, argv[0])) {
        return false;
    }
    if (argc < 2) {
        usage_error(sh, ".shell COMMAND ARGS...");
        return false;
    }
    text = join_args(argc, argv, 1);
    if (text == NULL) {
        return false;
    }
    (void)fflush(shell_out(sh));
    /* Running a command processor is the entire point of .shell/.system, and
     * -safe refuses the command before it reaches here. */
    /* NOLINTNEXTLINE(cert-env33-c) */
    rc = system(text);
    free(text);
    if (rc != 0) {
        fprintf(shell_err(sh), "redstone: command returned %d\n", rc);
    }
    return rc == 0;
}

static bool cmd_read(Shell *sh, int argc, char **argv)
{
    if (shell_unsafe(sh, "read")) {
        return false;
    }
    if (argc < 2) {
        usage_error(sh, ".read FILE");
        return false;
    }
    return shell_source(sh, argv[1], true);
}

/* Ours, not upstream's: the editor keymap. Upstream has no equivalent
 * because it has no editor of its own. */
/* .theme: redstone's own. With no argument it prints the palette in the theme
 * file's own format, so that the listing is also the starting point for a
 * file: ".theme > ~/.config/redstone/theme" is how a user begins editing one. */
static bool cmd_theme(Shell *sh, int argc, char **argv)
{
    FILE *out = shell_out(sh);
    const char *arg = argc > 1 ? argv[1] : NULL;

    if (arg == NULL || strcmp(arg, "dump") == 0) {
        theme_dump(out);
        return true;
    }
    if (strcmp(arg, "list") == 0) {
        size_t i;

        for (i = 0u; theme_name_at(i) != NULL; i++) {
            fprintf(out, "%s\n", theme_name_at(i));
        }
        return true;
    }
    if (strcmp(arg, "on") == 0 || strcmp(arg, "off") == 0) {
        bool on = dot_boolean(sh, arg);

        theme_set_colour(on);
        out_set_colour(db_out(shell_db(sh)), on);
        return true;
    }
    if (strcmp(arg, "reload") == 0) {
        char *path = theme_path();
        bool ok;

        if (path == NULL) {
            fprintf(shell_err(sh), "redstone: no theme path: set HOME or XDG_CONFIG_HOME\n");
            return false;
        }
        theme_reset();
        ok = theme_load_file(path, shell_err(sh));
        free(path);
        return ok;
    }
    return theme_load(arg, shell_err(sh));
}

/* With an argument, edits that text; with none, edits the line before this
 * one in history (history already holds ".edit" itself by the time a dot
 * command runs, so index count-2 is "what the user typed last"). Either way
 * the result is handed to line_seed rather than run, so a mis-edit or a
 * change of mind is just Ctrl-C away instead of already having executed. */
static bool cmd_edit(Shell *sh, int argc, char **argv)
{
    Line *ln = shell_line(sh);
    char *joined = NULL;
    const char *seed;
    char *edited;

    if (shell_unsafe(sh, argv[0])) {
        return false;
    }
    if (ln == NULL) {
        return true;
    }
    if (argc > 1) {
        joined = join_args(argc, argv, 1);
        if (joined == NULL) {
            return false;
        }
        seed = joined;
    } else {
        size_t n = line_history_count(ln);

        seed = n > 1u ? line_history_at(ln, n - 2u) : "";
    }

    edited = line_external_edit(seed, strlen(seed));
    free(joined);
    if (edited == NULL) {
        fprintf(shell_err(sh), "redstone: EDITOR failed\n");
        return false;
    }
    line_seed(ln, edited);
    free(edited);
    return true;
}

static bool cmd_editor(Shell *sh, int argc, char **argv)
{
    Line *ln = shell_line(sh);

    if (ln == NULL) {
        return true;
    }
    if (argc < 2) {
        fprintf(shell_out(sh), "%s\n", line_keymap(ln) == EDIT_VI ? "vi" : "emacs");
        return true;
    }
    if (strcmp(argv[1], "vi") == 0) {
        line_set_keymap(ln, EDIT_VI);
    } else if (strcmp(argv[1], "emacs") == 0) {
        line_set_keymap(ln, EDIT_EMACS);
    } else {
        usage_error(sh, ".editor emacs|vi");
        return false;
    }
    return true;
}

static bool cmd_connection(Shell *sh, int argc, char **argv)
{
    size_t i;

    if (argc >= 3 && strcmp(argv[1], "close") == 0) {
        return shell_conn_close(sh, (size_t)strtoul(argv[2], NULL, 10));
    }
    if (argc >= 2) {
        return shell_conn_switch(sh, (size_t)strtoul(argv[1], NULL, 10));
    }
    for (i = 0u; i < SHELL_MAX_CONN; i++) {
        const char *path = shell_conn_path(sh, i);

        if (path != NULL) {
            fprintf(shell_out(sh), "%s%u: %s\n",
                    i == shell_conn_current(sh) ? "ACTIVE " : "       ", (unsigned)i, path);
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * The connection
 * ------------------------------------------------------------------------ */

static bool cmd_open(Shell *sh, int argc, char **argv)
{
    const char *path = NULL;
    bool readonly = false;
    bool fresh = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--readonly") == 0 || strcmp(argv[i], "-readonly") == 0) {
            readonly = true;
        } else if (strcmp(argv[i], "--new") == 0 || strcmp(argv[i], "-new") == 0) {
            fresh = true;
        } else if (argv[i][0] == '-') {
            fprintf(shell_err(sh), "redstone: unknown option to .open: %s\n", argv[i]);
            return false;
        } else {
            path = argv[i];
        }
    }
    if (path != NULL && shell_unsafe(sh, "open")) {
        return false;
    }
    if (fresh && path != NULL) {
        (void)remove(path);
    }
    return db_reopen(shell_db(sh), path, readonly, true, shell_err(sh));
}

static bool backup_command(Shell *sh, int argc, char **argv, bool to_file)
{
    const char *dbname = "main";
    const char *file;

    if (shell_unsafe(sh, argv[0])) {
        return false;
    }
    if (argc < 2) {
        usage_error(sh, to_file ? ".backup ?DB? FILE" : ".restore ?DB? FILE");
        return false;
    }
    if (argc > 2) {
        dbname = argv[1];
        file = argv[2];
    } else {
        file = argv[1];
    }
    return to_file ? db_backup_to(shell_db(sh), dbname, file, shell_err(sh))
                   : db_restore_from(shell_db(sh), dbname, file, shell_err(sh));
}

static bool cmd_backup(Shell *sh, int argc, char **argv)
{
    return backup_command(sh, argc, argv, true);
}

static bool cmd_restore(Shell *sh, int argc, char **argv)
{
    return backup_command(sh, argc, argv, false);
}

static bool cmd_timeout(Shell *sh, int argc, char **argv)
{
    db_busy_timeout(shell_db(sh), argc > 1 ? (int)strtol(argv[1], NULL, 10) : 0);
    return true;
}

static bool cmd_load(Shell *sh, int argc, char **argv)
{
    if (shell_unsafe(sh, "load")) {
        return false;
    }
    if (argc < 2) {
        usage_error(sh, ".load FILE ?ENTRYPOINT?");
        return false;
    }
    return db_load_extension(shell_db(sh), argv[1], argc > 2 ? argv[2] : NULL, shell_err(sh));
}

static bool cmd_limit(Shell *sh, int argc, char **argv)
{
    int value = 0;

    if (argc < 2) {
        size_t i;

        for (i = 0u; db_limit_name(i) != NULL; i++) {
            if (db_limit(shell_db(sh), db_limit_name(i), -1, &value)) {
                fprintf(shell_out(sh), "%20s %d\n", db_limit_name(i), value);
            }
        }
        return true;
    }
    if (!db_limit(shell_db(sh), argv[1], argc > 2 ? (int)strtol(argv[2], NULL, 10) : -1, &value)) {
        fprintf(shell_err(sh), "unknown limit: \"%s\"\n", argv[1]);
        return false;
    }
    fprintf(shell_out(sh), "%20s %d\n", argv[1], value);
    return true;
}

/* fp_digits is the one dbconfig option whose value is a count rather than a
 * switch, and sqlite3(1) prints it as a number. */
static bool dbconfig_numeric(const char *name)
{
    return strcmp(name, "fp_digits") == 0;
}

static void dbconfig_print(FILE *dest, const char *name, int value)
{
    if (dbconfig_numeric(name)) {
        fprintf(dest, "%19s %d\n", name, value);
    } else {
        fprintf(dest, "%19s %s\n", name, value ? "on" : "off");
    }
}

static bool cmd_dbconfig(Shell *sh, int argc, char **argv)
{
    int value = 0;
    int want;

    if (argc < 2) {
        size_t i;

        for (i = 0u; db_dbconfig_name(i) != NULL; i++) {
            if (db_dbconfig(shell_db(sh), db_dbconfig_name(i), -1, &value)) {
                dbconfig_print(shell_out(sh), db_dbconfig_name(i), value);
            }
        }
        return true;
    }
    want = -1; /* -1 queries the current setting without changing it */
    if (argc > 2) {
        if (dbconfig_numeric(argv[1])) {
            want = (int)strtol(argv[2], NULL, 10);
        } else {
            want = dot_boolean(sh, argv[2]) ? 1 : 0;
        }
    }
    if (!db_dbconfig(shell_db(sh), argv[1], want, &value)) {
        fprintf(shell_err(sh), "Error: unknown dbconfig \"%s\"\n", argv[1]);
        return false;
    }
    dbconfig_print(shell_out(sh), argv[1], value);
    return true;
}

static bool cmd_filectrl(Shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        size_t i;

        fputs("Available file-controls:\n", shell_out(sh));
        for (i = 0u; db_file_control_name(i) != NULL; i++) {
            fprintf(shell_out(sh), "  .filectrl %s ?ARG?\n", db_file_control_name(i));
        }
        return true;
    }
    if (!db_file_control(shell_db(sh), "main", argv[1], argc > 2 ? argv[2] : NULL, shell_out(sh),
                         shell_err(sh))) {
        fprintf(shell_err(sh), "Error: unknown file-control: %s\n", argv[1]);
        return false;
    }
    return true;
}

static bool cmd_vfslist(Shell *sh, int argc, char **argv)
{
    size_t i;

    (void)argc;
    (void)argv;
    for (i = 0u; db_vfs_at(i) != NULL; i++) {
        fprintf(shell_out(sh), "vfs.zName      = \"%s\"\n", db_vfs_at(i));
    }
    return true;
}

static bool cmd_vfsname(Shell *sh, int argc, char **argv)
{
    const char *name = db_vfs_current(shell_db(sh), argc > 1 ? argv[1] : "main");

    if (name != NULL) {
        fprintf(shell_out(sh), "%s\n", name);
    }
    return true;
}

static bool cmd_vfsinfo(Shell *sh, int argc, char **argv)
{
    const char *name = db_vfs_current(shell_db(sh), argc > 1 ? argv[1] : "main");
    const char *file = db_filename(shell_db(sh), argc > 1 ? argv[1] : "main");

    fprintf(shell_out(sh), "vfs.zName      = \"%s\"\n", name != NULL ? name : "");
    fprintf(shell_out(sh), "file.zPath     = \"%s\"\n", file != NULL ? file : "");
    return true;
}

static bool cmd_auth(Shell *sh, int argc, char **argv)
{
    db_set_authorizer(shell_db(sh), argc > 1 && dot_boolean(sh, argv[1]), shell_out(sh));
    return true;
}

static bool cmd_progress(Shell *sh, int argc, char **argv)
{
    int nops = 0;
    bool quiet = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-quiet") == 0 || strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "-reset") == 0 || strcmp(argv[i], "--reset") == 0) {
            nops = 0;
        } else if (argv[i][0] != '-') {
            nops = (int)strtol(argv[i], NULL, 10);
        }
    }
    db_set_progress(shell_db(sh), nops, quiet, shell_out(sh));
    return true;
}

static bool cmd_trace(Shell *sh, int argc, char **argv)
{
    unsigned flags = 0u;
    const char *target = NULL;
    int i;

    if (shell_unsafe(sh, "trace")) {
        return false;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stmt") == 0) {
            flags |= DB_TRACE_STMT;
        } else if (strcmp(argv[i], "--profile") == 0) {
            flags |= DB_TRACE_PROFILE;
        } else if (strcmp(argv[i], "--row") == 0) {
            flags |= DB_TRACE_ROW;
        } else if (strcmp(argv[i], "--close") == 0) {
            flags |= DB_TRACE_CLOSE;
        } else {
            target = argv[i];
        }
    }
    if (target != NULL && strcmp(target, "off") == 0) {
        db_set_trace(shell_db(sh), 0u, NULL);
        return true;
    }
    if (flags == 0u) {
        flags = DB_TRACE_STMT;
    }
    db_set_trace(shell_db(sh), flags, shell_out(sh));
    return true;
}

static bool cmd_parameter(Shell *sh, int argc, char **argv)
{
    Db *db = shell_db(sh);

    if (argc < 2) {
        usage_error(sh, ".parameter init|list|clear|set KEY VALUE|unset KEY");
        return false;
    }
    if (strcmp(argv[1], "init") == 0) {
        return db_parameter_set(db, "$__init", "1", shell_err(sh)) &&
               db_parameter_unset(db, "$__init", shell_err(sh));
    }
    if (strcmp(argv[1], "clear") == 0) {
        return db_parameter_clear(db, shell_err(sh));
    }
    if (strcmp(argv[1], "list") == 0) {
        return shell_exec(sh, "SELECT key, quote(value) FROM temp.sqlite_parameters ORDER BY key");
    }
    if (strcmp(argv[1], "set") == 0 && argc > 3) {
        return db_parameter_set(db, argv[2], argv[3], shell_err(sh));
    }
    if (strcmp(argv[1], "unset") == 0 && argc > 2) {
        return db_parameter_unset(db, argv[2], shell_err(sh));
    }
    usage_error(sh, ".parameter init|list|clear|set KEY VALUE|unset KEY");
    return false;
}

static bool cmd_stats(Shell *sh, int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        db_print_stats(shell_db(sh), shell_out(sh), true);
        return true;
    }
    return flag_command(sh, argc, argv, SHELL_STATS);
}

static bool cmd_bail(Shell *sh, int argc, char **argv)
{
    return flag_command(sh, argc, argv, SHELL_BAIL);
}

static bool cmd_echo(Shell *sh, int argc, char **argv)
{
    return flag_command(sh, argc, argv, SHELL_ECHO);
}

static bool cmd_timer(Shell *sh, int argc, char **argv)
{
    return flag_command(sh, argc, argv, SHELL_TIMER);
}

static bool cmd_changes(Shell *sh, int argc, char **argv)
{
    return flag_command(sh, argc, argv, SHELL_CHANGES);
}

/* Safe mode is meant to survive a hostile script, so the only way out is a
 * nonce the invoking user passed on the command line. */
static bool cmd_nonce(Shell *sh, int argc, char **argv)
{
    if (argc < 2 || !shell_nonce_matches(sh, argv[1])) {
        fprintf(shell_err(sh), "incorrect nonce: \"%s\"\n", argc > 1 ? argv[1] : "");
        shell_quit(sh, 1);
        return false;
    }
    shell_set_flag(sh, SHELL_SAFE, false);
    return true;
}

/* .show prints the separators as they would be typed, so a newline reads as
 * \n rather than wrapping the listing. */
/* .show prints the explain setting as a word; SHELL_EXPLAIN_AUTO is a third
 * state, not a boolean. */
static const char *explain_word(ShellExplain explain)
{
    if (explain == SHELL_EXPLAIN_AUTO) {
        return "auto";
    }
    return explain == SHELL_EXPLAIN_ON ? "on" : "off";
}

static void show_separator(FILE *dest, const char *name, const char *text)
{
    size_t i;

    fprintf(dest, "%12s: \"", name);
    for (i = 0u; text[i] != '\0'; i++) {
        switch (text[i]) {
        case '\n':
            fputs("\\n", dest);
            break;
        case '\t':
            fputs("\\t", dest);
            break;
        case '\r':
            fputs("\\r", dest);
            break;
        case '\\':
            fputs("\\\\", dest);
            break;
        default:
            fputc(text[i], dest);
            break;
        }
    }
    fputs("\"\n", dest);
}

static bool cmd_show(Shell *sh, int argc, char **argv)
{
    const Out *out = db_out(shell_db(sh));
    FILE *dest = shell_out(sh);
    const char *path = db_path(shell_db(sh));
    const short *width;
    size_t nwidth = out_widths(out, &width);
    size_t i;

    (void)argc;
    (void)argv;
    fprintf(dest, "%12s: %s\n", "echo", shell_flag(sh, SHELL_ECHO) ? "on" : "off");
    fprintf(dest, "%12s: %s\n", "eqp", shell_flag(sh, SHELL_EQP) ? "on" : "off");
    fprintf(dest, "%12s: %s\n", "explain", explain_word(shell_explain(sh)));
    fprintf(dest, "%12s: %s\n", "headers", out_headers(out) ? "on" : "off");
    fprintf(dest, "%12s: %s\n", "mode", out_mode_name(out));
    fprintf(dest, "%12s: \"%s\"\n", "nullvalue", out_null_text(out));
    fprintf(dest, "%12s: %s\n", "output", shell_output_name(sh));
    show_separator(dest, "colseparator", out_colsep(out));
    show_separator(dest, "rowseparator", out_rowsep(out));
    fprintf(dest, "%12s: %s\n", "stats", shell_flag(sh, SHELL_STATS) ? "on" : "off");
    fprintf(dest, "%12s: ", "width");
    for (i = 0u; i < nwidth; i++) {
        fprintf(dest, "%d ", width[i]);
    }
    fputc('\n', dest);
    fprintf(dest, "%12s: %s\n", "filename", path);
    return true;
}

/* --------------------------------------------------------------------------
 * The table
 * ------------------------------------------------------------------------ */

/* What argument 1 of a command completes to. Keeping it in the table means
 * comp.c never grows a second opinion about what `.mode <tab>` should offer. */
typedef enum { A_NONE = 0, A_FILE, A_TABLE, A_MODE, A_BOOL, A_LIMIT, A_DBCONFIG } DotArgKind;

typedef struct {
    const char *name;
    DotFn fn;
    const char *args;
    const char *help;
    /* Non-NULL means the command is recognised and refused; the text names
     * the extension source it would need. */
    const char *needs;
    DotArgKind arg;
} DotCmd;

#define REFUSE(name, args, help, needs) {name, NULL, args, help, needs, A_NONE}

/* clang-format off */
/* One row per command, in the order `.help` prints them: alphabetical, the
 * way upstream's is, so the two can be diffed. */
static const DotCmd g_cmd[] = {
REFUSE("archive","...",              "Manage SQL archives",                                          "sqlar.c and zipfile.c"),
REFUSE("ar",    "...",               "Alias for .archive",                                           "sqlar.c and zipfile.c"),
{"auth",      cmd_auth,              "ON|OFF",           "Show authorizer callbacks",                NULL, A_BOOL},
{"backup",    cmd_backup,            "?DB? FILE",        "Backup DB (default \"main\") to FILE",     NULL, A_FILE},
{"bail",      cmd_bail,              "on|off",           "Stop after hitting an error",              NULL, A_BOOL},
{"binary",    cmd_binary,            "on|off",           "Turn binary output on or off",             NULL, A_BOOL},
{"cd",        cmd_cd,                "DIRECTORY",        "Change the working directory",             NULL, A_FILE},
{"changes",   cmd_changes,           "on|off",           "Show number of rows changed by SQL",       NULL, A_BOOL},
REFUSE("check","GLOB",               "Fail if output since .testcase does not match",                "the TCL test harness"),
{"clear",     cmd_clear,             "",                 "Clear the terminal screen (redstone)",        NULL, A_NONE},
{"clone",     schema_cmd_clone,      "NEWDB",            "Clone data into NEWDB from the existing database", NULL, A_FILE},
{"connection",cmd_connection,        "?close? ?NUMBER?", "Open or close an auxiliary database connection", NULL, A_NONE},
{"crlf",      cmd_crlf,              "?on|off?",         "Use \\r\\n line endings on output",        NULL, A_BOOL},
{"crnl",      cmd_crlf,              "?on|off?",         "Alias for .crlf",                          NULL, A_BOOL},
{"databases", schema_cmd_databases,  "",                 "List names and files of attached databases", NULL, A_NONE},
{"dbconfig",  cmd_dbconfig,          "?op? ?val?",       "List or change sqlite3_db_config() options", NULL, A_DBCONFIG},
{"dbinfo",    schema_cmd_dbinfo,     "?DB?",             "Show status information about the database", NULL, A_NONE},
{"dbtotxt",   schema_cmd_dbtotxt,    "",                 "Hex dump of the database file",            NULL, A_NONE},
{"dump",      schema_cmd_dump,       "?OPTIONS? ?LIKE?", "Render database content as SQL",           NULL, A_TABLE},
{"echo",      cmd_echo,              "on|off",           "Turn command echo on or off",              NULL, A_BOOL},
{"edit",      cmd_edit,              "?TEXT?",           "Edit TEXT, or the last line, in $VISUAL/$EDITOR/vi (redstone)", NULL, A_NONE},
{"editor",    cmd_editor,            "emacs|vi",         "Select the line-editor keymap (redstone)",    NULL, A_NONE},
{"eqp",       cmd_eqp,               "on|off|full",      "Enable or disable automatic EXPLAIN QUERY PLAN", NULL, A_BOOL},
{"excel",     import_cmd_excel,      "?QUERY?",          "Display the output of next command in spreadsheet", NULL, A_NONE},
{"exit",      cmd_exit,              "?CODE?",           "Exit this program with return-code CODE",  NULL, A_NONE},
REFUSE("expert","?OPTIONS?",         "Suggest indexes for queries",                                  "sqlite3expert.c"),
{"explain",   cmd_explain,           "?on|off|auto?",    "Change the EXPLAIN formatting mode",       NULL, A_BOOL},
{"filectrl",  cmd_filectrl,          "CMD ?ARG?",        "Run various sqlite3_file_control() operations", NULL, A_NONE},
{"fullschema",schema_cmd_fullschema, "?--indent?",       "Show schema and the content of sqlite_stat tables", NULL, A_NONE},
{"headers",   cmd_headers,           "on|off",           "Turn display of headers on or off",        NULL, A_BOOL},
{"help",      NULL,                  "?-all? ?PATTERN?", "Show help text for PATTERN",               NULL, A_NONE},
{"import",    import_cmd_import,     "FILE TABLE",       "Import data from FILE into TABLE",         NULL, A_FILE},
REFUSE("imposter","INDEX IMPOSTER",  "Create imposter table IMPOSTER on index INDEX",                "sqlite3_test_control()"),
{"indexes",   schema_cmd_indexes,    "?TABLE?",          "Show names of indexes",                    NULL, A_TABLE},
{"indices",   schema_cmd_indexes,    "?TABLE?",          "Alias for .indexes",                       NULL, A_TABLE},
REFUSE("intck","?STEPS_PER_UNLOCK?", "Run an incremental integrity check on the database",           "sqlite3_intck.c"),
{"limit",     cmd_limit,             "?LIMIT? ?VAL?",    "Display or change the value of an SQLITE_LIMIT", NULL, A_LIMIT},
{"limits",    cmd_limit,             "?LIMIT? ?VAL?",    "Alias for .limit",                         NULL, A_LIMIT},
{"lint",      schema_cmd_lint,       "OPTIONS",          "Report potential schema issues",           NULL, A_NONE},
{"load",      cmd_load,              "FILE ?ENTRY?",     "Load an extension library",                NULL, A_FILE},
{"log",       cmd_log,               "FILE|off",         "Turn logging on or off",                   NULL, A_FILE},
{"mode",      cmd_mode,              "?MODE? ?OPTIONS?", "Set output mode",                          NULL, A_MODE},
{"nonce",     cmd_nonce,             "STRING",           "Suspend safe mode for one command",        NULL, A_NONE},
{"nullvalue", cmd_nullvalue,         "STRING",           "Use STRING in place of NULL values",       NULL, A_NONE},
{"once",      cmd_once,              "?OPTIONS? ?FILE?", "Output for the next SQL command only to FILE", NULL, A_FILE},
{"open",      cmd_open,              "?OPTIONS? ?FILE?", "Close existing database and reopen FILE",  NULL, A_FILE},
{"output",    cmd_output,            "?FILE?",           "Send output to FILE or stdout if FILE is omitted", NULL, A_FILE},
{"parameter", cmd_parameter,         "CMD ...",          "Manage SQL parameter bindings",            NULL, A_NONE},
{"print",     cmd_print,             "STRING...",        "Print literal STRING",                     NULL, A_NONE},
{"progress",  cmd_progress,          "N",                "Invoke progress handler after every N opcodes", NULL, A_NONE},
{"prompt",    cmd_prompt,            "MAIN CONTINUE",    "Replace the standard prompts",             NULL, A_NONE},
{"quit",      cmd_quit,              "",                 "Stop interpreting input stream, exit",     NULL, A_NONE},
{"read",      cmd_read,              "FILE",             "Read input from FILE or command output",   NULL, A_FILE},
REFUSE("recover","?OPTIONS?",        "Recover as much data as possible from corrupt db",             "dbdata.c and sqlite3recover.c"),
{"restore",   cmd_restore,           "?DB? FILE",        "Restore content of DB (default \"main\") from FILE", NULL, A_FILE},
{"save",      cmd_backup,            "?DB? FILE",        "Alias for .backup",                        NULL, A_FILE},
REFUSE("scanstats","on|off|est",     "Display scanstats information",                                "an SQLITE_ENABLE_STMT_SCANSTATUS build"),
{"schema",    schema_cmd_schema,     "?PATTERN?",        "Show the CREATE statements matching PATTERN", NULL, A_TABLE},
REFUSE("selftest","?OPTIONS?",       "Run tests defined in the SELFTEST table",                      "shathree.c"),
{"separator", cmd_separator,         "COL ?ROW?",        "Change the column and row separators",     NULL, A_NONE},
REFUSE("session","?NAME? CMD ...",   "Create or control sessions",                                   "sqlite3session.c"),
REFUSE("sha3sum","?OPTIONS?",        "Compute a SHA3 hash of database content",                      "shathree.c"),
{"shell",     cmd_shell,             "CMD ARGS...",      "Run CMD ARGS... in a system shell",        NULL, A_NONE},
{"show",      cmd_show,              "",                 "Show the current values for various settings", NULL, A_NONE},
{"stats",     cmd_stats,             "?ARG?",            "Show stats or turn stats on or off",       NULL, A_BOOL},
{"system",    cmd_shell,             "CMD ARGS...",      "Run CMD ARGS... in a system shell",        NULL, A_NONE},
{"tables",    schema_cmd_tables,     "?TABLE?",          "List names of tables matching LIKE pattern TABLE", NULL, A_TABLE},
REFUSE("testcase","NAME",            "Begin redirecting output to NAME",                             "the TCL test harness"),
{"theme",     cmd_theme,             "?NAME|FILE?",      "Show, load or reload the colour theme (redstone)", NULL, A_FILE},
{"timeout",   cmd_timeout,           "MS",               "Try opening locked tables for MS milliseconds", NULL, A_NONE},
{"timer",     cmd_timer,             "on|off",           "Turn SQL timer on or off",                 NULL, A_BOOL},
{"trace",     cmd_trace,             "?OPTIONS?",        "Output each SQL statement as it is run",   NULL, A_FILE},
{"version",   cmd_version,           "",                 "Show source, library and header versions", NULL, A_NONE},
{"vfsinfo",   cmd_vfsinfo,           "?AUX?",            "Information about the top-level VFS",      NULL, A_NONE},
{"vfslist",   cmd_vfslist,           "",                 "List all available VFSes",                 NULL, A_NONE},
{"vfsname",   cmd_vfsname,           "?AUX?",            "Print the name of the VFS stack",          NULL, A_NONE},
{"width",     cmd_width,             "NUM1 NUM2 ...",    "Set minimum column widths for columnar output", NULL, A_NONE},
{"www",       import_cmd_www,        "?QUERY?",          "Display output of the next command in web browser", NULL, A_NONE},
};
/* clang-format on */

#define DOT_COUNT (sizeof(g_cmd) / sizeof(g_cmd[0]))

/* --------------------------------------------------------------------------
 * Dispatch
 * ------------------------------------------------------------------------ */

static bool cmd_help(Shell *sh, int argc, char **argv)
{
    const char *pattern = argc > 1 && argv[1][0] != '-' ? argv[1] : NULL;
    FILE *dest = shell_out(sh);
    bool found = false;
    size_t i;

    for (i = 0u; i < DOT_COUNT; i++) {
        char label[40];

        if (pattern != NULL && strstr(g_cmd[i].name, pattern) == NULL) {
            continue;
        }
        found = true;
        (void)snprintf(label, sizeof(label), ".%s %s", g_cmd[i].name, g_cmd[i].args);
        fprintf(dest, "%-28s %s\n", label, g_cmd[i].help);
        if (g_cmd[i].needs != NULL) {
            fprintf(dest, "%-28s   unsupported: needs %s\n", "", g_cmd[i].needs);
        }
    }
    if (!found) {
        fprintf(dest, "Nothing matches '%s'\n", pattern != NULL ? pattern : "");
    }
    return true;
}

/* Longest common prefix length, for suggesting what the user meant. An edit
 * distance would be better for transpositions but worse for the mistake
 * people actually make, which is stopping halfway through a name. */
static size_t common_prefix(const char *a, const char *b)
{
    size_t n = 0u;

    while (a[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

static const DotCmd *lookup(const char *name)
{
    const DotCmd *best = NULL;
    size_t best_len = 0u;
    size_t matches = 0u;
    size_t i;

    for (i = 0u; i < DOT_COUNT; i++) {
        if (strcmp(name, g_cmd[i].name) == 0) {
            return &g_cmd[i];
        }
    }
    /* Unambiguous prefixes work, as they do upstream: `.tab` is `.tables`. */
    for (i = 0u; i < DOT_COUNT; i++) {
        size_t n = strlen(name);

        if (strncmp(name, g_cmd[i].name, n) == 0) {
            matches++;
            best = &g_cmd[i];
            best_len = n;
        }
    }
    (void)best_len;
    return matches == 1u ? best : NULL;
}

static void suggest(Shell *sh, const char *name)
{
    const char *best = NULL;
    size_t best_len = 0u;
    size_t i;

    for (i = 0u; i < DOT_COUNT; i++) {
        size_t n = common_prefix(name, g_cmd[i].name);

        if (n > best_len) {
            best_len = n;
            best = g_cmd[i].name;
        }
    }
    if (best != NULL && best_len >= 2u) {
        fprintf(shell_err(sh), "redstone: unknown command: .%s -- did you mean .%s?\n", name, best);
    } else {
        fprintf(shell_err(sh), "redstone: unknown command: .%s\n", name);
    }
}

bool dot_run(Shell *sh, const char *line)
{
    char buf[4096];
    char *argv[64];
    const DotCmd *cmd;
    int argc;
    size_t n;

    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line != '.') {
        return false;
    }
    line++;
    n = strlen(line);
    if (n >= sizeof(buf)) {
        fprintf(shell_err(sh), "redstone: command too long\n");
        return false;
    }
    memcpy(buf, line, n + 1u);
    argc = dot_split(buf, argv, (int)(sizeof(argv) / sizeof(argv[0])));
    if (argc == 0) {
        return true;
    }

    cmd = lookup(argv[0]);
    if (cmd == NULL) {
        suggest(sh, argv[0]);
        return false;
    }
    if (cmd->needs != NULL) {
        fprintf(shell_err(sh),
                "redstone: .%s is not supported: it needs %s, which redstone does not vendor.\n"
                "       Use sqlite3(1) for this command.\n",
                cmd->name, cmd->needs);
        return false;
    }
    if (strcmp(cmd->name, "help") == 0) {
        return cmd_help(sh, argc, argv);
    }
    return cmd->fn(sh, argc, argv);
}

/* --------------------------------------------------------------------------
 * Completion
 * ------------------------------------------------------------------------ */

static bool source_command(size_t i, const char **name, const char **help)
{
    if (i >= DOT_COUNT) {
        return false;
    }
    *name = g_cmd[i].name;
    *help = g_cmd[i].help;
    return true;
}

static CompKind source_arg_kind(const char *name, size_t argno, const char *const **words,
                                size_t *nwords)
{
    static const char *const bools[] = {"on", "off"};
    const DotCmd *cmd = lookup(name);
    size_t n;

    /* NULL means "no fixed word list": comp.c falls through to the returned
     * kind (a table name, a pragma, ...) instead of treating this as an empty
     * fixed list and stopping there. */
    *words = NULL;
    *nwords = 0u;
    if (cmd == NULL || argno != 1u) {
        return COMP_KEYWORD;
    }
    switch (cmd->arg) {
    case A_TABLE:
        return COMP_TABLE;
    case A_MODE:
        *words = out_mode_names();
        for (n = 0u; (*words)[n] != NULL; n++) {
            /* counting */
        }
        *nwords = n;
        return COMP_KEYWORD;
    case A_BOOL:
        *words = bools;
        *nwords = 2u;
        return COMP_KEYWORD;
    case A_LIMIT:
    case A_DBCONFIG:
    case A_FILE:
    case A_NONE:
    default:
        return COMP_KEYWORD;
    }
}

const CompDotSource *dot_comp_source(void)
{
    static const CompDotSource source = {source_command, source_arg_kind};

    return &source;
}
