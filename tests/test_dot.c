/*
 * Tests for the dot-command surface: dot.c, schema.c and import.c.
 *
 * The output-parity suite (tests/parity.sh) already diffs the schema and dump
 * commands against sqlite3(1) byte for byte, so nothing here repeats that.
 * What it cannot check is behaviour that has no counterpart to diff against:
 * that a refused command says so instead of failing quietly, that a failed
 * .open leaves the session usable, that a typo is met with a suggestion, and
 * that .import survives the CSV a spreadsheet actually produces.
 *
 * Each test drives a real Shell through shell_feed, which is the same entry
 * point the REPL, `.read` and the init files use.
 */
#include "comp.h"
#include "db.h"
#include "dot.h"
#include "line.h"
#include "minunit.h"
#include "out.h"
#include "plat.h"
#include "shell.h"
#include "sqlctx.h"
#include "suites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A session whose two output streams are temporary files, so a test can read
 * back everything a command wrote. */
typedef struct {
    Shell *sh;
    FILE *out;
    FILE *err;
} Fix;

static void fix_close(Fix *f)
{
    shell_free(f->sh);
    f->sh = NULL;
    if (f->out != NULL) {
        fclose(f->out);
        f->out = NULL;
    }
    if (f->err != NULL) {
        fclose(f->err);
        f->err = NULL;
    }
}

static bool fix_open(Fix *f)
{
    Db *db = db_open(NULL, stderr);
    Line *ln = line_new(stdin, stdout);

    f->sh = NULL;
    f->out = tmpfile();
    f->err = tmpfile();
    if (db != NULL && ln != NULL && f->out != NULL && f->err != NULL) {
        f->sh = shell_new(stdin, f->out, f->err);
    }
    if (f->sh == NULL) {
        db_close(db);
        line_free(ln);
        fix_close(f);
        return false;
    }
    shell_adopt(f->sh, db, ln);
    /* Colour would put escape sequences in every expectation below. */
    out_set_colour(db_out(db), false);
    return true;
}

/* Everything written to STREAM since the last call, as a NUL-terminated
 * string the caller frees. The stream is emptied, so each test can compare
 * whole outputs rather than accumulating everything that came before. */
