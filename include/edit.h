/*
 * edit.h - the line editing core: buffer, cursor, keymaps, history.
 *
 * This module performs no I/O whatsoever. It is fed input bytes one at a time
 * and reports what the caller should do about the screen. That is what makes
 * every keybinding testable without a pty; `line.c` is the thin terminal layer
 * that drives it.
 */
#ifndef SQLSH_EDIT_H
#define SQLSH_EDIT_H

#include <stdbool.h>
#include <stddef.h>

/* What the caller must do after feeding a byte. */
typedef enum {
    EDIT_NONE = 0, /* nothing visible changed */
    EDIT_REDRAW,   /* buffer or cursor moved; repaint the line */
    EDIT_ACCEPT,   /* Enter: the buffer is a finished line */
    EDIT_EOF,      /* Ctrl-D on an empty buffer */
    EDIT_INTR,     /* Ctrl-C: abandon the line and any pending statement */
    EDIT_CLEAR,    /* Ctrl-L: clear the screen, then repaint */
    EDIT_BELL      /* the key means nothing here */
} EditAction;

typedef enum { EDIT_EMACS = 0, EDIT_VI } EditKeymap;

/* Vi's two states. Emacs mode is the degenerate case that never leaves
 * EDIT_VI_INSERT, which is why one state machine serves both keymaps. */
typedef enum { EDIT_VI_INSERT = 0, EDIT_VI_NORMAL } EditViState;

typedef struct Edit Edit;

Edit *edit_new(void);
void edit_free(Edit *e);

/* Begin a new line: empty buffer, cursor at 0, history browsing reset. The
 * keymap and the history ring survive. */
void edit_reset(Edit *e);

/* Feed one input byte. Escape sequences are accumulated internally, so a
 * sequence split across reads decodes correctly. */
EditAction edit_feed(Edit *e, int byte);

/* True while an escape sequence is part-read. A lone Esc is indistinguishable
 * from the start of an arrow key until either the next byte arrives or enough
 * time passes without one, so the terminal layer waits briefly and then calls
 * edit_timeout to say no byte followed. That keeps the timing decision in the
 * layer that owns the file descriptor and leaves this module deterministic. */
bool edit_pending_escape(const Edit *e);
EditAction edit_timeout(Edit *e);

const char *edit_buffer(const Edit *e); /* NUL-terminated, never NULL */
size_t edit_len(const Edit *e);
size_t edit_cursor(const Edit *e); /* byte offset, 0..len */

/* Replace the buffer wholesale; cursor goes to the end. Used to seed a line. */
bool edit_set_buffer(Edit *e, const char *text);

void edit_set_keymap(Edit *e, EditKeymap keymap);
EditKeymap edit_keymap(const Edit *e);
EditViState edit_vi_state(const Edit *e);

/* History. Entries are added most-recent-last. A repeat of the most recent
 * entry, and an all-whitespace entry, are both ignored. */
bool edit_history_add(Edit *e, const char *text);
size_t edit_history_count(const Edit *e);
const char *edit_history_at(const Edit *e, size_t i); /* NULL when out of range */
void edit_history_clear(Edit *e);

/* Bound on retained entries; the oldest are dropped first. Default 1000. */
void edit_history_set_max(Edit *e, size_t max);
size_t edit_history_max(const Edit *e);

#endif /* SQLSH_EDIT_H */
