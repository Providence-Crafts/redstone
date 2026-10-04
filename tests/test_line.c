/*
 * Tests for the terminal layer.
 *
 * The non-tty path is tested directly. The raw-mode path needs something that
 * answers isatty(), so a pty pair is opened with POSIX calls only —
 * posix_openpt and friends are in libc, so this costs no libutil dependency.
 */
#include "hl.h"
#include "line.h"
#include "minunit.h"
#include "plat.h"
#include "suites.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

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

/* The pty path is POSIX only: the Windows console has no pty to open. The
 * Windows editor is covered by the manual checks in PROJECT.md. */
#ifndef _WIN32

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
    pty->out = NULL;
    if (pty->in != NULL) {
        int copy = dup(slave);

        if (copy >= 0) {
            pty->out = fdopen(copy, "w");
            if (pty->out == NULL) {
                (void)close(copy);
            }
        }
    }
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
    st = line_read(pty.line, "redstone> ");
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

/* A pipe must get no escape bytes at all: a redirected redstone has to produce the
 * same stream sqlite3(1) would. */
static const char *test_plain_emits_no_escapes(void)
{
    FILE *in = text_stream("select 1;\n");
    FILE *out = tmpfile();
    Line *ln = (in != NULL && out != NULL) ? line_new(in, out) : NULL;
    long written = -1;
    bool ok;

    if (ln != NULL) {
        (void)line_read(ln, "redstone> ");
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

/* --- completion over a pty ---------------------------------------------
 *
 * The four headline scenarios, end to end: bytes in at the master, a finished
 * line out of line_read. The window is deliberately small so a single menu
 * never approaches the pty's buffer, which the slave would otherwise block
 * writing into while this thread waits for line_read to return.
 */

#define COMP_FIXTURE_DB "tests/test.db"

/* Stands in for Phase 6's dot-command table. Only what the scenarios need. */
static const char *const g_dot_names[] = {"tables", "schema", "mode", "quit"};
static const char *const g_dot_modes[] = {"box", "csv", "json"};

static bool dot_command(size_t i, const char **name, const char **help)
{
    if (i >= sizeof(g_dot_names) / sizeof(g_dot_names[0])) {
        return false;
    }
    *name = g_dot_names[i];
    *help = "";
    return true;
}

static CompKind dot_arg_kind(const char *name, size_t argno, const char *const **words,
                             size_t *nwords)
{
    if (strcmp(name, "tables") == 0 && argno == 1u) {
        return COMP_TABLE;
    }
    if (strcmp(name, "mode") == 0 && argno == 1u) {
        *words = g_dot_modes;
        *nwords = sizeof(g_dot_modes) / sizeof(g_dot_modes[0]);
    }
    return COMP_KEYWORD;
}

static const CompDotSource *comp_test_dots(void)
{
    static const CompDotSource dots = {dot_command, dot_arg_kind};

    return &dots;
}

static CompList *test_completer(void *ctx, const char *text, size_t cursor)
{
    SqlContext sctx;

    sql_context(text, cursor, &sctx);
    return comp_generate((Db *)ctx, &sctx, comp_test_dots());
}

static void pty_resize(const Pty *pty, unsigned short cols, unsigned short rows)
{
    struct winsize ws;

    ws.ws_col = cols;
    ws.ws_row = rows;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;
    (void)ioctl(pty->master, TIOCSWINSZ, &ws);
}

/* A line longer than the terminal wraps onto more than one row; editing at
 * the far end from where the cursor lands exercises the multi-row redraw
 * arithmetic that a line shorter than the terminal never touches. */
static const char *test_pty_long_line_redraw(void)
{
    Pty pty;
    bool correct;
    LineStatus st;
    char expect[42];
    int i;

    if (!pty_open(&pty)) {
        return NULL;
    }
    pty_resize(&pty, 20, 24);
    (void)pty_set_raw(&pty);
    for (i = 0; i < 40; i++) {
        pty_send(&pty, "x");
    }
    /* Ctrl-A (start of line), insert Y, Enter. */
    pty_send(&pty, "\x01Y\r");
    st = line_read(pty.line, "redstone> ");
    expect[0] = 'Y';
    memset(expect + 1, 'x', 40);
    expect[41] = '\0';
    correct = st == LINE_OK && strcmp(line_text(pty.line), expect) == 0;
    pty_close(&pty);

    mu_assert("editing a line wider than the terminal produced the wrong buffer", correct);
    return NULL;
}

/* Read whatever the shell has written, stopping once the stream goes quiet. */
static void pty_capture(const Pty *pty, char *buf, size_t cap)
{
    size_t len = 0u;
    struct pollfd pfd;

    pfd.fd = pty->master;
    pfd.events = POLLIN;
    for (;;) {
        ssize_t n;

        pfd.revents = 0;
        if (poll(&pfd, (nfds_t)1, 50) <= 0) {
            break;
        }
        n = read(pty->master, buf + len, cap - len - 1u);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
        if (len + 1u >= cap) {
            break;
        }
    }
    buf[len] = '\0';
}

/* True when S contains a CSI ending in 'm'. Cursor motion and erasing are not
 * colour, and must survive NO_COLOR. */
static bool capture_has_sgr(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] == '\x1b' && s[i + 1u] == '[') {
            size_t j = i + 2u;

            while (s[j] != '\0' && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                j++;
            }
            if (s[j] == 'm') {
                return true;
            }
        }
    }
    return false;
}