static char *drain(FILE *stream)
{
    char *buf;
    long size;
    size_t got;

    fflush(stream);
    if (fseek(stream, 0L, SEEK_END) != 0) {
        return NULL;
    }
    size = ftell(stream);
    if (size < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        return NULL;
    }
    got = fread(buf, 1u, (size_t)size, stream);
    buf[got] = '\0';
    /* Empty the file so the next call returns only what is written after
     * this one. Both steps are checked: a half-reset stream would silently
     * prepend old output to a later comparison. */
    if (ftruncate(fileno(stream), 0) != 0 || fseek(stream, 0L, SEEK_SET) != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Drop whatever a stream holds: the setup statements before the part of the
 * output a test actually compares. */
static void discard(FILE *stream)
{
    free(drain(stream));
}

/* True when TEXT contains NEEDLE. Used rather than a whole-output compare
 * wherever the surrounding text is sqlite3(1)'s and parity.sh owns it. */
static bool has(const char *text, const char *needle)
{
    return text != NULL && strstr(text, needle) != NULL;
}

#ifndef _WIN32 /* only the POSIX-only .edit tests need it */
/* Reads PATH fully into a NUL-terminated buffer the caller frees, or NULL on
 * any failure. Used to see what .edit's capture-script editor received. */
static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0L, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    return buf;
}
#endif

/* --------------------------------------------------------------------------
 * Argument splitting
 * ------------------------------------------------------------------------ */

static const char *test_split_quoting(void)
{
    /* dot_run strips the leading dot before splitting, so the first word here
     * is the command name already. */
    char line[] = "import 'a file.csv' \"t\\\"x\" plain";
    char *argv[8];
    int n = dot_split(line, argv, 8);

    mu_assert("four arguments expected", n == 4);
    mu_assert("the command name is the first argument", strcmp(argv[0], "import") == 0);
    mu_assert("single quotes group words", strcmp(argv[1], "a file.csv") == 0);
    mu_assert("backslash escapes the quote", strcmp(argv[2], "t\"x") == 0);
    mu_assert("bare word passes through", strcmp(argv[3], "plain") == 0);
    return NULL;
}

static const char *test_split_limits(void)
{
    char empty[] = "   ";
    char many[] = "x a b c d";
    char *argv[3];

    mu_assert("blank line has no arguments", dot_split(empty, argv, 3) == 0);
    /* Upstream stops at the cap rather than overflowing; so do we. */
    mu_assert("argument count is capped", dot_split(many, argv, 3) == 3);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Refusals and typos
 * ------------------------------------------------------------------------ */

/* Every command redstone refuses. Each is refused for a named missing source
 * file, never silently ignored, because a script that thinks .sha3sum ran is
 * worse off than one that stopped. */
static const char *test_refusals(void)
{
    static const char *const refused[] = {"archive",  "ar",      "check",   "expert",
                                          "imposter", "intck",   "recover", "scanstats",
                                          "selftest", "session", "sha3sum", "testcase"};
    Fix f;
    size_t i;

    mu_assert("fixture failed", fix_open(&f));
    for (i = 0u; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char cmd[32];
        char *err;

        (void)snprintf(cmd, sizeof(cmd), ".%s", refused[i]);
        mu_assert("a refused command must report failure", !shell_feed(f.sh, cmd));
        err = drain(f.err);
        mu_assert("refusal must name the command and the reason",
                  has(err, refused[i]) && has(err, "is not supported: it needs"));
        free(err);
    }
    fix_close(&f);
    return NULL;
}

static const char *test_unknown_command_suggests(void)
{
    Fix f;
    char *err;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert("an unknown command fails", !shell_feed(f.sh, ".tabels"));
    err = drain(f.err);
    mu_assert("the nearest command is suggested", has(err, ".tables"));
    free(err);
    fix_close(&f);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Settings that change what the next statement prints
 * ------------------------------------------------------------------------ */

static const char *test_mode_and_headers(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert(".mode list failed", shell_feed(f.sh, ".mode list"));
    mu_assert(".headers on failed", shell_feed(f.sh, ".headers on"));
    mu_assert("query failed", shell_feed(f.sh, "SELECT 1 AS a, 2 AS b;"));
    text = drain(f.out);
    mu_assert("headers then row, list mode", text != NULL && strcmp(text, "a|b\n1|2\n") == 0);
    free(text);

    mu_assert(".headers off failed", shell_feed(f.sh, ".headers off"));
    mu_assert("query failed", shell_feed(f.sh, "SELECT 3 AS a;"));
    text = drain(f.out);
    mu_assert("no header the second time", text != NULL && strcmp(text, "3\n") == 0);
    free(text);
    fix_close(&f);
    return NULL;
}

/* ".separator COL ROW" sets only the column separator. The 3.53.3 binary
 * accepts the second argument and ignores it; -newline and ".mode --rowsep"
 * are the ways to change the row separator there. */
static const char *test_separator_ignores_rowsep(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert(".mode list failed", shell_feed(f.sh, ".mode list"));
    mu_assert(".separator failed", shell_feed(f.sh, ".separator : ;"));
    mu_assert("query failed", shell_feed(f.sh, "SELECT 1, 2;"));
    text = drain(f.out);
    mu_assert("column separator changed, row separator did not",
              text != NULL && strcmp(text, "1:2\n") == 0);
    free(text);
    fix_close(&f);
    return NULL;
}

static const char *test_show_reports_settings(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert(".mode csv failed", shell_feed(f.sh, ".mode csv"));
    mu_assert(".nullvalue failed", shell_feed(f.sh, ".nullvalue NIL"));
    mu_assert(".width failed", shell_feed(f.sh, ".width 3 4"));
    discard(f.out);
    mu_assert(".show failed", shell_feed(f.sh, ".show"));
    text = drain(f.out);
    mu_assert("mode is reported", has(text, "mode: csv"));
    mu_assert("nullvalue is reported", has(text, "nullvalue: \"NIL\""));
    mu_assert("widths are reported", has(text, "width: 3 4"));
    /* Separators are escaped rather than printed raw, or the report would
     * span lines of its own. */
    mu_assert("row separator is escaped", has(text, "rowseparator: \"\\r\\n\""));
    free(text);
    fix_close(&f);
    return NULL;
}

/* --------------------------------------------------------------------------
 * The session survives a failure
 * ------------------------------------------------------------------------ */

static const char *test_open_failure_keeps_session(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE TABLE keep (x);"));
    mu_assert("a bad path must fail", !shell_feed(f.sh, ".open /nonexistent-dir/nope.db"));
    discard(f.err);

    mu_assert(".mode list failed", shell_feed(f.sh, ".mode list"));
    mu_assert(".headers off failed", shell_feed(f.sh, ".headers off"));
    discard(f.out);
    /* The previous connection is still the current one: its table is there. */
    mu_assert("session still usable", shell_feed(f.sh, "SELECT count(*) FROM keep;"));
    text = drain(f.out);
    mu_assert("the old connection survived", text != NULL && strcmp(text, "0\n") == 0);
    free(text);
    fix_close(&f);
    return NULL;
}

/* --------------------------------------------------------------------------
 * .import
 * ------------------------------------------------------------------------ */

/* The CSV a spreadsheet produces: a quoted field with a comma, one with a
 * doubled quote, and one with an embedded newline. RFC 4180 says all three
 * are one field each; a naive split on ',' gets every one of them wrong. */
static const char *test_import_csv_quoting(void)
{
    static const char *const csv = "a,b\n"
                                   "\"one,two\",plain\n"
                                   "\"say \"\"hi\"\"\",x\n"
                                   "\"line\nbreak\",y\n";
    Fix f;
    FILE *file;
    char path[] = "/tmp/redstone-import-XXXXXX";
    char cmd[128];
    char *text;
    int fd = mkstemp(path);

    mu_assert("mkstemp failed", fd >= 0);
    file = fdopen(fd, "w");
    mu_assert("fdopen failed", file != NULL);
    (void)fputs(csv, file);
    (void)fclose(file);

    mu_assert("fixture failed", fix_open(&f));
    (void)snprintf(cmd, sizeof(cmd), ".import --csv %s t", path);
    mu_assert(".import failed", shell_feed(f.sh, cmd));
    mu_assert(".mode list failed", shell_feed(f.sh, ".mode list"));
    mu_assert(".headers off failed", shell_feed(f.sh, ".headers off"));
    discard(f.out);

    mu_assert("query failed", shell_feed(f.sh, "SELECT count(*) FROM t;"));
    text = drain(f.out);
    mu_assert("three rows, the header having named the columns",
              text != NULL && strcmp(text, "3\n") == 0);
    free(text);

    mu_assert("query failed", shell_feed(f.sh, "SELECT a FROM t ORDER BY rowid;"));
    text = drain(f.out);
    mu_assert("quoted commas, quotes and newlines survive",
              text != NULL && strcmp(text, "one,two\nsay \"hi\"\nline\nbreak\n") == 0);
    free(text);

    fix_close(&f);
    (void)remove(path);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Schema commands, on a schema built here
 * ------------------------------------------------------------------------ */

static const char *test_tables_and_indexes(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE TABLE alpha (x, y);"));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE TABLE beta (z);"));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE INDEX beta_z ON beta(z);"));
    discard(f.out);

    mu_assert(".tables failed", shell_feed(f.sh, ".tables"));
    text = drain(f.out);
    /* Column-aligned in a single row, as sqlite3(1) prints it. */
    mu_assert("both tables listed in one row",
              text != NULL && strcmp(text, "alpha     beta\n") == 0);
    free(text);

    mu_assert(".tables with a pattern failed", shell_feed(f.sh, ".tables al%"));
    text = drain(f.out);
    mu_assert("the pattern filters", text != NULL && strcmp(text, "alpha\n") == 0);
    free(text);

    mu_assert(".indexes failed", shell_feed(f.sh, ".indexes beta"));
    text = drain(f.out);
    mu_assert("the index is listed", text != NULL && strcmp(text, "beta_z\n") == 0);
    free(text);
    fix_close(&f);
    return NULL;
}

/* .dump has to be replayable. Rather than diff it (parity.sh does that), feed
 * it back into a second session and compare what the two databases hold. */
static const char *test_dump_replays(void)
{
    Fix src;
    Fix dst;
    char *sql;
    char *text;

    mu_assert("fixture failed", fix_open(&src));
    mu_assert("setup failed", shell_feed(src.sh, "CREATE TABLE t (a TEXT, b BLOB);"));
    mu_assert("setup failed", shell_feed(src.sh, "INSERT INTO t VALUES ('it''s', x'00ff');"));
    mu_assert("setup failed", shell_feed(src.sh, "INSERT INTO t VALUES (NULL, NULL);"));
    discard(src.out);
    mu_assert(".dump failed", shell_feed(src.sh, ".dump"));
    sql = drain(src.out);
    mu_assert("dump produced nothing", sql != NULL && sql[0] != '\0');
    fix_close(&src);

    mu_assert("fixture failed", fix_open(&dst));
    mu_assert("replay failed", shell_feed(dst.sh, sql));
    mu_assert(".mode list failed", shell_feed(dst.sh, ".mode list"));
    mu_assert(".headers off failed", shell_feed(dst.sh, ".headers off"));
    discard(dst.out);
    mu_assert("query failed", shell_feed(dst.sh, "SELECT quote(a), quote(b) FROM t;"));
    text = drain(dst.out);
    mu_assert("values survive the round trip",
              text != NULL && strcmp(text, "'it''s'|X'00FF'\nNULL|NULL\n") == 0);
    free(text);
    free(sql);
    fix_close(&dst);
    return NULL;
}

/* .lint fkey-indexes reports a foreign key whose child columns have no index,
 * and says nothing once the index exists. */
static const char *test_lint_fkey_indexes(void)
{
    Fix f;
    char *text;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE TABLE p (id INTEGER PRIMARY KEY);"));
    mu_assert("setup failed", shell_feed(f.sh, "CREATE TABLE c (pid REFERENCES p(id));"));
    discard(f.out);

    mu_assert(".lint failed", shell_feed(f.sh, ".lint fkey-indexes"));
    text = drain(f.out);
    mu_assert("the missing index is suggested",
              text != NULL && strcmp(text, "CREATE INDEX 'c_pid' ON 'c'('pid'); --> p(id)\n") == 0);
    free(text);

    mu_assert("setup failed", shell_feed(f.sh, "CREATE INDEX c_pid ON c(pid);"));
    discard(f.out);
    mu_assert(".lint failed", shell_feed(f.sh, ".lint fkey-indexes"));
    text = drain(f.out);
    mu_assert("nothing left to report", text != NULL && text[0] == '\0');
    free(text);
    fix_close(&f);
    return NULL;
}

/* --------------------------------------------------------------------------
 * .edit / .clear
 * ------------------------------------------------------------------------ */

/* The .edit tests stand in a #!/bin/sh script for the editor, which cmd.exe
 * cannot run; on Windows .edit is covered by the manual checks instead. */
#ifndef _WIN32

/* Writes a shell script to PATH that copies its $1 argument (the temp file
 * .edit hands the editor) to CAPTURE, so a test can see what text .edit
 * actually sent out without needing a real editor. */
static bool write_capture_editor(const char *path, const char *capture)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        return false;
    }
    fprintf(f, "#!/bin/sh\ncp \"$1\" \"%s\"\n", capture);
    fclose(f);
    return chmod(path, 0700) == 0;
}

