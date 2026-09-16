#include "edit.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define EDIT_HISTORY_DEFAULT 1000u
#define EDIT_SEQ_MAX 8u

/* Keys, as seen by the keymaps. Byte values pass through unchanged, so the
 * synthetic keys start above any possible byte. */
enum { K_UNKNOWN = 256, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_DEL, K_SHIFT_TAB };

#define CTRL(c) ((c) & 0x1f)
#define K_ESC 0x1b
#define K_BACKSPACE 0x7f

struct Edit {
    char *buf;
    size_t len;
    size_t cap;
    size_t pos;

    EditKeymap keymap;
    EditViState vi;
    int vi_op;         /* pending operator: 0, 'd' or 'c' */
    unsigned vi_count; /* pending count; 0 means "unspecified", i.e. 1 */
    int vi_pending;    /* awaiting an argument byte: 0 or 'r' */
    char *undo;        /* single-level undo snapshot, or NULL */
    size_t undo_pos;

    bool completing;

    bool in_esc;
    char seq[EDIT_SEQ_MAX];
    size_t seqlen;

    char **hist;
    size_t hcount;
    size_t hcap;
    size_t hmax;
    size_t hpos; /* hcount when not browsing, else the entry being shown */
    char *stash; /* the in-progress line, saved while browsing history */
};

/* --------------------------------------------------------------------------
 * Buffer primitives
 * ------------------------------------------------------------------------ */

static bool ensure_cap(Edit *e, size_t need)
{
    size_t cap = e->cap;
    char *buf;

    if (need + 1u <= cap) {
        return true;
    }
    cap = cap == 0u ? 64u : cap;
    while (cap < need + 1u) {
        if (cap > (size_t)-1 / 2u) {
            return false;
        }
        cap *= 2u;
    }
    buf = (char *)realloc(e->buf, cap);
    if (buf == NULL) {
        return false;
    }
    e->buf = buf;
    e->cap = cap;
    return true;
}

static bool buf_insert(Edit *e, size_t at, const char *text, size_t n)
{
    if (n == 0u) {
        return true;
    }
    if (!ensure_cap(e, e->len + n)) {
        return false;
    }
    memmove(e->buf + at + n, e->buf + at, e->len - at);
    memcpy(e->buf + at, text, n);
    e->len += n;
    e->buf[e->len] = '\0';
    return true;
}

static void buf_delete(Edit *e, size_t from, size_t to)
{
    if (from >= to || to > e->len) {
        return;
    }
    memmove(e->buf + from, e->buf + to, e->len - to);
    e->len -= to - from;
    e->buf[e->len] = '\0';
    if (e->pos > e->len) {
        e->pos = e->len;
    }
}

/* Snapshot for `u`. One level is deliberate: more would need a real undo log,
 * and vi's single-step undo covers the mistakes made while typing one line. */
static void undo_save(Edit *e)
{
    char *copy = (char *)malloc(e->len + 1u);

    if (copy == NULL) {
        return; /* undo is a convenience; losing it must not fail the edit */
    }
    memcpy(copy, e->buf != NULL ? e->buf : "", e->len);
    copy[e->len] = '\0';
    free(e->undo);
    e->undo = copy;
    e->undo_pos = e->pos;
}

static EditAction undo_restore(Edit *e)
{
    size_t n;

    if (e->undo == NULL) {
        return EDIT_BELL;
    }
    n = strlen(e->undo);
    if (!ensure_cap(e, n)) {
        return EDIT_BELL;
    }
    memcpy(e->buf, e->undo, n + 1u);
    e->len = n;
    e->pos = e->undo_pos <= n ? e->undo_pos : n;
    free(e->undo);
    e->undo = NULL;
    return EDIT_REDRAW;
}

/* --------------------------------------------------------------------------
 * Word motion. A word is alphanumerics and underscore, matching what the SQL
 * tokenizer treats as an identifier character.
 * ------------------------------------------------------------------------ */

static bool is_word(char c)
{
    return isalnum((unsigned char)c) != 0 || c == '_';
}

static size_t word_left(const Edit *e, size_t pos)
{
    while (pos > 0u && !is_word(e->buf[pos - 1u])) {
        pos--;
    }
    while (pos > 0u && is_word(e->buf[pos - 1u])) {
        pos--;
    }
    return pos;
}

