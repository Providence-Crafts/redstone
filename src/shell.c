/*
 * shell.c - the session: state, statement execution and the REPL.
 *
 * What lives here is everything that outlives one input line. The output
 * redirect is the clearest case: `.output FILE` must hold until it is turned
 * off, so the stream results go to cannot be a parameter threaded down from
 * the read loop, which is how main.c did it before Phase 6.
 */
#include "shell.h"

#include "dot.h"
#include "out.h"

#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>

/* Growable line accumulator for multi-line statements. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

struct Shell {
    Db *conn[SHELL_MAX_CONN];
    size_t conn_cur;
    Line *ln;
    FILE *in;
    FILE *out; /* the base stream */
    FILE *err;
    FILE *redirect; /* the active redirect, or NULL */
    char *redirect_name;
    bool redirect_pipe;
    bool redirect_once;

    bool flag[SHELL_FLAG_COUNT];
    ShellExplain explain;
    char *prompt_main;
    char *prompt_cont;

    FILE *log;
    bool log_owned;
    char *log_name;

    /* The only key that reopens safe mode, handed in on the command line by
     * the user who turned it on. */
    char *nonce;

    Buffer stmt;
    bool quitting;
    int status;
};

/* strdup is not C99. */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *copy = malloc(n);

    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

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
        size_t cap = buf->cap == 0u ? 256u : buf->cap;
        char *data;

        while (cap < buf->len + add + 1u) {
            cap *= 2u;
        }
        data = realloc(buf->data, cap);
        if (data == NULL) {
            return false;
        }
        buf->data = data;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, text, add + 1u);
    buf->len += add;
    return true;
}

static const char *skip_space(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') {
        text++;
    }
    return text;
}

Shell *shell_new(FILE *in, FILE *out, FILE *err)
{
    Shell *sh = calloc(1u, sizeof(*sh));

    if (sh == NULL) {
        return NULL;
    }
    sh->in = in;
    sh->out = out;
    sh->err = err;
    sh->explain = SHELL_EXPLAIN_AUTO;
    sh->prompt_main = dup_str("redstone> ");
    sh->prompt_cont = dup_str("   ...> ");
    if (sh->prompt_main == NULL || sh->prompt_cont == NULL) {
        shell_free(sh);
        return NULL;
    }
    return sh;
}

void shell_free(Shell *sh)
{
    size_t i;

    if (sh == NULL) {
        return;
    }

    (void)shell_redirect(sh, NULL, false);
    buffer_free(&sh->stmt);
    (void)shell_set_log(sh, NULL);
    free(sh->nonce);
    free(sh->prompt_main);
    free(sh->prompt_cont);
    line_free(sh->ln);
    for (i = 0u; i < SHELL_MAX_CONN; i++) {
        db_close(sh->conn[i]);
    }
    free(sh);
}