/* Open a pty wired to the fixture database, sized for a small menu. Returns
 * NULL when the platform has no ptys, which the caller treats as "skip". */
/* A highlighter, so the pty tests see the bytes the user's terminal sees. It
 * knows one keyword and nothing else: what is under test here is that line.c
 * routes the redraw through hl.c at all, not which name gets which colour --
 * that is test_hl.c's job. */
static bool pty_is_keyword(void *ctx, const char *name, size_t len)
{
    static const char word[] = "select";
    size_t i;

    (void)ctx;
    if (len != sizeof word - 1u) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        if (tolower((unsigned char)name[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

static void pty_set_highlighter(Pty *pty)
{
    HlSchema schema;

    schema.is_keyword = pty_is_keyword;
    schema.is_function = NULL;
    schema.is_table = NULL;
    schema.is_column = NULL;
    schema.ctx = NULL;
    line_set_highlighter(pty->line, &schema);
}

static Db *comp_pty_open(Pty *pty)
{
    Db *db;

    if (!pty_open(pty)) {
        return NULL;
    }
    db = db_open(COMP_FIXTURE_DB, stderr);
    if (db == NULL) {
        pty_close(pty);
        return NULL;
    }
    (void)pty_set_raw(pty);
    pty_resize(pty, 60, 8);
    {
        LineCompleter completer;

        completer.generate = test_completer;
        completer.ctx = db;
        line_set_completer(pty->line, &completer);
    }
    pty_set_highlighter(pty);
    return db;
}

/* FROM <tab> then WHERE <tab>: each menu is narrowed to one candidate by
 * typing, then accepted with Enter. */
static const char *test_pty_completion_scenarios(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    char capture[16384];
    bool line_ok;
    bool menu_drawn;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    /* Accepting "employees" (a table) inserts its own trailing space, so the
     * "WHERE" that follows needs none of its own. */
    pty_send(&pty, "SELECT * FROM \templo\rWHERE \tstat\r\r");
    line_ok = line_read(pty.line, "> ") == LINE_OK &&
              strcmp(line_text(pty.line), "SELECT * FROM employees WHERE status") == 0;
    pty_capture(&pty, capture, sizeof(capture));
    menu_drawn = strstr(capture, "\x1b[0J") != NULL;
    pty_close(&pty);
    db_close(db);

    mu_assert("the completed statement is wrong", line_ok);
    mu_assert("no menu was ever drawn", menu_drawn);
    return NULL;
}

/* Accepting a keyword, a dot command, or a table/view/alias inserts a
 * trailing space of its own, since a word almost always follows; a column, a
 * value, a function and a pragma do not, since punctuation is at least as
 * likely to follow those as another word. A dot command's own table argument
 * is the exception to the table/view/alias rule: it is usually the last thing
 * on the line. */
static const char *test_pty_completion_trailing_space(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool keyword_spaced;
    bool column_bare;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    /* "sel<TAB>" completes the one matching keyword and gets a space; typing
     * "*" straight after it would run into the word if the space were
     * missing. */
    pty_send(&pty, "sel\t*\r");
    keyword_spaced =
        line_read(pty.line, "> ") == LINE_OK && strcmp(line_text(pty.line), "select *") == 0;
    pty_close(&pty);
    db_close(db);

    db = comp_pty_open(&pty);
    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    /* "sal<TAB>" completes the one matching column with no space, so typing
     * "=" straight after it lands right against the name. */
    pty_send(&pty, "SELECT * FROM employees WHERE sal\t=1\r");
    column_bare = line_read(pty.line, "> ") == LINE_OK &&
                  strcmp(line_text(pty.line), "SELECT * FROM employees WHERE salary=1") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("a completed keyword should get a trailing space", keyword_spaced);
    mu_assert("a completed column should not get a trailing space", column_bare);
    return NULL;
}

/* The select list is the scenario that motivated the whole context analyser:
 * the table is named after the cursor, and the columns offered are still the
 * right ones. Ctrl-A puts the cursor back before the already-typed FROM. */
static const char *test_pty_select_list_scope(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool scoped;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    pty_send(&pty, " FROM employees\x01SELECT \tsal\r\r");
    scoped = line_read(pty.line, "> ") == LINE_OK &&
             strcmp(line_text(pty.line), "SELECT salary FROM employees") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("a select list did not scope to the table named after it", scoped);
    return NULL;
}

static const char *test_pty_dot_completion(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool command;
    bool argument;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    /* ".t<tab>" has one match, so it is inserted without a menu, with its own
     * trailing space -- no leading space is typed before "ord". Its table
     * argument gets no trailing space of its own, since it is normally the
     * last thing on the line. The argument then opens a menu of the two
     * tables starting "ord", which typing "_" narrows to one by typing the
     * rest of the prefix. */
    pty_send(&pty, ".t\tord\ter_\r\r");
    command = line_read(pty.line, "> ") == LINE_OK;
    argument = strcmp(line_text(pty.line), ".tables order_items") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("reading the dot line failed", command);
    mu_assert("dot command or its table argument completed wrongly", argument);
    return NULL;
}

/* A single candidate is inserted directly: drawing a one-line menu to choose
 * from one thing is noise. */
static const char *test_pty_unique_draws_no_menu(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    char capture[16384];
    bool inserted;
    bool quiet;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    /* A table name completing FROM gets its own trailing space. */
    pty_send(&pty, "SELECT * FROM order_i\t\r");
    inserted = line_read(pty.line, "> ") == LINE_OK &&
               strcmp(line_text(pty.line), "SELECT * FROM order_items ") == 0;
    pty_capture(&pty, capture, sizeof(capture));
    quiet = strstr(capture, "\x1b[0J") == NULL;
    pty_close(&pty);
    db_close(db);

    mu_assert("the unique candidate was not inserted", inserted);
    mu_assert("a menu was drawn for a single candidate", quiet);
    return NULL;
}

/* Esc must leave the buffer as it was and take the menu off the screen. */
static const char *test_pty_escape_dismisses(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    char capture[16384];
    bool restored;
    bool cleared;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    pty_send(&pty, "SELECT * FROM \t\x1b"
                   "x\r");
    restored =
        line_read(pty.line, "> ") == LINE_OK && strcmp(line_text(pty.line), "SELECT * FROM x") == 0;
    pty_capture(&pty, capture, sizeof(capture));
    /* The last thing drawn must be a bare prompt line: no rows below it, so no
     * cursor-up to get back to it. */
    cleared = strstr(capture, "\x1b[0J") != NULL && strstr(capture, "employees") != NULL;
    pty_close(&pty);
    db_close(db);

    mu_assert("Esc did not restore the buffer", restored);
    mu_assert("the menu was never drawn, so its removal proves nothing", cleared);
    return NULL;
}

/* While a menu is open, Left/Right must drive the selection (same as
 * Tab/Shift-Tab) instead of moving the cursor. Ctrl-G discards the selection
 * and the marker typed afterwards proves the cursor never left the end of
 * "FROM ". */
static const char *test_pty_arrows_select_in_menu(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool cursor_untouched;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    pty_send(&pty, "SELECT * FROM \t\x1b[C\x1b[C\x1b[D\x07X\r");
    cursor_untouched =
        line_read(pty.line, "> ") == LINE_OK && strcmp(line_text(pty.line), "SELECT * FROM X") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("Left/Right moved the cursor instead of the menu selection", cursor_untouched);
    return NULL;
}

/* Candidates sort as departments, employees, order_items, orders. Two Rights
 * from the initial selection land on order_items; Enter accepts it. This
 * proves the arrows are actually wired to menu movement, not merely inert. */
static const char *test_pty_arrows_cycle_candidates(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool picked;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    pty_send(&pty, "SELECT * FROM \t\x1b[C\x1b[C\r\r");
    picked = line_read(pty.line, "> ") == LINE_OK &&
             strcmp(line_text(pty.line), "SELECT * FROM order_items ") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("Right did not step the menu selection to order_items", picked);
    return NULL;
}

/* Typing after Tab narrows the open menu instead of dismissing it, which is
 * the behaviour that makes the menu worth opening. The buffer proves it: if
 * the menu had closed, Enter would end the line at the typed prefix, and if
 * the menu had merely stayed as it was, Enter would accept the candidate that
 * was selected before the prefix was typed. */
static const char *test_pty_typing_narrows(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    bool narrowed;

    if (db == NULL) {
        return NULL;
    }
    theme_set_colour(false);
    pty_send(&pty, "SELECT * FROM \tord\r\r");
    narrowed = line_read(pty.line, "> ") == LINE_OK &&
               strcmp(line_text(pty.line), "SELECT * FROM order_items ") == 0;
    pty_close(&pty);
    db_close(db);

    mu_assert("typing after Tab did not narrow the open menu", narrowed);
    return NULL;
}

/* The headline of Phase 7 over a real terminal: what is typed comes back
 * coloured, in the styles the theme defines, without a redraw for each byte
 * having to be parsed by the test. */
static const char *test_pty_highlight(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    char capture[16384];
    char want[128];
    bool coloured;

    if (db == NULL) {
        return NULL;
    }
    theme_reset();
    theme_set_colour(true);
    (void)snprintf(want, sizeof want, "%sSELECT%s", theme_sgr(THEME_KEYWORD),
                   theme_sgr(THEME_RESET));
    pty_send(&pty, "SELECT x\r");
    (void)line_read(pty.line, "> ");
    pty_capture(&pty, capture, sizeof(capture));
    coloured = strstr(capture, want) != NULL;
    theme_set_colour(false);
    pty_close(&pty);
    db_close(db);

    mu_assert("the keyword was not drawn in the keyword style", coloured);
    return NULL;
}

static const char *test_pty_no_color(void)
{
    Pty pty;
    Db *db = comp_pty_open(&pty);
    char capture[16384];
    bool detected_tty;
    bool plain;

    if (db == NULL) {
        return NULL;
    }
    /* A pty is a terminal, so detection must turn colour on here ... */
    (void)plat_unsetenv("NO_COLOR");
    theme_detect(pty.out);
    detected_tty = theme_colour();
    /* ... and NO_COLOR must override that. */
    (void)plat_setenv("NO_COLOR", "1");
    theme_detect(pty.out);

    /* The highlighter installed by comp_pty_open runs on this line too, so
     * NO_COLOR is checked over syntax colour as well as over the menu. */
    pty_send(&pty, "SELECT * FROM \t\r\r");
    (void)line_read(pty.line, "> ");
    pty_capture(&pty, capture, sizeof(capture));
    plain = !capture_has_sgr(capture);

    (void)plat_unsetenv("NO_COLOR");
    theme_set_colour(false);
    pty_close(&pty);
    db_close(db);

    mu_assert("colour should be detected on a terminal", detected_tty);
    mu_assert("NO_COLOR must suppress every SGR sequence", plain);
    return NULL;
}

#endif /* !_WIN32 */

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

    if (plat_setenv("XDG_STATE_HOME", "/tmp/redstone-state") != 0) {
        return NULL;
    }
    path = line_history_path();
    ok = path != NULL && strcmp(path, "/tmp/redstone-state/redstone/history") == 0;
    free(path);
    (void)plat_unsetenv("XDG_STATE_HOME");

    mu_assert("XDG_STATE_HOME not honoured", ok);
    return NULL;
}

const char *line_suite(void)
{
    mu_run_test(test_plain_reads);
    mu_run_test(test_plain_is_not_interactive);
#ifndef _WIN32
    mu_run_test(test_pty_editing);
    mu_run_test(test_pty_interrupt_and_eof);
    mu_run_test(test_pty_restores_termios);
    mu_run_test(test_plain_emits_no_escapes);
    mu_run_test(test_sigterm_restores_termios);
    mu_run_test(test_pty_long_line_redraw);
    mu_run_test(test_pty_completion_scenarios);
    mu_run_test(test_pty_completion_trailing_space);
    mu_run_test(test_pty_select_list_scope);
    mu_run_test(test_pty_dot_completion);
    mu_run_test(test_pty_unique_draws_no_menu);
    mu_run_test(test_pty_escape_dismisses);
    mu_run_test(test_pty_typing_narrows);
    mu_run_test(test_pty_arrows_select_in_menu);
    mu_run_test(test_pty_arrows_cycle_candidates);
    mu_run_test(test_pty_highlight);
    mu_run_test(test_pty_no_color);
#endif
    mu_run_test(test_history_roundtrip);
    mu_run_test(test_history_missing_file);
    mu_run_test(test_history_path);
    return NULL;
}