static size_t word_right(const Edit *e, size_t pos)
{
    while (pos < e->len && is_word(e->buf[pos])) {
        pos++;
    }
    while (pos < e->len && !is_word(e->buf[pos])) {
        pos++;
    }
    return pos;
}

static size_t word_end(const Edit *e, size_t pos)
{
    if (pos < e->len) {
        pos++;
    }
    while (pos < e->len && !is_word(e->buf[pos])) {
        pos++;
    }
    while (pos + 1u < e->len && is_word(e->buf[pos + 1u])) {
        pos++;
    }
    return pos;
}

static size_t first_non_blank(const Edit *e)
{
    size_t i = 0u;

    while (i < e->len && isspace((unsigned char)e->buf[i]) != 0) {
        i++;
    }
    return i;
}

/* --------------------------------------------------------------------------
 * History
 * ------------------------------------------------------------------------ */

static bool all_space(const char *s)
{
    while (*s != '\0') {
        if (isspace((unsigned char)*s) == 0) {
            return false;
        }
        s++;
    }
    return true;
}

static void history_drop_oldest(Edit *e, size_t n)
{
    size_t i;

    if (n > e->hcount) {
        n = e->hcount;
    }
    if (n == 0u) {
        return; /* memmove(NULL, NULL, 0) is undefined, and hist may be NULL */
    }
    for (i = 0u; i < n; i++) {
        free(e->hist[i]);
    }
    memmove((void *)e->hist, (const void *)(e->hist + n), (e->hcount - n) * sizeof(*e->hist));
    e->hcount -= n;
}

bool edit_history_add(Edit *e, const char *text)
{
    char *copy;

    if (e == NULL || text == NULL || all_space(text)) {
        return false;
    }
    if (e->hcount > 0u && strcmp(e->hist[e->hcount - 1u], text) == 0) {
        return false; /* consecutive duplicate */
    }
    if (e->hmax == 0u) {
        return false;
    }

    if (e->hcount == e->hcap) {
        size_t cap = e->hcap == 0u ? 64u : e->hcap * 2u;
        char **grown = (char **)realloc((void *)e->hist, cap * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        e->hist = grown;
        e->hcap = cap;
    }
    copy = (char *)malloc(strlen(text) + 1u);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text, strlen(text) + 1u);
    e->hist[e->hcount++] = copy;

    if (e->hcount > e->hmax) {
        history_drop_oldest(e, e->hcount - e->hmax);
    }
    e->hpos = e->hcount;
    return true;
}

size_t edit_history_count(const Edit *e)
{
    return e != NULL ? e->hcount : 0u;
}

const char *edit_history_at(const Edit *e, size_t i)
{
    if (e == NULL || i >= e->hcount) {
        return NULL;
    }
    return e->hist[i];
}

void edit_history_clear(Edit *e)
{
    if (e == NULL) {
        return;
    }
    history_drop_oldest(e, e->hcount);
    e->hpos = 0u;
}

void edit_history_set_max(Edit *e, size_t max)
{
    if (e == NULL) {
        return;
    }
    e->hmax = max;
    if (e->hcount > max) {
        history_drop_oldest(e, e->hcount - max);
    }
    e->hpos = e->hcount;
}

size_t edit_history_max(const Edit *e)
{
    return e != NULL ? e->hmax : 0u;
}

static bool replace_all(Edit *e, const char *text)
{
    size_t n = strlen(text);

    if (!ensure_cap(e, n)) {
        return false;
    }
    memcpy(e->buf, text, n + 1u);
    e->len = n;
    e->pos = n;
    return true;
}

/* Browsing saves the half-typed line on the way out and restores it on the way
 * back, so moving through history is never destructive. */
static EditAction history_prev(Edit *e)
{
    if (e->hcount == 0u || e->hpos == 0u) {
        return EDIT_BELL;
    }
    if (e->hpos == e->hcount) {
        free(e->stash);
        e->stash = (char *)malloc(e->len + 1u);
        if (e->stash != NULL) {
            memcpy(e->stash, e->buf != NULL ? e->buf : "", e->len);
            e->stash[e->len] = '\0';
        }
    }
    e->hpos--;
    return replace_all(e, e->hist[e->hpos]) ? EDIT_REDRAW : EDIT_BELL;
}

