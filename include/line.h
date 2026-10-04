/*
 * line.h - the terminal layer over the editing core.
 *
 * Owns the termios raw mode, the redraw, and the history file. All key
 * semantics live in `edit.c`; this module only turns bytes into screen output.
 * When the input is not a tty it degrades to plain reads, so pipes and here
 * documents behave exactly as they did before line editing existed.
 */
#ifndef REDSTONE_LINE_H
#define REDSTONE_LINE_H

#include "comp.h"
#include "edit.h"
#include "hl.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct Line Line;

/* Result of one edited line. */
typedef enum {
    LINE_OK = 0, /* a line was read; see line_text */
    LINE_EOF,    /* end of input */
    LINE_INTR,   /* Ctrl-C: discard the pending statement, keep going */
    LINE_ERROR   /* read or allocation failure */
} LineStatus;

Line *line_new(FILE *in, FILE *out);
void line_free(Line *ln);

/* Read one line. The returned text excludes the newline and stays valid until
 * the next call. */
LineStatus line_read(Line *ln, const char *prompt);
const char *line_text(const Line *ln);

/* True when both streams are terminals; false means no editing, no prompt
 * decoration and no history. */
bool line_interactive(const Line *ln);

/* Force interactive on or off, for -interactive and -batch. */
void line_set_interactive(Line *ln, bool on);

void line_set_keymap(Line *ln, EditKeymap keymap);
EditKeymap line_keymap(const Line *ln);

bool line_history_add(Line *ln, const char *text);

/* Preload TEXT into the buffer of the next line_read call, then discard it.
 * NULL clears a pending seed. What `.edit` uses to hand back an edited line
 * for the user to look over rather than executing it unseen. */
void line_seed(Line *ln, const char *text);

/* Read access to the history ring, for `.edit` to find "the previous line"
 * when it is given no text of its own to edit. */
size_t line_history_count(const Line *ln);
const char *line_history_at(const Line *ln, size_t i); /* NULL when out of range */

/* Run $VISUAL/$EDITOR/vi on TEXT (LEN bytes) and return what came back, or
 * NULL on failure; caller frees. Does not touch raw mode -- the caller is
 * expected to already be outside it (as `.edit` is) or to suspend it itself
 * (as the Ctrl-X Ctrl-E chord does). */
char *line_external_edit(const char *text, size_t len);

/* The completion hook. `generate` produces the candidates for TEXT with the
 * cursor at CURSOR and the line layer takes ownership of the list; NULL means
 * there is nothing to offer. Passing a generator in rather than calling comp.c
 * directly keeps this module unaware of the database, the same way comp.c is
 * kept unaware of the dot-command table. The struct is copied. */
typedef struct {
    CompList *(*generate)(void *ctx, const char *text, size_t cursor);
    void *ctx;
} LineCompleter;

void line_set_completer(Line *ln, const LineCompleter *completer);

/* The highlighting hook, injected for the same reason the completer is: this
 * module must not know what a table is. SCHEMA is copied; NULL turns
 * highlighting back to the purely lexical colouring. */
void line_set_highlighter(Line *ln, const HlSchema *schema);

/* Which vi state the last edited line ended in, so the caller can show it in
 * the prompt. Always EDIT_VI_INSERT under the emacs keymap. */
EditViState line_vi_state(const Line *ln);

/* Default path: $XDG_STATE_HOME/redstone/history, else ~/.local/state/redstone/
 * history. Returns NULL when neither XDG_STATE_HOME nor HOME is set. The
 * caller frees. */
char *line_history_path(void);

/* Both are best-effort: a missing or unreadable file is not an error, because
 * losing history must never stop the shell from starting. */
bool line_history_load(Line *ln, const char *path);
bool line_history_save(const Line *ln, const char *path);

#endif /* REDSTONE_LINE_H */
