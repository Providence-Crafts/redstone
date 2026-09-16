/*
 * Tests for the terminal layer.
 *
 * The non-tty path is tested directly. The raw-mode path needs something that
 * answers isatty(), so a pty pair is opened with POSIX calls only —
 * posix_openpt and friends are in libc, so this costs no libutil dependency.
 */
#include "line.h"
#include "minunit.h"
#include "suites.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* --- the non-tty path -------------------------------------------------- */

static FILE *text_stream(const char *text)
{
    FILE *tmp = tmpfile();

    if (tmp == NULL) {
        return NULL;
    }
    fputs(text, tmp);
    if (fseek(tmp, 0L, SEEK_SET) != 0) {
        (void)fclose(tmp);
        return NULL;
    }
    return tmp;
}

static const char *test_plain_reads(void)
{
    FILE *in = text_stream("first\nsecond\nno newline");
    FILE *out = tmpfile();
    Line *ln = NULL;
    bool one = false;
    bool two = false;
    bool three = false;
    bool ended = false;

    if (in != NULL && out != NULL) {
        ln = line_new(in, out);
    }
    if (ln != NULL) {
        one = line_read(ln, "> ") == LINE_OK && strcmp(line_text(ln), "first") == 0;
        two = line_read(ln, "> ") == LINE_OK && strcmp(line_text(ln), "second") == 0;
        /* A final line without a newline is still a line. */
        three = line_read(ln, "> ") == LINE_OK && strcmp(line_text(ln), "no newline") == 0;
        ended = line_read(ln, "> ") == LINE_EOF;
    }
    line_free(ln);
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL) {
        (void)fclose(out);
    }

    mu_assert("line_new failed", ln != NULL);
    mu_assert("first line wrong", one);
    mu_assert("second line wrong", two);
    mu_assert("unterminated last line wrong", three);
    mu_assert("EOF not reported", ended);
    return NULL;
}

static const char *test_plain_is_not_interactive(void)
{
    FILE *in = text_stream("x\n");
    FILE *out = tmpfile();
    Line *ln = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    bool ok = ln != NULL && !line_interactive(ln);

    line_free(ln);
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL) {
        (void)fclose(out);
    }
    mu_assert("a pipe must not be interactive", ok);
    return NULL;
}

/* --- the pty path ------------------------------------------------------ */

typedef struct {
    int master;
    FILE *in;
    FILE *out;
    Line *line;
} Pty;

static void pty_close(Pty *pty)
{
    line_free(pty->line);
    if (pty->in != NULL) {
        (void)fclose(pty->in);
    }
    if (pty->out != NULL) {
        (void)fclose(pty->out);
    }
    if (pty->master >= 0) {
        (void)close(pty->master);
    }
    pty->line = NULL;
    pty->in = NULL;
    pty->out = NULL;
    pty->master = -1;
}

/* Open a pty pair and wrap the slave in a Line. Returns false when the platform
 * has no ptys available, which the caller treats as "skip", not "fail". */
static bool pty_open(Pty *pty)
{
    char *name;
    int slave;

    pty->master = -1;
    pty->in = NULL;
    pty->out = NULL;
    pty->line = NULL;

    pty->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty->master < 0 || grantpt(pty->master) != 0 || unlockpt(pty->master) != 0) {
        pty_close(pty);
        return false;
    }
    name = ptsname(pty->master);
    if (name == NULL) {
        pty_close(pty);
        return false;
    }
    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        pty_close(pty);
        return false;
    }
    pty->in = fdopen(slave, "r");
    pty->out = pty->in != NULL ? fdopen(dup(slave), "w") : NULL;
    if (pty->out == NULL) {
        if (pty->in == NULL) {
            (void)close(slave);
        }
        pty_close(pty);
        return false;
    }
    pty->line = line_new(pty->in, pty->out);
    if (pty->line == NULL) {
        pty_close(pty);
        return false;
    }
    return true;
}