/* Writes a shell script to PATH that deletes its $1 argument, so the round
 * trip has no file left to read back. */
static bool write_delete_editor(const char *path)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        return false;
    }
    fputs("#!/bin/sh\nrm -f \"$1\"\n", f);
    fclose(f);
    return chmod(path, 0700) == 0;
}

#endif

static const char *test_clear_writes_to_base_out(void)
{
    Fix f;
    char *out;
    bool ok;

    mu_assert("fixture failed", fix_open(&f));
    mu_assert(".clear should succeed", shell_feed(f.sh, ".clear"));
    out = drain(f.out);
    ok = out != NULL && strcmp(out, "\x1b[H\x1b[2J") == 0;
    free(out);
    fix_close(&f);

    mu_assert(".clear should write the clear-screen escape sequence", ok);
    return NULL;
}

#ifndef _WIN32
static const char *test_edit_sends_argument_to_editor(void)
{
    char script[] = "/tmp/redstone-test-editor-XXXXXX";
    char capture[] = "/tmp/redstone-test-capture-XXXXXX";
    int sfd = mkstemp(script);
    int cfd = mkstemp(capture);
    Fix f;
    char *got = NULL;
    bool fed = false;

    if (sfd >= 0) {
        close(sfd);
    }
    if (cfd >= 0) {
        close(cfd);
    }
    mu_assert("setup failed", sfd >= 0 && cfd >= 0 && write_capture_editor(script, capture));
    plat_unsetenv("VISUAL"); /* $VISUAL, if set, would win over $EDITOR */
    plat_setenv("EDITOR", script);

    mu_assert("fixture failed", fix_open(&f));
    fed = shell_feed(f.sh, ".edit SELECT 1;");
    discard(f.out);
    discard(f.err);
    fix_close(&f);

    got = read_whole_file(capture);
    unlink(script);
    unlink(capture);
    plat_unsetenv("EDITOR");

    mu_assert(".edit with an argument should succeed", fed);
    mu_assert(".edit should hand the argument text to the editor",
              got != NULL && has(got, "SELECT 1;"));
    free(got);
    return NULL;
}

