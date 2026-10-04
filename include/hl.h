/*
 * hl.h - syntax highlighting for the line being typed.
 *
 * Pure: it is handed a buffer and a place to write, and asks a caller-supplied
 * schema for the few things only the database knows. That keeps `line.c`
 * unaware of the database — exactly as it already is for completion — and lets
 * every colouring decision be tested without a terminal or a connection.
 *
 * The lexer is the Phase 2 one, unchanged. Two lexers over the same syntax
 * would drift, and the whole reason `sqlctx.h` exposes token kinds is so that
 * this module can reuse them.
 */
#ifndef REDSTONE_HL_H
#define REDSTONE_HL_H

#include "sqlctx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* What the highlighter cannot know by itself. Every callback may be NULL, and
 * a NULL schema is legal: the result is then the purely lexical colouring —
 * strings, numbers, comments and errors — with every bare word left plain.
 *
 * NAME is not NUL-terminated; it is LEN bytes of the line being edited, spelled
 * as the user typed it: SQL is case-insensitive, so every callback must match
 * case-insensitively or "SELECT" and "select" would colour differently. The
 * callbacks are called once per identifier per redraw, so they must answer
 * from a cache rather than by querying: `db.c` already caches every list these
 * come from, invalidated by the schema cookie. */
typedef struct {
    bool (*is_keyword)(void *ctx, const char *name, size_t len);
    bool (*is_function)(void *ctx, const char *name, size_t len);
    /* Sets *IS_VIEW when the object is a view rather than a table. */
    bool (*is_table)(void *ctx, const char *name, size_t len, bool *is_view);
    /* TABLES are the tables named by the statement's FROM/UPDATE/INTO clause,
     * so that a column is only looked for where it could be in scope. */
    bool (*is_column)(void *ctx, const char *name, size_t len, const SqlCtxTable *tables,
                      size_t ntables);
    void *ctx;
} HlSchema;

/* Write TEXT[FROM, TO) to OUT, coloured. The whole of TEXT is analysed even
 * when only part of it is written, because a string that opened before FROM
 * still colours what follows: the window is a horizontal scroll of the line,
 * not a separate line. When colour is off the bytes are written unchanged, so
 * callers never branch on it. */
void hl_write(FILE *out, const char *text, size_t from, size_t to, const HlSchema *schema);

#endif /* REDSTONE_HL_H */