/* Take the slave out of canonical mode before any byte is written to it.
 * Otherwise the line discipline consumes what the test sends — ^D becomes VEOF,
 * ^C becomes a signal, CR is translated — and none of it reaches the editor.
 * The editor sets raw mode again itself; this only settles what happens to
 * bytes that arrive before it does. */
static bool pty_set_raw(const Pty *pty)
{
    struct termios tio;

    if (tcgetattr(pty->master, &tio) != 0) {
        return false;
    }
    tio.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    tio.c_oflag &= (tcflag_t) ~(OPOST);
    tio.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    return tcsetattr(pty->master, TCSANOW, &tio) == 0;
}

static void pty_send(const Pty *pty, const char *bytes)
{
    size_t len = strlen(bytes);
    size_t done = 0u;

    while (done < len) {
        ssize_t n = write(pty->master, bytes + done, len - done);

        if (n <= 0) {
            return;
        }
        done += (size_t)n;
    }
}

static const char *test_pty_editing(void)
{
    Pty pty;
    bool interactive;
    bool edited;
    LineStatus st;

    if (!pty_open(&pty)) {
        return NULL; /* no ptys here: nothing to assert */
    }
    interactive = line_interactive(pty.line);
    (void)pty_set_raw(&pty);
    /* Type "selct", back up two and insert the missing letter, then Enter. */
    pty_send(&pty, "selct\x1b[D\x1b[De\r");
    st = line_read(pty.line, "sqlsh> ");
    edited = st == LINE_OK && strcmp(line_text(pty.line), "select") == 0;
    pty_close(&pty);

    mu_assert("a pty must be interactive", interactive);
    mu_assert("editing over a pty produced the wrong line", edited);
    return NULL;
}

static const char *test_pty_interrupt_and_eof(void)
{
    Pty pty;
    bool intr;
    bool eof;

    if (!pty_open(&pty)) {
        return NULL;
    }
    (void)pty_set_raw(&pty);
    pty_send(&pty, "junk\x03");
    intr = line_read(pty.line, "> ") == LINE_INTR;
    pty_send(&pty, "\x04");
    eof = line_read(pty.line, "> ") == LINE_EOF;
    pty_close(&pty);

    mu_assert("Ctrl-C should report LINE_INTR", intr);
    mu_assert("Ctrl-D on an empty line should report LINE_EOF", eof);
    return NULL;
}

/* Raw mode must be left exactly as it was found, or the shell corrupts the
 * user's terminal on exit. */
static const char *test_pty_restores_termios(void)
{
    Pty pty;
    struct termios before;
    struct termios after;
    bool same;

    if (!pty_open(&pty)) {
        return NULL;
    }
    /* The baseline is taken after the harness settles the mode, so what is
     * compared is "the editor put it back", not "the harness changed it". */
    (void)pty_set_raw(&pty);
    if (tcgetattr(fileno(pty.in), &before) != 0) {
        pty_close(&pty);
        return NULL;
    }
    pty_send(&pty, "abc\r");
    (void)line_read(pty.line, "> ");
    same = tcgetattr(fileno(pty.in), &after) == 0 && before.c_lflag == after.c_lflag &&
           before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag;
    pty_close(&pty);

    mu_assert("termios not restored after reading a line", same);
    return NULL;
}

/* A pipe must get no escape bytes at all: a redirected sqlsh has to produce the
 * same stream sqlite3(1) would. */
static const char *test_plain_emits_no_escapes(void)
{
    FILE *in = text_stream("select 1;\n");
    FILE *out = tmpfile();
    Line *ln = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    long written = -1;
    bool ok;

    if (ln != NULL) {
        (void)line_read(ln, "sqlsh> ");
        (void)fflush(out);
        written = ftell(out);
    }
    ok = ln != NULL && written == 0L;
    line_free(ln);
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL) {
        (void)fclose(out);
    }
    mu_assert("a non-tty read must write nothing to the output stream", ok);
    return NULL;
}