/* With no argument .edit reaches back into history for "what the user typed
 * last". In the real REPL, history already holds ".edit" itself by the time
 * the command runs (shell_run adds every accepted line, including dot
 * commands, before dispatching), so "previous" means index count-2. shell_feed
 * alone never touches history, so the test adds both entries itself to
 * reproduce that shape. */
static const char *test_edit_falls_back_to_history(void)
{
    char script[] = "/tmp/redstone-test-editor-XXXXXX";
    char capture[] = "/tmp/redstone-test-capture-XXXXXX";
    int sfd = mkstemp(script);
    int cfd = mkstemp(capture);
    Fix f;
    char *got = NULL;
    bool fed = false;

    if (sfd >= 0) {
        close(sfd);
    }
    if (cfd >= 0) {
        close(cfd);
    }
    mu_assert("setup failed", sfd >= 0 && cfd >= 0 && write_capture_editor(script, capture));
    plat_unsetenv("VISUAL"); /* $VISUAL, if set, would win over $EDITOR */
    plat_setenv("EDITOR", script);

    mu_assert("fixture failed", fix_open(&f));
    mu_assert("history setup failed", line_history_add(shell_line(f.sh), "SELECT 2;"));
    mu_assert("history setup failed", line_history_add(shell_line(f.sh), ".edit"));
    fed = shell_feed(f.sh, ".edit");
    discard(f.out);
    discard(f.err);
    fix_close(&f);

    got = read_whole_file(capture);
    unlink(script);
    unlink(capture);
    plat_unsetenv("EDITOR");

    mu_assert(".edit with no argument should succeed", fed);
    mu_assert(".edit should fall back to the previous history entry",
              got != NULL && has(got, "SELECT 2;"));
    free(got);
    return NULL;
}

