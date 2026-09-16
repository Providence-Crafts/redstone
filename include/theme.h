/*
 * theme.h - colour capability and the style table.
 *
 * One process, one theme, so the state is global: threading a style pointer
 * through every drawing call would be ceremony for a shell that draws one
 * screen. Every style resolves to an SGR string, or to "" when colour is off,
 * so callers never branch on whether colour is enabled — they just emit the
 * style and get nothing when it is disabled.
 *
 * Phase 5 extends this with the output modes' styles and Phase 7 with a theme
 * file; the capability rules below are final.
 */
#ifndef SQLSH_THEME_H
#define SQLSH_THEME_H

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    THEME_RESET = 0,
    THEME_SELECTED, /* the highlighted menu row */
    THEME_MATCH,    /* the prefix the user has already typed */
    THEME_DETAIL,   /* the description column */
    THEME_GROUP,    /* a group header */
    THEME_NOTE,     /* counts, truncation notices */
    THEME_TABLE,
    THEME_VIEW,
    THEME_COLUMN,
    THEME_VALUE,
    THEME_FUNCTION,
    THEME_PRAGMA,
    THEME_KEYWORD,
    THEME_DOT,
    /* Result values, by storage class, plus the column titles above them. */
    THEME_HEADER,
    THEME_NULL,
    THEME_INTEGER,
    THEME_REAL,
    THEME_STRING,
    THEME_BLOB,
    THEME_STYLE_COUNT
} ThemeStyle;

/* Decide from the environment and the stream: colour is off when NO_COLOR is
 * set to anything non-empty, when TERM is absent or "dumb", or when OUT is not
 * a terminal. Call once at startup; a later theme_set_colour overrides it. */
void theme_detect(FILE *out);
void theme_set_colour(bool on);
bool theme_colour(void);

/* The SGR sequence for STYLE, or "" when colour is off. Never NULL. */
const char *theme_sgr(ThemeStyle style);

#endif /* SQLSH_THEME_H */