static EditAction history_next(Edit *e)
{
    if (e->hpos >= e->hcount) {
        return EDIT_BELL;
    }
    e->hpos++;
    if (e->hpos == e->hcount) {
        return replace_all(e, e->stash != NULL ? e->stash : "") ? EDIT_REDRAW : EDIT_BELL;
    }
    return replace_all(e, e->hist[e->hpos]) ? EDIT_REDRAW : EDIT_BELL;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

Edit *edit_new(void)
{
    Edit *e = (Edit *)calloc(1u, sizeof(*e));

    if (e == NULL) {
        return NULL;
    }
    if (!ensure_cap(e, 0u)) {
        free(e);
        return NULL;
    }
    e->buf[0] = '\0';
    e->hmax = EDIT_HISTORY_DEFAULT;
    return e;
}

void edit_free(Edit *e)
{
    if (e == NULL) {
        return;
    }
    history_drop_oldest(e, e->hcount);
    free((void *)e->hist);
    free(e->buf);
    free(e->undo);
    free(e->stash);
    free(e);
}

void edit_reset(Edit *e)
{
    if (e == NULL) {
        return;
    }
    e->len = 0u;
    e->pos = 0u;
    if (e->buf != NULL) {
        e->buf[0] = '\0';
    }
    e->hpos = e->hcount;
    e->vi_op = 0;
    e->vi_count = 0u;
    e->vi_pending = 0;
    e->vi = EDIT_VI_INSERT;
    e->completing = false;
    e->in_esc = false;
    e->seqlen = 0u;
    free(e->undo);
    e->undo = NULL;
    free(e->stash);
    e->stash = NULL;
}

const char *edit_buffer(const Edit *e)
{
    return (e != NULL && e->buf != NULL) ? e->buf : "";
}

size_t edit_len(const Edit *e)
{
    return e != NULL ? e->len : 0u;
}

size_t edit_cursor(const Edit *e)
{
    return e != NULL ? e->pos : 0u;
}

bool edit_set_buffer(Edit *e, const char *text)
{
    if (e == NULL || text == NULL) {
        return false;
    }
    return replace_all(e, text);
}

bool edit_replace_range(Edit *e, size_t from, size_t to, const char *text)
{
    if (e == NULL || text == NULL || from > to || to > e->len) {
        return false;
    }
    undo_save(e);
    buf_delete(e, from, to);
    if (!buf_insert(e, from, text, strlen(text))) {
        return false;
    }
    e->pos = from + strlen(text);
    return true;
}

void edit_completing(Edit *e, bool on)
{
    if (e != NULL) {
        e->completing = on;
    }
}

bool edit_is_completing(const Edit *e)
{
    return e != NULL && e->completing;
}

void edit_set_keymap(Edit *e, EditKeymap keymap)
{
    if (e == NULL) {
        return;
    }
    e->keymap = keymap;
    e->vi = EDIT_VI_INSERT;
}

EditKeymap edit_keymap(const Edit *e)
{
    return e != NULL ? e->keymap : EDIT_EMACS;
}

EditViState edit_vi_state(const Edit *e)
{
    return e != NULL ? e->vi : EDIT_VI_INSERT;
}

/* --------------------------------------------------------------------------
 * Escape sequence decoding
 * ------------------------------------------------------------------------ */

/* Map the final byte of a CSI/SS3 sequence to a key. Parameters are only
 * consulted for the "N~" forms. */
static int decode_final(const char *seq, size_t len)
{
    char final = seq[len - 1u];

    switch (final) {
    case 'A':
        return K_UP;
    case 'B':
        return K_DOWN;
    case 'C':
        return K_RIGHT;
    case 'D':
        return K_LEFT;
    case 'H':
        return K_HOME;
    case 'F':
        return K_END;
    case 'Z':
        return K_SHIFT_TAB;
    default:
        break;
    }
    if (final == '~' && len >= 3u) {
        switch (seq[1]) {
        case '1':
        case '7':
            return K_HOME;
        case '3':
            return K_DEL;
        case '4':
        case '8':
            return K_END;
        default:
            break;
        }
    }
    return K_UNKNOWN;
}

/* Returns the decoded key, or -1 while the sequence is still incomplete. */
static int seq_feed(Edit *e, int byte)
{
    if (e->seqlen >= EDIT_SEQ_MAX) {
        e->in_esc = false;
        return K_UNKNOWN; /* overlong: give up rather than grow forever */
    }
    e->seq[e->seqlen++] = (char)byte;

    if (e->seqlen == 1u) {
        return -1; /* need at least the introducer plus one byte */
    }
    /* CSI and SS3 end at the first byte in 0x40..0x7e. */
    if (byte >= 0x40 && byte <= 0x7e) {
        e->in_esc = false;
        return decode_final(e->seq, e->seqlen);
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Emacs keymap. Also serves vi's insert state, so the familiar control keys
 * keep working there.
 * ------------------------------------------------------------------------ */

static EditAction emacs_motion(Edit *e, int key)
{
    switch (key) {
    case K_LEFT:
    case CTRL('B'):
        if (e->pos == 0u) {
            return EDIT_BELL;
        }
        e->pos--;
        return EDIT_REDRAW;
    case K_RIGHT:
    case CTRL('F'):
        if (e->pos >= e->len) {
            return EDIT_BELL;
        }
        e->pos++;
        return EDIT_REDRAW;
    case K_HOME:
    case CTRL('A'):
        e->pos = 0u;
        return EDIT_REDRAW;
    case K_END:
    case CTRL('E'):
        e->pos = e->len;
        return EDIT_REDRAW;
    default:
        return EDIT_NONE;
    }
}

static EditAction emacs_kill(Edit *e, int key)
{
    undo_save(e);
    switch (key) {
    case CTRL('K'):
        buf_delete(e, e->pos, e->len);
        return EDIT_REDRAW;
    case CTRL('U'):
        buf_delete(e, 0u, e->pos);
        e->pos = 0u;
        return EDIT_REDRAW;
    case CTRL('W'): {
        size_t start = word_left(e, e->pos);

        buf_delete(e, start, e->pos);
        e->pos = start;
        return EDIT_REDRAW;
    }
    default:
        return EDIT_NONE;
    }
}

/* Swap the two characters around the cursor, as readline's Ctrl-T does. */
static EditAction emacs_transpose(Edit *e)
{
    size_t at;
    char tmp;

    if (e->len < 2u || e->pos == 0u) {
        return EDIT_BELL;
    }
    at = e->pos < e->len ? e->pos : e->len - 1u;
    tmp = e->buf[at - 1u];
    e->buf[at - 1u] = e->buf[at];
    e->buf[at] = tmp;
    if (e->pos < e->len) {
        e->pos++;
    }
    return EDIT_REDRAW;
}

static EditAction emacs_delete(Edit *e, int key)
{
    if (key == K_BACKSPACE || key == CTRL('H')) {
        if (e->pos == 0u) {
            return EDIT_BELL;
        }
        undo_save(e);
        buf_delete(e, e->pos - 1u, e->pos);
        e->pos--;
        return EDIT_REDRAW;
    }
    /* K_DEL, and Ctrl-D with a non-empty buffer. */
    if (e->pos >= e->len) {
        return EDIT_BELL;
    }
    undo_save(e);
    buf_delete(e, e->pos, e->pos + 1u);
    return EDIT_REDRAW;
}

static EditAction emacs_key(Edit *e, int key)
{
    EditAction act;

    switch (key) {
    case CTRL('C'):
        return EDIT_INTR;
    case CTRL('D'):
        return e->len == 0u ? EDIT_EOF : emacs_delete(e, K_DEL);
    case CTRL('J'):
    case CTRL('M'):
        return EDIT_ACCEPT;
    case CTRL('L'):
        return EDIT_CLEAR;
    case CTRL('P'):
    case K_UP:
        return history_prev(e);
    case CTRL('N'):
    case K_DOWN:
        return history_next(e);
    case CTRL('T'):
        return emacs_transpose(e);
    case K_BACKSPACE:
    case CTRL('H'):
    case K_DEL:
        return emacs_delete(e, key);
    case CTRL('K'):
    case CTRL('U'):
    case CTRL('W'):
        return emacs_kill(e, key);
    case '\t':
        return EDIT_COMPLETE;
    default:
        break;
    }

    act = emacs_motion(e, key);
    if (act != EDIT_NONE) {
        return act;
    }
    if (key >= 0x20 && key < 0x7f) {
        char c = (char)key;

        if (!buf_insert(e, e->pos, &c, 1u)) {
            return EDIT_BELL;
        }
        e->pos++;
        return EDIT_REDRAW;
    }
    /* Any remaining byte >= 0x80 is part of a UTF-8 sequence: insert it
     * verbatim. Display width is Phase 5's problem; byte fidelity is not. */
    if (key >= 0x80 && key <= 0xff) {
        char c = (char)key;

        if (!buf_insert(e, e->pos, &c, 1u)) {
            return EDIT_BELL;
        }
        e->pos++;
        return EDIT_REDRAW;
    }
    return EDIT_BELL;
}

/* --------------------------------------------------------------------------
 * Vi keymap
 * ------------------------------------------------------------------------ */

/* Resolve a motion key to a target offset. Returns false when the key is not a
 * motion, so the caller can try the command table instead. */
static bool vi_motion(const Edit *e, int key, unsigned count, size_t *target)
{
    size_t pos = e->pos;
    unsigned i;

    switch (key) {
    case 'h':
    case K_LEFT:
        for (i = 0u; i < count && pos > 0u; i++) {
            pos--;
        }
        break;
    case 'l':
    case ' ':
    case K_RIGHT:
        for (i = 0u; i < count && pos < e->len; i++) {
            pos++;
        }
        break;
    case '0':
        pos = 0u;
        break;
    case '^':
        pos = first_non_blank(e);
        break;
    case '$':
        pos = e->len;
        break;
    case 'w':
        for (i = 0u; i < count; i++) {
            pos = word_right(e, pos);
        }
        break;
    case 'b':
        for (i = 0u; i < count; i++) {
            pos = word_left(e, pos);
        }
        break;
    case 'e':
        for (i = 0u; i < count; i++) {
            pos = word_end(e, pos);
        }
        break;
    default:
        return false;
    }
    *target = pos;
    return true;
}

/* Apply the pending operator over [from,to). `cc`/`dd` arrive here as the
 * whole-line range. */
static EditAction vi_apply_op(Edit *e, size_t from, size_t to)
{
    int op = e->vi_op;

    e->vi_op = 0;
    undo_save(e);
    if (from > to) {
        size_t t = from;

        from = to;
        to = t;
    }
    buf_delete(e, from, to);
    e->pos = from;
    if (op == 'c') {
        e->vi = EDIT_VI_INSERT;
    }
    return EDIT_REDRAW;
}

static EditAction vi_enter_insert(Edit *e, int key)
{
    switch (key) {
    case 'i':
        break;
    case 'a':
        if (e->pos < e->len) {
            e->pos++;
        }
        break;
    case 'I':
        e->pos = first_non_blank(e);
        break;
    case 'A':
        e->pos = e->len;
        break;
    default:
        return EDIT_BELL;
    }
    e->vi = EDIT_VI_INSERT;
    return EDIT_REDRAW;
}

static EditAction vi_delete_char(Edit *e, unsigned count)
{
    size_t to = e->pos + count;

    if (e->pos >= e->len) {
        return EDIT_BELL;
    }
    undo_save(e);
    buf_delete(e, e->pos, to < e->len ? to : e->len);
    return EDIT_REDRAW;
}

/* Count digits. `0` is a motion when no count is being built, which is why
 * this has to be checked before the motion table. */
static bool vi_count_digit(Edit *e, int key)
{
    if (key < '0' || key > '9') {
        return false;
    }
    if (key == '0' && e->vi_count == 0u) {
        return false;
    }
    e->vi_count = (e->vi_count * 10u) + (unsigned)(key - '0');
    return true;
}

static EditAction vi_normal_key(Edit *e, int key)
{
    unsigned count = e->vi_count == 0u ? 1u : e->vi_count;
    size_t target;

    if (e->vi_pending == 'r') {
        e->vi_pending = 0;
        if (key < 0x20 || key > 0x7e || e->pos >= e->len) {
            return EDIT_BELL;
        }
        undo_save(e);
        e->buf[e->pos] = (char)key;
        return EDIT_REDRAW;
    }
    if (vi_count_digit(e, key)) {
        return EDIT_NONE;
    }

    /* A doubled operator (dd, cc) means the whole line. */
    if (e->vi_op != 0 && key == e->vi_op) {
        e->vi_count = 0u;
        return vi_apply_op(e, 0u, e->len);
    }
    if (vi_motion(e, key, count, &target)) {
        e->vi_count = 0u;
        if (e->vi_op != 0) {
            return vi_apply_op(e, e->pos, target);
        }
        e->pos = target;
        return EDIT_REDRAW;
    }
    e->vi_count = 0u;

    switch (key) {
    case 'd':
    case 'c':
        e->vi_op = key;
        return EDIT_NONE;
    case 'i':
    case 'a':
    case 'I':
    case 'A':
        return vi_enter_insert(e, key);
    case 'x':
        return vi_delete_char(e, count);
    case 'r':
        e->vi_pending = 'r';
        return EDIT_NONE;
    case 'u':
        return undo_restore(e);
    case 'k':
    case K_UP:
        return history_prev(e);
    case 'j':
    case K_DOWN:
        return history_next(e);
    case CTRL('M'):
    case CTRL('J'):
        return EDIT_ACCEPT;
    case CTRL('C'):
        return EDIT_INTR;
    case CTRL('L'):
        return EDIT_CLEAR;
    case CTRL('D'):
        return e->len == 0u ? EDIT_EOF : EDIT_BELL;
    case K_ESC:
        e->vi_op = 0;
        return EDIT_NONE;
    default:
        /* Unbound printable keys are ignored in normal mode, never inserted. */
        return EDIT_BELL;
    }
}

/* --------------------------------------------------------------------------
 * Dispatch
 * ------------------------------------------------------------------------ */

/* --------------------------------------------------------------------------
 * Completion keymap
 *
 * Consulted first while a menu is open. Only the keys that mean something to a
 * menu are claimed; everything else falls through to ordinary editing, which
 * is what makes typing narrow the list rather than dismiss it.
 * ------------------------------------------------------------------------ */

static EditAction completion_key(int key)
{
    switch (key) {
    case '\t':
    case CTRL('N'):
    case K_RIGHT:
        return EDIT_COMP_NEXT;
    case K_SHIFT_TAB:
    case CTRL('P'):
    case K_LEFT:
        return EDIT_COMP_PREV;
    case K_UP:
        return EDIT_COMP_UP;
    case K_DOWN:
        return EDIT_COMP_DOWN;
    case CTRL('J'):
    case CTRL('M'):
        return EDIT_COMP_ACCEPT;
    case K_ESC:
    case CTRL('G'):
    case CTRL('C'):
        return EDIT_COMP_CANCEL;
    default:
        return EDIT_NONE;
    }
}

static EditAction dispatch(Edit *e, int key)
{
    if (e->completing) {
        EditAction act = completion_key(key);

        if (act != EDIT_NONE) {
            return act;
        }
    }
    if (e->keymap == EDIT_VI) {
        if (e->vi == EDIT_VI_NORMAL) {
            return vi_normal_key(e, key);
        }
        if (key == K_ESC) {
            /* Leaving insert steps left, as vi does. */
            e->vi = EDIT_VI_NORMAL;
            if (e->pos > 0u) {
                e->pos--;
            }
            return EDIT_REDRAW;
        }
    } else if (key == K_ESC) {
        return EDIT_NONE; /* a bare Esc does nothing in emacs mode */
    }
    return emacs_key(e, key);
}

bool edit_pending_escape(const Edit *e)
{
    return e != NULL && e->in_esc;
}

EditAction edit_timeout(Edit *e)
{
    if (e == NULL || !e->in_esc) {
        return EDIT_NONE;
    }
    e->in_esc = false;
    if (e->seqlen > 0u) {
        /* A sequence that stopped halfway is garbage, not an Esc: drop it
         * rather than let its tail appear as typed characters. */
        e->seqlen = 0u;
        return EDIT_NONE;
    }
    return dispatch(e, K_ESC);
}

EditAction edit_feed(Edit *e, int byte)
{
    EditAction pending = EDIT_NONE;

    if (e == NULL) {
        return EDIT_NONE;
    }
    byte &= 0xff;

    if (e->in_esc) {
        /* The introducer decides whether this is a real sequence. Anything
         * else means the Esc stood alone, so deliver it and let this byte fall
         * through as an ordinary key. */
        if (e->seqlen == 0u && byte != '[' && byte != 'O') {
            e->in_esc = false;
            pending = dispatch(e, K_ESC);
        } else {
            int key = seq_feed(e, byte);
            if (key < 0) {
                return EDIT_NONE; /* sequence still incomplete */
            }
            if (key == K_UNKNOWN) {
                return EDIT_NONE; /* swallowed, never inserted as literal */
            }
            return dispatch(e, key);
        }
    }

    if (byte == K_ESC) {
        e->in_esc = true;
        e->seqlen = 0u;
        return pending;
    }
    {
        EditAction act = dispatch(e, byte);

        return act != EDIT_NONE ? act : pending;
    }
}
