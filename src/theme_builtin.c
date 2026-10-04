/*
 * theme_builtin.c - redstone's style names and built-in palettes.
 *
 * The program-specific half of the theme: which section and key each style
 * slot is written under, and the palettes shipped in the binary. theme.c is
 * the engine and holds nothing program-specific, so it is copied as is
 * between redstone and librarian; this file and theme_slots.h are not.
 */
#include "theme_slots.h"

#include <stddef.h>

/* clang-format off */
const StyleName theme_names[THEME_STYLE_COUNT] = {
    {NULL,     NULL},         /* RESET */
    {"menu",   "selected"},
    {"menu",   "match"},
    {"menu",   "detail"},
    {"menu",   "group"},
    {"menu",   "note"},
    {"menu",   "value"},
    {"syntax", "table"},
    {"syntax", "view"},
    {"syntax", "column"},
    {"syntax", "function"},
    {"syntax", "pragma"},
    {"syntax", "keyword"},
    {"syntax", "dot"},
    {"syntax", "number"},
    {"syntax", "comment"},
    {"syntax", "operator"},
    {"syntax", "parameter"},
    {"syntax", "identifier"},
    {"syntax", "error"},
    {"output", "header"},
    {"output", "null"},
    {"output", "integer"},
    {"output", "real"},
    {"output", "string"},
    {"output", "blob"},
    {"brand",  "glyph"},
    {"brand",  "name"},
    {"brand",  "tagline"},
    {"brand",  "facts"}
};

/* The built-in palettes, written in the file format so that the parser is the
 * only thing that turns a colour into an escape sequence -- and so that a
 * shipped theme cannot express anything a user's file cannot.
 *
 * "default" uses the 256-colour cube rather than the eight basic colours: the
 * basic set is remapped by most terminal themes, which is how a "blue" comment
 * ends up unreadable on a blue background. Its choices sit in the mid range,
 * legible on light and dark grounds alike. */
static const char g_default[] =
    "[menu]\n"
    "selected = reverse\n"          /* follows the terminal's own colours */
    "match = bold 39\n"
    "detail = 245\n"
    "group = bold 244\n"
    "note = 244\n"
    "value = 180\n"
    "[syntax]\n"
    "table = 75\n"
    "view = 79\n"
    "column = 223\n"
    "function = 141\n"
    "pragma = 108\n"
    "keyword = 110\n"
    "dot = 215\n"
    "number = 215\n"
    "comment = 244\n"
    "operator = 252\n"
    "parameter = 141\n"
    "identifier = none\n"
    "error = bold 203\n"
    "[output]\n"
    "header = bold 252\n"
    "null = 244\n"                  /* dim, so an absent value recedes */
    "integer = 215\n"
    "real = 216\n"
    "string = 151\n"
    "blob = 139\n"
    "[brand]\n"
    "glyph = 160\n"
    "name = bold 160\n"
    "tagline = 110\n"
    "facts = 244\n";

/* Saturated and high-contrast, for a dark ground. */
static const char g_dark[] =
    "[menu]\n"
    "selected = reverse\n"
    "match = bold 45\n"
    "detail = 102\n"
    "group = bold 109\n"
    "note = 102\n"
    "value = 222\n"
    "[syntax]\n"
    "table = 81\n"
    "view = 85\n"
    "column = 229\n"
    "function = 177\n"
    "pragma = 114\n"
    "keyword = bold 111\n"
    "dot = 221\n"
    "number = 209\n"
    "comment = italic 102\n"
    "operator = 254\n"
    "parameter = 177\n"
    "identifier = none\n"
    "error = bold 210\n"
    "[output]\n"
    "header = bold 231\n"
    "null = 102\n"
    "integer = 209\n"
    "real = 216\n"
    "string = 150\n"
    "blob = 176\n"
    "[brand]\n"
    "glyph = 196\n"
    "name = bold 196\n"
    "tagline = 110\n"
    "facts = 244\n";

/* Darker inks, for a light ground: on white, anything above about 250 in the
 * cube disappears, which is what makes the default palette hard to read
 * there. */
static const char g_light[] =
    "[menu]\n"
    "selected = reverse\n"
    "match = bold 26\n"
    "detail = 240\n"
    "group = bold 238\n"
    "note = 240\n"
    "value = 94\n"
    "[syntax]\n"
    "table = 25\n"
    "view = 29\n"
    "column = 94\n"
    "function = 91\n"
    "pragma = 22\n"
    "keyword = bold 26\n"
    "dot = 130\n"
    "number = 130\n"
    "comment = italic 244\n"
    "operator = 238\n"
    "parameter = 91\n"
    "identifier = none\n"
    "error = bold 124\n"
    "[output]\n"
    "header = bold 232\n"
    "null = 244\n"
    "integer = 130\n"
    "real = 131\n"
    "string = 28\n"
    "blob = 91\n"
    "[brand]\n"
    "glyph = 160\n"
    "name = bold 160\n"
    "tagline = 25\n"
    "facts = 244\n";

/* The eight ANSI colours only, for a terminal that has no 256-colour mode --
 * a Linux console, a serial line, a TERM the library does not know. */
static const char g_basic[] =
    "[menu]\n"
    "selected = reverse\n"
    "match = bold cyan\n"
    "detail = bright black\n"
    "group = bold bright black\n"
    "note = bright black\n"
    "value = yellow\n"
    "[syntax]\n"
    "table = bright cyan\n"
    "view = cyan\n"
    "column = yellow\n"
    "function = magenta\n"
    "pragma = green\n"
    "keyword = bold blue\n"
    "dot = yellow\n"
    "number = yellow\n"
    "comment = bright black\n"
    "operator = none\n"
    "parameter = magenta\n"
    "identifier = none\n"
    "error = bold red\n"
    "[output]\n"
    "header = bold\n"
    "null = bright black\n"
    "integer = yellow\n"
    "real = yellow\n"
    "string = green\n"
    "blob = magenta\n"
    "[brand]\n"
    "glyph = red\n"
    "name = bold red\n"
    "tagline = blue\n"
    "facts = bright black\n";

const ThemeBuiltin theme_builtins[] = {{"default", g_default},
                                       {"dark", g_dark},
                                       {"light", g_light},
                                       {"basic", g_basic},
                                       {NULL, NULL}};
/* clang-format on */