/* A nonzero exit from the editor is deliberately not treated as failure (see
 * the comment on line_external_edit in src/line.c -- upstream's edit() does
 * not check it either), so the only way to make the round trip fail is an
 * editor that leaves no file behind for read_file to read back. */
static const char *test_edit_reports_editor_failure(void)
{
    char script[] = "/tmp/redstone-test-editor-XXXXXX";
    int sfd = mkstemp(script);
    Fix f;
    char *err;
    bool fed;

    if (sfd >= 0) {
        close(sfd);
    }
    mu_assert("setup failed", sfd >= 0 && write_delete_editor(script));

    plat_unsetenv("VISUAL"); /* $VISUAL, if set, would win over $EDITOR */
    plat_setenv("EDITOR", script);
    mu_assert("fixture failed", fix_open(&f));
    fed = shell_feed(f.sh, ".edit SELECT 1;");
    discard(f.out);
    err = drain(f.err);
    fix_close(&f);
    plat_unsetenv("EDITOR");
    unlink(script);

    mu_assert(".edit should fail when the editor leaves no file behind", !fed);
    mu_assert("the failure should say why", has(err, "EDITOR"));
    free(err);
    return NULL;
}
#endif

/* --------------------------------------------------------------------------
 * Safe mode
 * ------------------------------------------------------------------------ */