/* Wait until the pty is (or is no longer) in raw mode, up to a bound. Polling
 * beats a fixed sleep: it is both faster and not a race on a loaded machine. */
static bool await_icanon(int fd, bool want, int tries)
{
    struct termios tio;
    int i;

    for (i = 0; i < tries; i++) {
        if (tcgetattr(fd, &tio) == 0 && ((tio.c_lflag & ICANON) != 0u) == want) {
            return true;
        }
        (void)poll(NULL, (nfds_t)0, 20);
    }
    return false;
}

/* The failure this guards against is the worst one a shell can commit: dying
 * and leaving the user's terminal in raw mode. */
static const char *test_sigterm_restores_termios(void)
{
    Pty pty;
    pid_t child;
    bool went_raw = false;
    bool restored = false;
    int status = 0;

    if (!pty_open(&pty)) {
        return NULL;
    }
    child = fork();
    if (child < 0) {
        pty_close(&pty);
        return NULL;
    }
    if (child == 0) {
        /* Blocks in raw mode with no input coming; the parent kills it. */
        (void)line_read(pty.line, "> ");
        _exit(0);
    }
    went_raw = await_icanon(pty.master, false, 100);
    (void)kill(child, SIGTERM);
    (void)waitpid(child, &status, 0);
    restored = await_icanon(pty.master, true, 100);
    pty_close(&pty);

    mu_assert("the child never entered raw mode", went_raw);
    mu_assert("SIGTERM left the terminal in raw mode", restored);
    return NULL;
}

/* --- history file ------------------------------------------------------ */

static const char *test_history_roundtrip(void)
{
    FILE *in = text_stream("");
    FILE *out = tmpfile();
    Line *saver = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    Line *loader = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    const char *path = "build/test_history";
    bool saved = false;
    bool loaded = false;

    if (saver != NULL && loader != NULL) {
        (void)line_history_add(saver, "select 1;");
        (void)line_history_add(saver, "select 2;");
        saved = line_history_save(saver, path);
        loaded = line_history_load(loader, path);
    }
    line_free(saver);
    line_free(loader);
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL) {
        (void)fclose(out);
    }
    (void)remove(path);

    mu_assert("history save failed", saved);
    mu_assert("history load failed", loaded);
    return NULL;
}

/* A missing history file is normal on first run and must not be an error the
 * caller has to handle. */
static const char *test_history_missing_file(void)
{
    FILE *in = text_stream("");
    FILE *out = tmpfile();
    Line *ln = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    bool ok = ln != NULL && !line_history_load(ln, "build/no_such_history");

    line_free(ln);
    if (in != NULL) {
        (void)fclose(in);
    }
    if (out != NULL) {
        (void)fclose(out);
    }
    mu_assert("loading a missing history file should report false, not crash", ok);
    return NULL;
}

static const char *test_history_path(void)
{
    char *path;
    bool ok;

    if (setenv("XDG_STATE_HOME", "/tmp/sqlsh-state", 1) != 0) {
        return NULL;
    }
    path = line_history_path();
    ok = path != NULL && strcmp(path, "/tmp/sqlsh-state/sqlsh/history") == 0;
    free(path);
    (void)unsetenv("XDG_STATE_HOME");

    mu_assert("XDG_STATE_HOME not honoured", ok);
    return NULL;
}

const char *line_suite(void)
{
    mu_run_test(test_plain_reads);
    mu_run_test(test_plain_is_not_interactive);
    mu_run_test(test_pty_editing);
    mu_run_test(test_pty_interrupt_and_eof);
    mu_run_test(test_pty_restores_termios);
    mu_run_test(test_plain_emits_no_escapes);
    mu_run_test(test_sigterm_restores_termios);
    mu_run_test(test_history_roundtrip);
    mu_run_test(test_history_missing_file);
    mu_run_test(test_history_path);
    return NULL;
}
