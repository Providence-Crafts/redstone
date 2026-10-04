#include "line.h"

#include "hl.h"
#include "menu.h"
#include "plat.h"
#include "width.h"

#include <stdlib.h>
#include <string.h>

#define LINE_CHUNK 128u

/* strdup is not C99, and the project builds with -std=c99 strictly. */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *copy = (char *)malloc(n);

    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* How long to wait for the rest of an escape sequence before concluding the Esc
 * stood alone. Long enough for a terminal to deliver the tail of an arrow key
 * over a slow link, short enough that vi's Esc feels immediate. */
#define LINE_ESC_MS 50

struct Line {
    FILE *in;
    FILE *out;
    bool tty;
    bool raw;

    Edit *edit;
    unsigned cols; /* terminal width, refreshed on SIGWINCH */
    unsigned trows;
    char *text; /* the accepted line, owned here */
    size_t tcap;
    char *seed; /* preloaded into the buffer on the next read, then consumed */

    Menu *menu;
    unsigned menu_rows; /* lines the menu occupied at the last redraw */
    LineCompleter completer;
    HlSchema schema;
    bool highlight;
};

/* The one live instance, so the atexit hook can restore the terminal. A shell
 * edits one line at a time; a registry would be ceremony. */
static Line *g_line = NULL;

/* ------------------------------------------------------------------------
 * Raw mode
 * ---------------------------------------------------------------------- */

static void raw_off(Line *ln)
{
    if (ln == NULL || !ln->raw) {
        return;
    }
    plat_raw_leave();
    ln->raw = false;
}

static bool raw_on(Line *ln)
{
    if (ln->raw) {
        return true;
    }
    if (!ln->tty || !plat_raw_enter(ln->in)) {
        return false;
    }
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
    ln->tty = plat_isatty(in) && plat_isatty(out);
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
    free(ln->seed);
    free(ln);
}

void line_seed(Line *ln, const char *text)
{
    if (ln == NULL) {
        return;
    }
    free(ln->seed);
    ln->seed = text != NULL ? dup_str(text) : NULL;
}

bool line_interactive(const Line *ln)
{
    return ln != NULL && ln->tty;
}

