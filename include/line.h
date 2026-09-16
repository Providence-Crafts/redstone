/*
 * line.h - the terminal layer over the editing core.
 *
 * Owns the termios raw mode, the redraw, and the history file. All key
 * semantics live in `edit.c`; this module only turns bytes into screen output.
 * When the input is not a tty it degrades to plain reads, so pipes and here
 * documents behave exactly as they did before line editing existed.
 */
#ifndef SQLSH_LINE_H
#define SQLSH_LINE_H

#include "edit.h"

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

void line_set_keymap(Line *ln, EditKeymap keymap);
EditKeymap line_keymap(const Line *ln);

bool line_history_add(Line *ln, const char *text);

/* Which vi state the last edited line ended in, so the caller can show it in
 * the prompt. Always EDIT_VI_INSERT under the emacs keymap. */
EditViState line_vi_state(const Line *ln);

/* Default path: $XDG_STATE_HOME/sqlsh/history, else ~/.local/state/sqlsh/
 * history. Returns NULL when neither XDG_STATE_HOME nor HOME is set. The
 * caller frees. */
char *line_history_path(void);

/* Both are best-effort: a missing or unreadable file is not an error, because
 * losing history must never stop the shell from starting. */
bool line_history_load(Line *ln, const char *path);
bool line_history_save(const Line *ln, const char *path);

#endif /* SQLSH_LINE_H */
