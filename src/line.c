#include "line.h"

#include "menu.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define LINE_CHUNK 128u

/* How long to wait for the rest of an escape sequence before concluding the Esc
 * stood alone. Long enough for a terminal to deliver the tail of an arrow key
 * over a slow link, short enough that vi's Esc feels immediate. */
#define LINE_ESC_MS 50

struct Line {
    FILE *in;
    FILE *out;
    int infd;
    bool tty;
    bool raw;
    struct termios saved;

    Edit *edit;
    unsigned cols; /* terminal width, refreshed on SIGWINCH */
    unsigned trows;
    char *text; /* the accepted line, owned here */
    size_t tcap;

    Menu *menu;
    unsigned menu_rows; /* lines the menu occupied at the last redraw */
    LineCompleter completer;
};

/* The one live instance, so the atexit hook and the signal handlers can restore
 * the terminal. A shell edits one line at a time; a registry would be ceremony.
 *
 * Only the two fields the handler touches are kept separately, as
 * sig_atomic_t/plain scalars: a handler must not chase pointers into a struct
 * that the main flow may be reallocating. */
static Line *g_line = NULL;
static volatile sig_atomic_t g_raw_fd = -1;
static struct termios g_saved_termios;
static volatile sig_atomic_t g_winch = 0;

/* Async-signal-safe: tcsetattr and _exit are both on the POSIX list. The
 * terminal is restored first, then the default disposition re-raises the signal
 * so the process dies with the right status. */
static void restore_on_signal(int sig)
{
    if (g_raw_fd >= 0) {
        (void)tcsetattr((int)g_raw_fd, TCSADRAIN, &g_saved_termios);
        g_raw_fd = -1;
    }
    (void)signal(sig, SIG_DFL);
    (void)raise(sig);
}

static void note_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}

/* SIGINT is deliberately absent: raw mode disables ISIG, so Ctrl-C reaches the
 * editor as a byte. These are the signals that would otherwise kill the process
 * with the terminal still in raw mode. */
static void install_handlers(void)
{
    static const int fatal[] = {SIGTERM, SIGQUIT, SIGHUP, SIGSEGV, SIGABRT};
    size_t i;

    for (i = 0u; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        (void)signal(fatal[i], restore_on_signal);
    }
    (void)signal(SIGWINCH, note_winch);
}

/* ------------------------------------------------------------------------
 * Raw mode
 * ---------------------------------------------------------------------- */

static void raw_off(Line *ln)
{
    if (ln == NULL || !ln->raw) {
        return;
    }
    (void)tcsetattr(ln->infd, TCSADRAIN, &ln->saved);
    g_raw_fd = -1;
    ln->raw = false;
}

static void restore_at_exit(void)
{
    raw_off(g_line);
}