void shell_adopt(Shell *sh, Db *db, Line *ln)
{
    sh->conn[0] = db;
    sh->conn_cur = 0u;
    sh->ln = ln;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
Db *shell_db(Shell *sh)
{
    return sh->conn[sh->conn_cur];
}

size_t shell_conn_count(const Shell *sh)
{
    size_t n = 0u;
    size_t i;

    for (i = 0u; i < SHELL_MAX_CONN; i++) {
        if (sh->conn[i] != NULL) {
            n++;
        }
    }
    return n;
}

size_t shell_conn_current(const Shell *sh)
{
    return sh->conn_cur;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
const char *shell_conn_path(Shell *sh, size_t i)
{
    return (i < SHELL_MAX_CONN && sh->conn[i] != NULL) ? db_path(sh->conn[i]) : NULL;
}

bool shell_conn_switch(Shell *sh, size_t i)
{
    Db *target;

    if (i >= SHELL_MAX_CONN) {
        fprintf(sh->err, "redstone: connection number must be between 0 and %d\n",
                SHELL_MAX_CONN - 1);
        return false;
    }
    if (i == sh->conn_cur) {
        return true;
    }
    if (sh->conn[i] == NULL) {
        /* An unused slot opens an empty in-memory database, which is what
         * upstream does: switching is never an error you have to recover. */
        sh->conn[i] = db_open(NULL, sh->err);
        if (sh->conn[i] == NULL) {
            return false;
        }
    }
    target = sh->conn[i];
    db_take_out(target, sh->conn[sh->conn_cur]);
    sh->conn_cur = i;
    return true;
}

bool shell_conn_close(Shell *sh, size_t i)
{
    if (i >= SHELL_MAX_CONN || sh->conn[i] == NULL) {
        fprintf(sh->err, "redstone: no such connection: %u\n", (unsigned)i);
        return false;
    }
    if (i == sh->conn_cur) {
        fprintf(sh->err, "redstone: cannot close the current connection\n");
        return false;
    }
    db_close(sh->conn[i]);
    sh->conn[i] = NULL;
    return true;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
Line *shell_line(Shell *sh)
{
    return sh->ln;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
FILE *shell_out(Shell *sh)
{
    return sh->redirect != NULL ? sh->redirect : sh->out;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
FILE *shell_err(Shell *sh)
{
    return sh->err;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
FILE *shell_base_out(Shell *sh)
{
    return sh->out;
}

/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
const char *shell_output_name(Shell *sh)
{
    return sh->redirect_name != NULL ? sh->redirect_name : "stdout";
}

bool shell_redirect(Shell *sh, const char *target, bool once)
{
    FILE *stream;
    bool pipe_target;

    if (sh->redirect != NULL) {
        if (sh->redirect_pipe) {
            (void)pclose(sh->redirect);
        } else {
            (void)fclose(sh->redirect);
        }
        sh->redirect = NULL;
        free(sh->redirect_name);
        sh->redirect_name = NULL;
        sh->redirect_pipe = false;
        sh->redirect_once = false;
    }
    if (target == NULL || strcmp(target, "stdout") == 0) {
        return true;
    }

    pipe_target = target[0] == '|';
    if (pipe_target) {
        /* ".output |CMD" is a documented sqlite3(1) feature; shell_unsafe
         * rejects it under -safe. */
        /* NOLINTNEXTLINE(cert-env33-c) */
        stream = popen(target + 1, "w");
    } else {
        stream = fopen(target, "wb");
    }
    if (stream == NULL) {
        fprintf(sh->err, "redstone: cannot write to %s\n", target);
        return false;
    }
    sh->redirect_name = dup_str(target);
    if (sh->redirect_name == NULL) {
        if (pipe_target) {
            (void)pclose(stream);
        } else {
            (void)fclose(stream);
        }
        return false;
    }
    sh->redirect = stream;
    sh->redirect_pipe = pipe_target;
    sh->redirect_once = once;
    return true;
}

/* cppcheck-suppress staticFunction ; the other half of shell_redirect_to;
 * dot.c's .once ends its redirect through shell.c's own statement loop */
void shell_redirect_done(Shell *sh)
{
    if (sh->redirect != NULL && sh->redirect_once) {
        (void)shell_redirect(sh, NULL, false);
    }
}

bool shell_flag(const Shell *sh, ShellFlag flag)
{
    return sh->flag[flag];
}

void shell_set_flag(Shell *sh, ShellFlag flag, bool on)
{
    sh->flag[flag] = on;
}

void shell_set_explain(Shell *sh, ShellExplain mode)
{
    sh->explain = mode;
}

ShellExplain shell_explain(const Shell *sh)
{
    return sh->explain;
}

void shell_set_prompt(Shell *sh, const char *main_prompt, const char *continuation)
{
    char *copy;

    if (main_prompt != NULL) {
        copy = dup_str(main_prompt);
        if (copy != NULL) {
            free(sh->prompt_main);
            sh->prompt_main = copy;
        }
    }
    if (continuation != NULL) {
        copy = dup_str(continuation);
        if (copy != NULL) {
            free(sh->prompt_cont);
            sh->prompt_cont = copy;
        }
    }
}

const char *shell_prompt(const Shell *sh, bool continuation)
{
    return continuation ? sh->prompt_cont : sh->prompt_main;
}

bool shell_set_log(Shell *sh, const char *target)
{
    FILE *stream = NULL;
    bool owned = false;

    if (sh->log_owned && sh->log != NULL) {
        (void)fclose(sh->log);
    }
    sh->log = NULL;
    sh->log_owned = false;
    free(sh->log_name);
    sh->log_name = NULL;

    if (target == NULL || strcmp(target, "off") == 0) {
        return true;
    }
    if (strcmp(target, "stdout") == 0) {
        stream = sh->out;
    } else if (strcmp(target, "stderr") == 0) {
        stream = sh->err;
    } else {
        stream = fopen(target, "a");
        owned = true;
    }
    if (stream == NULL) {
        fprintf(sh->err, "redstone: cannot open %s\n", target);
        return false;
    }
    sh->log = stream;
    sh->log_owned = owned;
    sh->log_name = dup_str(target);
    return true;
}

const char *shell_log_name(const Shell *sh)
{
    return sh->log_name != NULL ? sh->log_name : "off";
}

bool shell_unsafe(Shell *sh, const char *name)
{
    if (!sh->flag[SHELL_SAFE]) {
        return false;
    }
    fprintf(sh->err, "redstone: .%s is prohibited in safe mode\n", name);
    return true;
}

void shell_set_nonce(Shell *sh, const char *nonce)
{
    free(sh->nonce);
    sh->nonce = nonce != NULL ? dup_str(nonce) : NULL;
}

bool shell_nonce_matches(const Shell *sh, const char *text)
{
    return sh->nonce != NULL && text != NULL && strcmp(sh->nonce, text) == 0;
}

bool shell_quitting(const Shell *sh)
{
    return sh->quitting;
}

void shell_quit(Shell *sh, int status)
{
    sh->quitting = true;
    sh->status = status;
}

int shell_status(const Shell *sh)
{
    return sh->status;
}

/* --------------------------------------------------------------------------
 * Running statements
 * ------------------------------------------------------------------------ */

typedef struct {
    struct timeval wall;
    struct timeval user;
    struct timeval sys;
} Clocks;

static void clocks_read(Clocks *c)
{
    struct rusage ru;

    (void)gettimeofday(&c->wall, NULL);
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        c->user = ru.ru_utime;
        c->sys = ru.ru_stime;
    } else {
        c->user.tv_sec = c->user.tv_usec = 0;
        c->sys.tv_sec = c->sys.tv_usec = 0;
    }
}

static double elapsed(const struct timeval *start, const struct timeval *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           ((double)(end->tv_usec - start->tv_usec) / 1.0e6);
}

/* sqlite3(1)'s wording, so a script that greps "Run Time" keeps working. */
static void report_time(FILE *out, const Clocks *start, const Clocks *end)
{
    fprintf(out, "Run Time: real %.3f user %f sys %f\n", elapsed(&start->wall, &end->wall),
            elapsed(&start->user, &end->user), elapsed(&start->sys, &end->sys));
}

/* True for a statement that begins with EXPLAIN but not EXPLAIN QUERY PLAN:
 * the first prints the opcode table that needs the fixed layout, the second
 * prints ordinary rows. */
static bool is_explain(const char *sql)
{
    static const char word[] = "explain";
    size_t i;

    sql = skip_space(sql);
    for (i = 0u; word[i] != '\0'; i++) {
        if ((sql[i] | 0x20) != word[i]) {
            return false;
        }
    }
    sql = skip_space(sql + i);
    return (sql[0] | 0x20) != 'q';
}

/* Upstream's EXPLAIN layout. The widths are what turn the opcode dump into
 * something readable; without them every column is as wide as its widest
 * value and the addresses wander. */
static void explain_layout(Shell *sh, bool on, char *saved, size_t cap)
{
    Out *out = db_out(shell_db(sh));

    if (on) {
        const char *widths[2];

        (void)snprintf(saved, cap, "%s", out_mode_name(out));
        (void)out_set_mode(out, "column");
        widths[0] = "--widths";
        widths[1] = "4,13,4,4,4,13,2,13";
        (void)out_command(out, 2, widths, sh->err);
        out_set_headers(out, true);
    } else {
        (void)out_set_mode(out, saved);
    }
}

/* `.eqp on` prints the plan before the statement, the way upstream does, by
 * running the same text as an EXPLAIN QUERY PLAN. */
static void print_plan(Shell *sh, const char *sql, FILE *dest)
{
    static const char prefix[] = "EXPLAIN QUERY PLAN ";
    size_t n = sizeof(prefix) + strlen(sql);
    char *text = malloc(n);

    if (text == NULL) {
        return;
    }
    (void)snprintf(text, n, "%s%s", prefix, sql);
    (void)db_exec(shell_db(sh), text, dest, sh->err);
    free(text);
}

bool shell_exec(Shell *sh, const char *sql)
{
    FILE *dest = shell_out(sh);
    char saved_mode[32];
    bool explaining;
    Clocks start;
    Clocks end;
    bool ok;

    explaining =
        sh->explain == SHELL_EXPLAIN_ON || (sh->explain == SHELL_EXPLAIN_AUTO && is_explain(sql));

    if (sh->flag[SHELL_ECHO]) {
        fprintf(dest, "%s\n", sql);
    }
    if (sh->flag[SHELL_TIMER]) {
        clocks_read(&start);
    }

    if (sh->flag[SHELL_EQP] && !is_explain(sql)) {
        print_plan(sh, sql, dest);
    }
    if (explaining) {
        explain_layout(sh, true, saved_mode, sizeof(saved_mode));
    }

    ok = db_exec(shell_db(sh), sql, dest, sh->err);

    if (explaining) {
        explain_layout(sh, false, saved_mode, sizeof(saved_mode));
    }

    if (sh->flag[SHELL_TIMER]) {
        clocks_read(&end);
        report_time(sh->out, &start, &end);
    }
    if (sh->flag[SHELL_CHANGES]) {
        fprintf(dest, "changes: %d   total_changes: %lld\n", db_changes(shell_db(sh)),
                db_total_changes(shell_db(sh)));
    }
    if (sh->flag[SHELL_STATS]) {
        db_print_stats(shell_db(sh), sh->out, false);
    }
    shell_redirect_done(sh);
    return ok;
}

bool shell_feed(Shell *sh, const char *text)
{
    if (sh->stmt.len == 0u) {
        const char *first = skip_space(text);

        if (*first == '\0') {
            return true;
        }
        if (*first == '.') {
            bool ok = dot_run(sh, first);

            if (!ok && sh->flag[SHELL_BAIL]) {
                shell_quit(sh, 1);
            }
            return ok;
        }
    }

    if (!buffer_append(&sh->stmt, text) || !buffer_append(&sh->stmt, "\n")) {
        fputs("redstone: out of memory\n", sh->err);
        shell_quit(sh, 1);
        return false;
    }
    if (!db_is_complete(sh->stmt.data)) {
        return true;
    }
    {
        bool ok = shell_exec(sh, sh->stmt.data);

        buffer_clear(&sh->stmt);
        if (!ok && sh->flag[SHELL_BAIL]) {
            shell_quit(sh, 1);
        }
        return ok;
    }
}

/* True when a statement is half-typed, so the REPL knows which prompt to
 * show and the end of input knows to complain. */
static bool pending(const Shell *sh)
{
    return sh->stmt.len > 0u && *skip_space(sh->stmt.data) != '\0';
}

bool shell_source(Shell *sh, const char *path, bool complain)
{
    FILE *file = fopen(path, "r");
    char line[4096];
    bool ok = true;

    if (file == NULL) {
        if (complain) {
            fprintf(sh->err, "redstone: cannot open %s\n", path);
        }
        return false;
    }
    while (!sh->quitting && fgets(line, (int)sizeof(line), file) != NULL) {
        size_t n = strlen(line);

        while (n > 0u && (line[n - 1u] == '\n' || line[n - 1u] == '\r')) {
            line[--n] = '\0';
        }
        if (!shell_feed(sh, line)) {
            ok = false;
            if (sh->flag[SHELL_BAIL]) {
                break;
            }
        }
    }
    (void)fclose(file);
    return ok;
}

/* ~/.sqliterc for parity, then ours, so a setting in redstonerc wins over the
 * same setting in .sqliterc rather than the other way round. */
void shell_load_init(Shell *sh, const char *explicit_path)
{
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[1024];

    if (explicit_path != NULL) {
        (void)shell_source(sh, explicit_path, true);
        return;
    }
    if (home != NULL && home[0] != '\0') {
        if (snprintf(path, sizeof(path), "%s/.sqliterc", home) < (int)sizeof(path)) {
            (void)shell_source(sh, path, false);
        }
    }
    if (xdg != NULL && xdg[0] != '\0') {
        if (snprintf(path, sizeof(path), "%s/redstone/redstonerc", xdg) < (int)sizeof(path)) {
            (void)shell_source(sh, path, false);
        }
    } else if (home != NULL && home[0] != '\0') {
        if (snprintf(path, sizeof(path), "%s/.config/redstone/redstonerc", home) <
            (int)sizeof(path)) {
            (void)shell_source(sh, path, false);
        }
    }
}

/* In vi mode the prompt says which state the editor is in, because an editor
 * with hidden modes is the thing people hate about vi bindings. */
/* cppcheck-suppress constParameterPointer ; the parameter cannot be const:
 * the accessor hands out a mutable part of the session. */
static const char *prompt_for(Shell *sh)
{
    if (!sh->flag[SHELL_INTERACTIVE]) {
        return "";
    }
    if (pending(sh)) {
        return sh->prompt_cont;
    }
    if (line_keymap(sh->ln) == EDIT_VI) {
        return line_vi_state(sh->ln) == EDIT_VI_NORMAL ? "[n] " : "[i] ";
    }
    return sh->prompt_main;
}

int shell_run(Shell *sh)
{
    bool interactive = line_interactive(sh->ln);
    char *hist_path = interactive ? line_history_path() : NULL;

    sh->flag[SHELL_INTERACTIVE] = interactive;
    if (hist_path != NULL) {
        (void)line_history_load(sh->ln, hist_path);
    }

    while (!sh->quitting) {
        LineStatus status = line_read(sh->ln, prompt_for(sh));

        if (status == LINE_EOF) {
            break;
        }
        if (status == LINE_ERROR) {
            fputs("redstone: input error\n", sh->err);
            sh->status = 1;
            break;
        }
        if (status == LINE_INTR) {
            /* Ctrl-C abandons the statement under construction, not the
             * session; that is what every other shell does. */
            buffer_clear(&sh->stmt);
            continue;
        }

        (void)line_history_add(sh->ln, line_text(sh->ln));
        /* A failed statement is reported and the session goes on, as
         * sqlite3(1) does; `.bail on` is what makes it fatal. The exit
         * status still records that something failed, so a script that
         * ignores bail does not look like it succeeded. */
        if (!shell_feed(sh, line_text(sh->ln))) {
            if (!interactive) {
                sh->status = 1;
            }
            if (sh->flag[SHELL_BAIL]) {
                break;
            }
        }
    }

    if (pending(sh)) {
        fputs("redstone: incomplete statement at end of input\n", sh->err);
        sh->status = 1;
    }
    if (hist_path != NULL) {
        (void)line_history_save(sh->ln, hist_path);
        free(hist_path);
    }
    return sh->status;
}