/* -safe exists so that a session can be handed an untrusted script. Each of
 * these would write to, read from or execute something outside the database. */
static const char *test_safe_mode_refuses(void)
{
    static const char *const unsafe[] = {".open x.db",    ".shell echo hi",  ".system echo hi",
                                         ".output f.txt", ".import f.csv t", ".load ext",
                                         ".edit x"};
    Fix f;
    size_t i;

    mu_assert("fixture failed", fix_open(&f));
    shell_set_flag(f.sh, SHELL_SAFE, true);
    for (i = 0u; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        char *err;

        mu_assert("safe mode must refuse", !shell_feed(f.sh, unsafe[i]));
        err = drain(f.err);
        mu_assert("the refusal must say why", has(err, "safe mode"));
        free(err);
    }
    fix_close(&f);
    return NULL;
}

/* Regression: source_arg_kind() used to hand back a non-NULL empty word list
 * for every dot argument, including A_TABLE ones, so comp.c's "a fixed word
 * list wins" check always fired and .schema/.tables/.indexes/.dump never
 * reached the table-name fallback. */
static const char *test_table_arg_completion(void)
{
    Fix f;
    Db *db;
    CompList *list;
    SqlContext ctx;
    bool ok;

    mu_assert("fixture failed", fix_open(&f));
    db = shell_db(f.sh);
    mu_assert("setup failed", db_exec(db, "CREATE TABLE widgets (id INTEGER);", stdout, stderr));

    sql_context(".schema wid", 11u, &ctx);
    list = comp_generate(db, &ctx, dot_comp_source());
    ok = comp_count(list) == 1u && strcmp(comp_at(list, 0u)->display, "widgets") == 0 &&
         comp_at(list, 0u)->kind == COMP_TABLE;
    comp_free(list);
    fix_close(&f);

    mu_assert(".schema should suggest table names", ok);
    return NULL;
}

const char *dot_suite(void)
{
    mu_run_test(test_split_quoting);
    mu_run_test(test_split_limits);
    mu_run_test(test_refusals);
    mu_run_test(test_unknown_command_suggests);
    mu_run_test(test_mode_and_headers);
    mu_run_test(test_separator_ignores_rowsep);
    mu_run_test(test_show_reports_settings);
    mu_run_test(test_open_failure_keeps_session);
    mu_run_test(test_import_csv_quoting);
    mu_run_test(test_tables_and_indexes);
    mu_run_test(test_dump_replays);
    mu_run_test(test_lint_fkey_indexes);
    mu_run_test(test_clear_writes_to_base_out);
#ifndef _WIN32
    mu_run_test(test_edit_sends_argument_to_editor);
    mu_run_test(test_edit_falls_back_to_history);
    mu_run_test(test_edit_reports_editor_failure);
#endif
    mu_run_test(test_safe_mode_refuses);
    mu_run_test(test_table_arg_completion);
    return NULL;
}