static bool raw_on(Line *ln)
{
    struct termios raw;

    if (ln->raw) {
        return true;
    }
    if (!ln->tty || tcgetattr(ln->infd, &ln->saved) != 0) {
        return false;
    }
    raw = ln->saved;
    /* No canonical input, no echo, no signal or flow-control interception: the
     * editor sees every byte, including Ctrl-C and Ctrl-S, and decides. */
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    /* TCSADRAIN, not TCSAFLUSH: the latter discards whatever the user has
     * already typed, which loses pasted input and type-ahead. */
    if (tcsetattr(ln->infd, TCSADRAIN, &raw) != 0) {
        return false;
    }
    g_saved_termios = ln->saved;
    g_raw_fd = ln->infd;
    ln->raw = true;
    return true;
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

Line *line_new(FILE *in, FILE *out)
{
    Line *ln = (Line *)calloc(1u, sizeof(*ln));

    if (ln == NULL) {
        return NULL;
    }
    ln->in = in;
    ln->out = out;
    ln->infd = fileno(in);
    ln->tty = ln->infd >= 0 && isatty(ln->infd) == 1 && isatty(fileno(out)) == 1;
    ln->edit = edit_new();
    ln->menu = menu_new();
    if (ln->edit == NULL || ln->menu == NULL) {
        edit_free(ln->edit);
        menu_free(ln->menu);
        free(ln);
        return NULL;
    }
    if (g_line == NULL) {
        g_line = ln;
        (void)atexit(restore_at_exit);
        install_handlers();
    }
    return ln;
}

void line_free(Line *ln)
{
    if (ln == NULL) {
        return;
    }
    raw_off(ln);
    if (g_line == ln) {
        g_line = NULL;
    }
    edit_free(ln->edit);
    menu_free(ln->menu);
    free(ln->text);
    free(ln);
}

bool line_interactive(const Line *ln)
{
    return ln != NULL && ln->tty;
}

void line_set_keymap(Line *ln, EditKeymap keymap)
{
    if (ln != NULL) {
        edit_set_keymap(ln->edit, keymap);
    }
}

EditKeymap line_keymap(const Line *ln)
{
    return ln != NULL ? edit_keymap(ln->edit) : EDIT_EMACS;
}

EditViState line_vi_state(const Line *ln)
{
    return ln != NULL ? edit_vi_state(ln->edit) : EDIT_VI_INSERT;
}

void line_set_completer(Line *ln, const LineCompleter *completer)
{
    static const LineCompleter none = {NULL, NULL};

    if (ln != NULL) {
        ln->completer = completer != NULL ? *completer : none;
    }
}

bool line_history_add(Line *ln, const char *text)
{
    return ln != NULL && edit_history_add(ln->edit, text);
}

const char *line_text(const Line *ln)
{
    return (ln != NULL && ln->text != NULL) ? ln->text : "";
}

/* ------------------------------------------------------------------------
 * Drawing
 *
 * Single-line redraw: return to column 0, write prompt and buffer, clear to end
 * of line, then place the cursor. Wrapped lines are Phase 5's concern, when the
 * output module learns display widths; the escape sequences used here are the
 * unambiguous CSI subset every terminal since the VT100 implements.
 * ---------------------------------------------------------------------- */

#define LINE_COLS_FALLBACK 80u
#define LINE_ROWS_FALLBACK 24u

static void update_cols(Line *ln)
{
    struct winsize ws;

    if (ioctl(ln->infd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ln->cols = ws.ws_col;
        ln->trows = ws.ws_row > 0 ? ws.ws_row : LINE_ROWS_FALLBACK;
        return;
    }
    if (ln->cols == 0u) {
        ln->cols = LINE_COLS_FALLBACK;
    }
    if (ln->trows == 0u) {
        ln->trows = LINE_ROWS_FALLBACK;
    }
}

/* Draw the menu below the prompt line and report how many rows it took. The
 * caller has already erased everything below the prompt, so a menu that has
 * shrunk or closed leaves nothing behind. */
static void draw_menu(Line *ln)
{
    unsigned rows = 0u;
    const char *text;

    ln->menu_rows = 0u;
    if (!menu_active(ln->menu)) {
        return;
    }
    menu_set_size(ln->menu, ln->cols, ln->trows > 1u ? ln->trows - 1u : 1u);
    text = menu_render(ln->menu, &rows);
    fputs(text, ln->out);
    ln->menu_rows = rows;
}

/* Keep the visible window on the part of the line the cursor is in. Scrolling
 * horizontally rather than wrapping keeps the prompt on one row, which is what
 * makes the completion menu in Phase 4 a simple region below it. */
static void refresh(Line *ln, const char *prompt)
{
    const char *buf = edit_buffer(ln->edit);
    size_t plen = strlen(prompt);
    size_t len = edit_len(ln->edit);
    size_t curs = edit_cursor(ln->edit);
    size_t avail;
    size_t start = 0u;

    if (g_winch != 0) {
        g_winch = 0;
        update_cols(ln);
    }
    if (ln->cols == 0u) {
        update_cols(ln);
    }

    /* One column is left unwritten so a full line does not wrap on its own. */
    avail = ln->cols > plen + 1u ? ln->cols - plen - 1u : 1u;
    if (curs > avail) {
        start = curs - avail;
    }
    if (len - start > avail) {
        len = start + avail;
    }

    fputc('\r', ln->out);
    fputs(prompt, ln->out);
    fwrite(buf + start, 1u, len - start, ln->out);
    if (ln->menu_rows > 0u || menu_active(ln->menu)) {
        /* Erase to the end of the display, not just the line: the menu owns
         * everything below the prompt, and 0J is what removes the rows a
         * narrowed or dismissed menu no longer needs. */
        fputs("\x1b[0J", ln->out);
        draw_menu(ln);
    } else {
        fputs("\x1b[0K", ln->out); /* erase whatever the previous line left */
    }
    fputc('\r', ln->out);
    if (ln->menu_rows > 0u) {
        fprintf(ln->out, "\x1b[%uA", ln->menu_rows);
    }
    if (plen + curs - start > 0u) {
        fprintf(ln->out, "\x1b[%luC", (unsigned long)(plen + curs - start));
    }
    (void)fflush(ln->out);
}

/* ------------------------------------------------------------------------
 * Completion
 * ---------------------------------------------------------------------- */

static CompList *candidates(Line *ln)
{
    if (ln->completer.generate == NULL) {
        return NULL;
    }
    return ln->completer.generate(ln->completer.ctx, edit_buffer(ln->edit), edit_cursor(ln->edit));
}

/* Overwrite the partial word with the chosen text. The offset comes from the
 * list rather than from a rescan, so the two can never disagree. */
static void insert_candidate(Line *ln, const CompList *list, const Comp *c)
{
    if (list == NULL || c == NULL) {
        return;
    }
    (void)edit_replace_range(ln->edit, comp_offset(list), edit_cursor(ln->edit), c->text);
}

static void close_menu(Line *ln)
{
    menu_close(ln->menu);
    edit_completing(ln->edit, false);
}

static void start_completion(Line *ln, const char *prompt)
{
    CompList *list = candidates(ln);

    if (list == NULL || comp_count(list) == 0u) {
        comp_free(list);
        fputc('\a', ln->out);
        (void)fflush(ln->out);
        return;
    }
    if (comp_count(list) == 1u) {
        /* One candidate is not a choice: insert it and never draw a menu. */
        insert_candidate(ln, list, comp_at(list, 0u));
        comp_free(list);
        refresh(ln, prompt);
        return;
    }
    menu_open(ln->menu, list);
    edit_completing(ln->edit, true);
    refresh(ln, prompt);
}

/* Called after the buffer changed while the menu was open: the list is rebuilt
 * for the new prefix, and the menu closes by itself once nothing matches. */
static void narrow(Line *ln)
{
    menu_open(ln->menu, candidates(ln));
    if (!menu_active(ln->menu)) {
        edit_completing(ln->edit, false);
    }
}

static void accept_completion(Line *ln, const char *prompt)
{
    insert_candidate(ln, menu_list(ln->menu), menu_selected(ln->menu));
    close_menu(ln);
    refresh(ln, prompt);
}

static LineStatus apply_completion(Line *ln, EditAction act, const char *prompt)
{
    static const MenuMove moves[] = {MENU_NEXT, MENU_PREV, MENU_UP, MENU_DOWN};

    switch (act) {
    case EDIT_COMPLETE:
        start_completion(ln, prompt);
        return LINE_OK;
    case EDIT_COMP_NEXT:
    case EDIT_COMP_PREV:
    case EDIT_COMP_UP:
    case EDIT_COMP_DOWN:
        menu_move(ln->menu, moves[act - EDIT_COMP_NEXT]);
        refresh(ln, prompt);
        return LINE_OK;
    case EDIT_COMP_ACCEPT:
        accept_completion(ln, prompt);
        return LINE_OK;
    case EDIT_COMP_CANCEL:
    default:
        close_menu(ln);
        refresh(ln, prompt);
        return LINE_OK;
    }
}

/* ------------------------------------------------------------------------
 * Reading
 * ---------------------------------------------------------------------- */

static bool reserve(Line *ln, size_t need)
{
    size_t cap = ln->tcap == 0u ? LINE_CHUNK : ln->tcap;
    char *grown;

    if (need <= ln->tcap) {
        return true;
    }
    while (cap < need) {
        if (cap > (size_t)-1 / 2u) {
            return false;
        }
        cap *= 2u;
    }
    grown = (char *)realloc(ln->text, cap);
    if (grown == NULL) {
        return false;
    }
    ln->text = grown;
    ln->tcap = cap;
    return true;
}

/* Copy an external buffer in. Never called with a pointer into ln->text:
 * reserve may move that allocation. */
static bool store_text(Line *ln, const char *src, size_t len)
{
    if (!reserve(ln, len + 1u)) {
        return false;
    }
    memcpy(ln->text, src, len);
    ln->text[len] = '\0';
    return true;
}

/* Non-tty input: no editing, no prompt, no history. Reads until newline or EOF
 * so scripts behave identically whether or not a terminal is attached. */
static LineStatus read_plain(Line *ln)
{
    size_t len = 0u;

    for (;;) {
        int ch = fgetc(ln->in);
        if (ch == EOF) {
            if (len == 0u) {
                return ferror(ln->in) != 0 ? LINE_ERROR : LINE_EOF;
            }
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (!reserve(ln, len + 2u)) {
            return LINE_ERROR;
        }
        ln->text[len++] = (char)ch;
    }
    if (!reserve(ln, len + 1u)) {
        return LINE_ERROR;
    }
    ln->text[len] = '\0';
    return LINE_OK;
}

/* Translate one editor action into screen output. Returns the terminating
 * status, or LINE_OK with *done false to keep reading. */
static LineStatus apply(Line *ln, EditAction act, const char *prompt, bool *done)
{
    *done = false;
    if (act >= EDIT_COMPLETE) {
        return apply_completion(ln, act, prompt);
    }
    switch (act) {
    case EDIT_REDRAW:
        if (edit_is_completing(ln->edit)) {
            narrow(ln);
        }
        refresh(ln, prompt);
        return LINE_OK;
    case EDIT_CLEAR:
        fputs("\x1b[H\x1b[2J", ln->out);
        refresh(ln, prompt);
        return LINE_OK;
    case EDIT_BELL:
        fputc('\a', ln->out);
        (void)fflush(ln->out);
        return LINE_OK;
    case EDIT_ACCEPT:
        fputs("\r\n", ln->out);
        (void)fflush(ln->out);
        *done = true;
        return LINE_OK;
    case EDIT_INTR:
        fputs("^C\r\n", ln->out);
        (void)fflush(ln->out);
        *done = true;
        return LINE_INTR;
    case EDIT_EOF:
        fputs("\r\n", ln->out);
        (void)fflush(ln->out);
        *done = true;
        return LINE_EOF;
    case EDIT_NONE:
    default:
        return LINE_OK;
    }
}

/* True when another byte is already on its way. EINTR is treated as "keep
 * waiting" so a window resize does not turn an arrow key into an Esc. */
static bool more_input(const Line *ln)
{
    struct pollfd pfd;
    int ready;

    pfd.fd = ln->infd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        ready = poll(&pfd, (nfds_t)1, LINE_ESC_MS);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

static LineStatus read_raw(Line *ln, const char *prompt)
{
    unsigned char byte;
    bool done = false;
    LineStatus st = LINE_OK;

    edit_reset(ln->edit);
    refresh(ln, prompt);

    while (!done) {
        ssize_t n = read(ln->infd, &byte, 1u);

        if (n < 0) {
            if (errno == EINTR) {
                continue; /* a resize or a caught signal is not an error */
            }
            return LINE_ERROR;
        }
        if (n == 0) {
            fputs("\r\n", ln->out);
            (void)fflush(ln->out);
            return edit_len(ln->edit) == 0u ? LINE_EOF : LINE_OK;
        }
        st = apply(ln, edit_feed(ln->edit, (int)byte), prompt, &done);
        if (st == LINE_ERROR) {
            return st;
        }
        if (!done && edit_pending_escape(ln->edit) && !more_input(ln)) {
            st = apply(ln, edit_timeout(ln->edit), prompt, &done);
            if (st == LINE_ERROR) {
                return st;
            }
        }
    }
    if (st != LINE_OK) {
        return st;
    }
    return store_text(ln, edit_buffer(ln->edit), edit_len(ln->edit)) ? LINE_OK : LINE_ERROR;
}

LineStatus line_read(Line *ln, const char *prompt)
{
    LineStatus st;

    if (ln == NULL) {
        return LINE_ERROR;
    }
    if (prompt == NULL) {
        prompt = "";
    }
    if (!ln->tty) {
        return read_plain(ln);
    }
    if (!raw_on(ln)) {
        /* A terminal that refuses raw mode still deserves a working shell. */
        fputs(prompt, ln->out);
        (void)fflush(ln->out);
        return read_plain(ln);
    }
    st = read_raw(ln, prompt);
    close_menu(ln);
    ln->menu_rows = 0u;
    raw_off(ln);
    return st;
}

/* ------------------------------------------------------------------------
 * History file
 * ---------------------------------------------------------------------- */

static char *join_path(const char *dir, const char *rest)
{
    size_t need = strlen(dir) + strlen(rest) + 2u;
    char *out = (char *)malloc(need);

    if (out == NULL) {
        return NULL;
    }
    if ((size_t)snprintf(out, need, "%s/%s", dir, rest) >= need) {
        free(out);
        return NULL;
    }
    return out;
}

char *line_history_path(void)
{
    const char *state = getenv("XDG_STATE_HOME");
    const char *home;
    char *base;
    char *path;

    if (state != NULL && state[0] != '\0') {
        return join_path(state, "sqlsh/history");
    }
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return NULL;
    }
    base = join_path(home, ".local/state");
    if (base == NULL) {
        return NULL;
    }
    path = join_path(base, "sqlsh/history");
    free(base);
    return path;
}

/* Create every missing component of the path's directory. Failures are
 * swallowed: the caller treats an unwritable history directory as "no
 * history", never as a fatal error. */
static void make_parents(const char *path)
{
    char *copy = (char *)malloc(strlen(path) + 1u);
    char *slash;

    if (copy == NULL) {
        return;
    }
    memcpy(copy, path, strlen(path) + 1u);
    for (slash = copy + 1; *slash != '\0'; slash++) {
        if (*slash != '/') {
            continue;
        }
        *slash = '\0';
        (void)mkdir(copy, 0700);
        *slash = '/';
    }
    free(copy);
}

bool line_history_load(Line *ln, const char *path)
{
    FILE *fp;
    char *buf = NULL;
    size_t cap = 0u;
    size_t len = 0u;

    if (ln == NULL || path == NULL) {
        return false;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        return false;
    }
    for (;;) {
        int ch = fgetc(fp);
        if (ch == '\n' || ch == EOF) {
            if (len > 0u) {
                buf[len] = '\0';
                (void)edit_history_add(ln->edit, buf);
                len = 0u;
            }
            if (ch == EOF) {
                break;
            }
            continue;
        }
        if (len + 2u > cap) {
            size_t want = cap == 0u ? LINE_CHUNK : cap * 2u;
            char *grown = (char *)realloc(buf, want);

            if (grown == NULL) {
                break;
            }
            buf = grown;
            cap = want;
        }
        buf[len++] = (char)ch;
    }
    free(buf);
    (void)fclose(fp);
    return true;
}

bool line_history_save(const Line *ln, const char *path)
{
    FILE *fp;
    size_t i;
    size_t count;
    bool ok;

    if (ln == NULL || path == NULL) {
        return false;
    }
    make_parents(path);
    fp = fopen(path, "w");
    if (fp == NULL) {
        return false;
    }
    count = edit_history_count(ln->edit);
    for (i = 0u; i < count; i++) {
        fputs(edit_history_at(ln->edit, i), fp);
        fputc('\n', fp);
    }
    /* One check for the whole file: stream errors are sticky, and the flush is
     * what forces the write that might fail. */
    ok = fflush(fp) == 0 && ferror(fp) == 0;
    return fclose(fp) == 0 && ok;
}
