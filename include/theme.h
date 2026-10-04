/*
 * theme.h - colour capability, the style table, and the theme file.
 *
 * One process, one theme, so the state is global: threading a style pointer
 * through every drawing call would be ceremony for a shell that draws one
 * screen. Every style resolves to an SGR string, or to "" when colour is off,
 * so callers never branch on whether colour is enabled — they just emit the
 * style and get nothing when it is disabled.
 *
 * No colour in the program is hardcoded at a call site: the built-in palettes
 * are theme-file text parsed by the same parser a user's file goes through, so
 * `.theme dump` round-trips and a shipped theme cannot drift from what the
 * parser accepts.
 */
#ifndef THEME_H
#define THEME_H

#include "theme_slots.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Decide from the environment and the stream: colour is off when NO_COLOR is
 * set to anything non-empty, when TERM is absent or "dumb", or when OUT is not
 * a terminal. Call once at startup; a later theme_set_colour overrides it. */
void theme_detect(FILE *out);
void theme_set_colour(bool on);
bool theme_colour(void);

/* The SGR sequence for STYLE, or "" when colour is off. Never NULL. */
const char *theme_sgr(ThemeStyle style);

/* --- the theme file ------------------------------------------------------
 *
 * INI: "[section]" headers over "key = value" lines. A line whose first
 * non-blank character is "#" is a comment, and "--" comments to end of line
 * as it does in SQL -- "#" cannot, because a value may contain a #rrggbb
 * colour. A value is a space-separated list of attributes (bold,
 * dim, italic, underline, reverse) and at most one foreground and one
 * background colour, the background introduced by "on":
 *
 *     [syntax]
 *     keyword = bold blue
 *     comment = dim
 *     error   = bold #ff5f5f on 52
 *
 * A colour is a name (black red green yellow blue magenta cyan white, each
 * with a "bright" prefix), a 0-255 palette index, or #rrggbb. "none" clears
 * the style. Unknown sections, keys and values are reported and ignored: a
 * theme file is a preference, so a typo in one must never stop the shell or
 * lose the rest of the palette.
 * ---------------------------------------------------------------------- */

/* Reset to the built-in "default" palette. */
void theme_reset(void);

/* Apply the theme-file text in TEXT, reporting problems to ERR (which may be
 * NULL) with ORIGIN as the file name. Styles the text does not mention keep
 * their current value, so a short file is a patch over the default palette.
 * Returns false when anything was rejected. */
bool theme_apply(const char *text, const char *origin, FILE *err);

/* As theme_apply, reading PATH. A missing file is not a failure: it means the
 * user has no theme, which is the normal case. */
bool theme_load_file(const char *path, FILE *err);

/* Load a built-in palette by name, or, when NAME is not a built-in, a file of
 * that name. Every load starts from the built-in default, so themes do not
 * accumulate. Returns false after reporting to ERR. */
bool theme_load(const char *name, FILE *err);

/* The built-in palette names, NULL past the end. The first is "default". */
const char *theme_name_at(size_t i);

/* Default theme path: <plat_config_dir>/<THEME_PROGRAM>/theme, i.e.
 * $XDG_CONFIG_HOME/<program>/theme or ~/.config/<program>/theme, and
 * %APPDATA%/<program>/theme on Windows. NULL when no config dir is known.
 * The caller frees. */
char *theme_path(void);

/* Write the current palette as a theme file. What comes out is accepted by
 * theme_apply, so it is a starting point for editing as well as a listing. */
void theme_dump(FILE *out);

#endif /* THEME_H */
