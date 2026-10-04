/*
 * sqlctx.h - SQL tokenizer and cursor-context analysis.
 *
 * Two things live here, and neither touches a terminal, a database or the
 * heap:
 *
 *   1. A lexer over SQLite's surface syntax. It is an iterator, so the caller
 *      supplies the storage and nothing is allocated. Phase 7's highlighter
 *      uses it unchanged; that is why token kinds are part of this header
 *      rather than an implementation detail.
 *
 *   2. sql_context, which answers the only question completion actually asks:
 *      given a buffer and a cursor offset, what may legally appear *here*?
 *
 * This is deliberately not a parser. It never decides whether a statement is
 * valid or what it means, only where the cursor stands in it, so a half-typed
 * and quite illegal fragment still yields a useful answer. That is the normal
 * case while completing, not the exception.
 */
#ifndef REDSTONE_SQLCTX_H
#define REDSTONE_SQLCTX_H

#include <stdbool.h>
#include <stddef.h>

/* --- lexer -------------------------------------------------------------- */

typedef enum {
    SQL_TOK_IDENT = 0,    /* bare word: identifier or keyword, undistinguished */
    SQL_TOK_QUOTED_IDENT, /* "x", `x`, [x] */
    SQL_TOK_STRING,       /* 'x' */
    SQL_TOK_BLOB,         /* x'00ff' */
    SQL_TOK_NUMBER,       /* 1, 1.5, 1e3, 0xff */
    SQL_TOK_PARAM,        /* ?, ?1, :name, @name, $name */
    SQL_TOK_OPERATOR,     /* = < > + || ... */
    SQL_TOK_PUNCT,        /* ( ) , ; . and anything else unrecognised */
    SQL_TOK_COMMENT       /* -- to end of line, or a block comment */
} SqlTokenKind;

/* A token is a span of the original buffer; nothing is copied. */
typedef struct {
    SqlTokenKind kind;
    size_t off;
    size_t len;
    /* False for a string, blob, quoted identifier or block comment that the
     * buffer ends before closing. Half-typed input hits this constantly, so it
     * is a normal state rather than an error: the span simply runs to the end
     * of the buffer. */
    bool terminated;
} SqlToken;

typedef struct {
    const char *text;
    size_t pos;
} SqlLexer;

void sql_lex_init(SqlLexer *lx, const char *text);

/* Write the next token into TOK and return true; return false at end of
 * input. Whitespace is skipped, so the gaps between token spans are exactly
 * the whitespace. */
bool sql_lex_next(SqlLexer *lx, SqlToken *tok);

/* --- context ------------------------------------------------------------ */

typedef enum {
    CTX_UNKNOWN = 0,     /* nothing sensible to offer */
    CTX_DOT_COMMAND,     /* the name of a dot command */
    CTX_DOT_ARG,         /* an argument to one; see dot_command and dot_argno */
    CTX_STATEMENT_START, /* the first word of a statement */
    CTX_SELECT_LIST,     /* between SELECT and FROM */
    CTX_TABLE,           /* a table or view name */
    CTX_COLUMN,          /* a column of the in-scope tables */
    CTX_VALUE,           /* a literal; see the column field */
    CTX_FUNCTION,        /* inside a call's argument list */
    CTX_PRAGMA,          /* a pragma name */
    CTX_KEYWORD          /* only a keyword can continue the statement */
} SqlCtxKind;

#define SQL_NAME_MAX 128
#define SQL_CTX_MAX_TABLES 8

typedef struct {
    char name[SQL_NAME_MAX];
    char alias[SQL_NAME_MAX]; /* "" when the table was not aliased */
} SqlCtxTable;

typedef struct {
    SqlCtxKind kind;

    /* The partial word the cursor sits at the end of, and where it starts.
     * Empty when the cursor is at whitespace or punctuation. The offset is
     * what a completion replaces. */
    char word[SQL_NAME_MAX];
    size_t word_off;

    /* The "t" of "t.col|". Empty when the word is unqualified. */
    char qualifier[SQL_NAME_MAX];

    /* Tables named by FROM, JOIN, UPDATE or INSERT INTO, in source order.
     * Overflow beyond SQL_CTX_MAX_TABLES is dropped; a query joining more than
     * eight tables is past the point where completing it helps. */
    SqlCtxTable tables[SQL_CTX_MAX_TABLES];
    size_t ntables;

    /* For CTX_VALUE: the column being compared, unqualified, so the caller can
     * ask the database for its distinct values. Empty when unknown. */
    char column[SQL_NAME_MAX];

    /* For CTX_DOT_COMMAND and CTX_DOT_ARG: the command without its leading
     * dot, and the 1-based index of the argument under the cursor. */
    char dot_command[SQL_NAME_MAX];
    size_t dot_argno;
} SqlContext;

/* Fill CTX for the cursor at byte offset CURSOR in TEXT. CURSOR beyond the
 * end of TEXT is clamped. Never fails: an input it cannot make sense of
 * yields CTX_UNKNOWN with everything else empty. */
void sql_context(const char *text, size_t cursor, SqlContext *ctx);

/* Stable lowercase name of a context, for tests and diagnostics. */
const char *sql_ctx_kind_name(SqlCtxKind kind);

#endif /* REDSTONE_SQLCTX_H */
