/*
 * theme_slots.h - the program-specific half of the theme.
 *
 * theme.c is a generic engine shared verbatim with librarian; what differs
 * between the programs is here and in theme_builtin.c: the program name (for
 * error prefixes and the config path), the style slots, where each slot is
 * written in a theme file, and the shipped palettes.
 */
#ifndef REDSTONE_THEME_SLOTS_H
#define REDSTONE_THEME_SLOTS_H

#define THEME_PROGRAM "redstone"

typedef enum {
    THEME_RESET = 0,
    /* [menu] */
    THEME_SELECTED, /* the highlighted menu row */
    THEME_MATCH,    /* the prefix the user has already typed */
    THEME_DETAIL,   /* the description column */
    THEME_GROUP,    /* a group header */
    THEME_NOTE,     /* counts, truncation notices */
    THEME_VALUE,    /* a column value offered as a candidate */
    /* [syntax]: schema objects, token kinds, and the error cue */
    THEME_TABLE,
    THEME_VIEW,
    THEME_COLUMN,
    THEME_FUNCTION,
    THEME_PRAGMA,
    THEME_KEYWORD,
    THEME_DOT,
    THEME_NUMBER,
    THEME_COMMENT,
    THEME_OPERATOR,
    THEME_PARAM,
    THEME_IDENT, /* a bare name the schema does not know */
    THEME_ERROR, /* unclosed quote, unbalanced paren */
    /* [output]: result values, by storage class, plus the column titles */
    THEME_HEADER,
    THEME_NULL,
    THEME_INTEGER,
    THEME_REAL,
    THEME_STRING, /* also a string literal in the input line */
    THEME_BLOB,
    /* [brand]: the banner and the prompt */
    THEME_BRAND_GLYPH,
    THEME_BRAND_NAME,
    THEME_BRAND_TAGLINE,
    THEME_BRAND_FACTS,
    THEME_STYLE_COUNT
} ThemeStyle;

/* Where each style is written in a theme file. RESET is not configurable: it
 * is the sequence that ends every other style, so a user who could redefine it
 * could leave the terminal in any state at all. Its entry is {NULL, NULL}. */
typedef struct {
    const char *section;
    const char *key;
} StyleName;

extern const StyleName theme_names[THEME_STYLE_COUNT];

/* A built-in palette, written in the theme-file format. */
typedef struct {
    const char *name;
    const char *text;
} ThemeBuiltin;

/* NULL-terminated; the first entry is "default". */
extern const ThemeBuiltin theme_builtins[];

#endif /* REDSTONE_THEME_SLOTS_H */