void line_set_interactive(Line *ln, bool on)
{
    /* -batch and -interactive override what isatty(3) found: a script may
     * want the prompts suppressed on a terminal, or kept off a pipe. */
    ln->tty = on;
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

void line_set_highlighter(Line *ln, const HlSchema *schema)
{
    static const HlSchema none = {NULL, NULL, NULL, NULL, NULL};

    if (ln == NULL) {
        return;
    }
    ln->schema = schema != NULL ? *schema : none;
    ln->highlight = schema != NULL;
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

size_t line_history_count(const Line *ln)
{
    return ln != NULL ? edit_history_count(ln->edit) : 0u;
}

const char *line_history_at(const Line *ln, size_t i)
{
    return ln != NULL ? edit_history_at(ln->edit, i) : NULL;
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
    unsigned cols;
    unsigned rows = 0u;

    if (plat_term_size(ln->in, &cols, &rows)) {
        ln->cols = cols;
        ln->trows = rows > 0u ? rows : LINE_ROWS_FALLBACK;
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

size_t line_prompt_width(const char *prompt)
{
    size_t cells = 0u;

    while (*prompt != '\0') {
        size_t used = width_vt100(prompt);

        if (*prompt == '\x1b' && used > 0u) {
            prompt += used;
        } else {
            uint32_t c = width_decode(prompt, &used);

            cells += width_char(c);
            prompt += used;
        }
    }
    return cells;
}

unsigned line_columns(Line *ln)
{
    update_cols(ln);
    return ln->cols;
}

/* Keep the visible window on the part of the line the cursor is in. Scrolling
 * horizontally rather than wrapping keeps the prompt on one row, which is what
 * makes the completion menu in Phase 4 a simple region below it. */
static void refresh(Line *ln, const char *prompt)
{
    const char *buf = edit_buffer(ln->edit);
    size_t plen = line_prompt_width(prompt);
    size_t len = edit_len(ln->edit);
    size_t curs = edit_cursor(ln->edit);
    size_t avail;
    size_t start = 0u;

    if (plat_resized()) {
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
    hl_write(ln->out, buf, start, len, ln->highlight ? &ln->schema : NULL);
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

/* Whether accepting a candidate of this kind should be followed by a space.
 * True for kinds that name a complete clause element and are almost always
 * followed by another word (a keyword, a dot command's name, a table/view/
 * alias naming what FROM or JOIN operates on). False for kinds that are
 * usually followed by punctuation rather than a word -- a function needs its
 * "(", a column is as likely to be followed by "," or "." as by a keyword,
 * a value by an operator or ")", and a pragma by "=" or "(".
 *
 * A dot command's own argument is the exception within the exception: a table
 * name completing ".tables" or ".schema" is normally the last thing on the
 * line, unlike a table name completing SQL's FROM/JOIN, so it gets no space
 * of its own there. */
static bool comp_wants_space(const char *buf, CompKind kind)
{
    switch (kind) {
    case COMP_KEYWORD:
    case COMP_DOT_COMMAND:
        return true;
    case COMP_TABLE:
    case COMP_VIEW:
    case COMP_ALIAS:
        return buf[0] != '.';
    case COMP_COLUMN:
    case COMP_VALUE:
    case COMP_FUNCTION:
    case COMP_PRAGMA:
    default:
        return false;
    }
}

/* Overwrite the partial word with the chosen text. The offset comes from the
 * list rather than from a rescan, so the two can never disagree. */
static void insert_candidate(Line *ln, const CompList *list, const Comp *c)
{
    size_t end;

    if (list == NULL || c == NULL) {
        return;
    }
    (void)edit_replace_range(ln->edit, comp_offset(list), edit_cursor(ln->edit), c->text);
    /* Only at the true end of the line: anything already typed past the
     * cursor is content the user placed there on purpose, and is not ours to
     * push further away. */
    end = edit_cursor(ln->edit);
    if (comp_wants_space(edit_buffer(ln->edit), c->kind) && end == edit_len(ln->edit)) {
        (void)edit_replace_range(ln->edit, end, end, " ");
    }
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

/* ------------------------------------------------------------------------
 * External editor
 *
 * Ctrl-X Ctrl-E or `v` (vi normal mode) hands the buffer to $VISUAL/$EDITOR
 * and loads back whatever comes out, the same convention bash and upstream
 * sqlite3(1)'s edit() SQL function use. edit.c only recognizes the chord; the
 * spawn happens here because edit.c performs no I/O.
 * ---------------------------------------------------------------------- */

static bool write_file(const char *path, const char *buf, size_t len)
{
    FILE *fp = fopen(path, "w");
    bool ok;

    if (fp == NULL) {
        return false;
    }
    ok = fwrite(buf, 1u, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

/* Caller frees. NULL on any failure, including an empty result -- an editor
 * that produced nothing is treated the same as one that failed to run. */
static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        (void)fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1u);
    if (buf == NULL || fread(buf, 1u, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        (void)fclose(fp);
        return NULL;
    }
    buf[sz] = '\0';
    (void)fclose(fp);
    return buf;
}

/* Write TEXT to a temp file, run $VISUAL/$EDITOR/vi on it, and return what
 * came back, or NULL on any failure (a missing temp directory, an editor
 * that exits non-zero is not checked here since upstream's edit() does not
 * either -- what matters is whether a file came back readable). Caller
 * frees. Shared by the Ctrl-X Ctrl-E chord below and `.edit`, which runs
 * with the terminal already out of raw mode and so needs none of that
 * suspend/restore dance. */
char *line_external_edit(const char *text, size_t len)
{
    char path[1024];
    const char *editor = plat_default_editor();
    char *cmd;
    char *result;
    size_t need;

    if (!plat_temp_file(path, sizeof(path), "redstone-edit")) {
        return NULL;
    }
    if (!write_file(path, text, len)) {
        (void)remove(path);
        return NULL;
    }

    need = strlen(editor) + strlen(path) + 4u;
    cmd = (char *)malloc(need);
    if (cmd != NULL && (size_t)snprintf(cmd, need, "%s \"%s\"", editor, path) < need) {
        (void)system(cmd);
    }
    free(cmd);

    result = read_file(path);
    (void)remove(path);
    return result;
}

/* Suspends raw mode around the child so the editor gets a normal terminal,
 * then restores it and loads whatever the editor left behind back into the
 * buffer. Any failure along the way leaves the buffer untouched. */
static void run_external_edit(Line *ln)
{
    char *text;

    raw_off(ln);
    fputs("\r\n", ln->out);
    (void)fflush(ln->out);

    text = line_external_edit(edit_buffer(ln->edit), edit_len(ln->edit));

    (void)raw_on(ln);

    if (text != NULL) {
        (void)edit_set_buffer(ln->edit, text);
        free(text);
    }
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
    case EDIT_EXTERNAL_EDIT:
        run_external_edit(ln);
        refresh(ln, prompt);
        return LINE_OK;
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

/* True when another byte is already on its way. */
static bool more_input(const Line *ln)
{
    return plat_input_ready(ln->in, LINE_ESC_MS);
}

static LineStatus read_raw(Line *ln, const char *prompt)
{
    unsigned char byte;
    bool done = false;
    LineStatus st = LINE_OK;

    edit_reset(ln->edit);
    if (ln->seed != NULL) {
        (void)edit_set_buffer(ln->edit, ln->seed);
        free(ln->seed);
        ln->seed = NULL;
    }
    refresh(ln, prompt);

    while (!done) {
        int n = plat_read_byte(ln->in, &byte);

        if (n < 0) {
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
    char *dir = plat_state_dir();
    char *path;

    if (dir == NULL) {
        return NULL;
    }
    path = join_path(dir, "redstone/history");
    free(dir);
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
        (void)plat_mkdir(copy);
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
