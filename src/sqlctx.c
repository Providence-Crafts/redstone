/*
 * sqlctx.c - SQL tokenizer and cursor-context analysis.
 *
 * The analyser makes one pass over the statement the cursor sits in. It keeps
 * a small amount of state -- which clause we are in, what sort of token came
 * last, how deep the parentheses go -- and snapshots that state at the moment
 * it reaches the cursor. Scanning then continues to the end of the statement,
 * but only to finish collecting table names, because a FROM clause typed after
 * the cursor still tells us which columns are in scope. Going back to edit the
 * select list of a finished query is the normal way to use a shell, so that
 * second half of the pass is what makes completion there work at all.
 *
 * No allocation, no I/O, no recursion.
 */
#include "sqlctx.h"

#include <string.h>

#define SQL_MAX_DEPTH 16u

/* --- character classes -------------------------------------------------- */

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Bytes >= 0x80 are accepted so that a UTF-8 identifier lexes as one token
 * rather than a run of unrecognised punctuation. */
static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (unsigned char)c >= 0x80u;
}

static bool is_ident(char c)
{
    return is_ident_start(c) || is_digit(c) || c == '$';
}

static char lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)((c - 'A') + 'a');
    }
    return c;
}

/* --- lexer -------------------------------------------------------------- */

void sql_lex_init(SqlLexer *lx, const char *text)
{
    if (lx == NULL) {
        return;
    }
    lx->text = (text != NULL) ? text : "";
    lx->pos = 0u;
}

/* Scan a quoted run opening at S[i] and closing at QUOTE, where the quote
 * character doubled stands for itself. Returns the offset just past the close,
 * or the end of the buffer with *OK cleared when it never closes. */
static size_t scan_quoted(const char *s, size_t i, char quote, bool *ok)
{
    i++;
    for (;;) {
        if (s[i] == '\0') {
            *ok = false;
            return i;
        }
        if (s[i] == quote) {
            if (s[i + 1u] == quote) {
                i += 2u;
                continue;
            }
            return i + 1u;
        }
        i++;
    }
}

static size_t scan_number(const char *s, size_t i)
{
    if (s[i] == '0' && (s[i + 1u] == 'x' || s[i + 1u] == 'X') && is_hex(s[i + 2u])) {
        i += 2u;
        while (is_hex(s[i])) {
            i++;
        }
        return i;
    }
    while (is_digit(s[i])) {
        i++;
    }
    if (s[i] == '.') {
        i++;
        while (is_digit(s[i])) {
            i++;
        }
    }
    if (s[i] == 'e' || s[i] == 'E') {
        size_t j = i + 1u;

        if (s[j] == '+' || s[j] == '-') {
            j++;
        }
        if (is_digit(s[j])) {
            i = j;
            while (is_digit(s[i])) {
                i++;
            }
        }
    }
    return i;
}

/* True when S[i..] opens a two-character operator. */
static bool two_char_op(const char *s, size_t i)
{
    static const char *const ops[] = {"<=", ">=", "<>", "!=", "==", "||", "<<", ">>", "->"};
    size_t k;

    for (k = 0u; k < sizeof(ops) / sizeof(ops[0]); k++) {
        if (s[i] == ops[k][0] && s[i + 1u] == ops[k][1]) {
            return true;
        }
    }
    return false;
}

static bool is_op_char(char c)
{
    return strchr("+-*/%<>=!&|~", c) != NULL && c != '\0';
}

/* Either comment form. A line comment that runs to the end of the buffer is
 * "unterminated" so that a cursor at its end counts as inside it; one ended by
 * a newline is closed, and the newline is not part of the token. */
static bool lex_comment(const char *s, size_t *i, SqlToken *tok)
{
    size_t j = *i;

    if (s[j] == '-' && s[j + 1u] == '-') {
        j += 2u;
        while (s[j] != '\0' && s[j] != '\n') {
            j++;
        }
        tok->terminated = s[j] == '\n';
    } else if (s[j] == '/' && s[j + 1u] == '*') {
        j += 2u;
        while (s[j] != '\0' && !(s[j] == '*' && s[j + 1u] == '/')) {
            j++;
        }
        if (s[j] == '\0') {
            tok->terminated = false;
        } else {
            j += 2u;
        }
    } else {
        return false;
    }
    tok->kind = SQL_TOK_COMMENT;
    *i = j;
    return true;
}

/* Comments and everything delimited by a quote. Returns false when S[i] does
 * not open one of them, leaving TOK untouched. Split out of sql_lex_next
 * purely so neither half needs a reader to hold ten branches in mind at once. */
static bool lex_delimited(const char *s, size_t *i, SqlToken *tok)
{
    size_t j = *i;

    if (lex_comment(s, &j, tok)) {
        *i = j;
        return true;
    }
    if (s[j] == '\'') {
        j = scan_quoted(s, j, '\'', &tok->terminated);
        tok->kind = SQL_TOK_STRING;
    } else if ((s[j] == 'x' || s[j] == 'X') && s[j + 1u] == '\'') {
        j = scan_quoted(s, j + 1u, '\'', &tok->terminated);
        tok->kind = SQL_TOK_BLOB;
    } else if (s[j] == '"' || s[j] == '`') {
        j = scan_quoted(s, j, s[j], &tok->terminated);
        tok->kind = SQL_TOK_QUOTED_IDENT;
    } else if (s[j] == '[') {
        /* MS-Access style brackets: no escape, so the first ] closes. */
        j++;
        while (s[j] != '\0' && s[j] != ']') {
            j++;
        }
        if (s[j] == ']') {
            j++;
        } else {
            tok->terminated = false;
        }
        tok->kind = SQL_TOK_QUOTED_IDENT;
    } else {
        return false;
    }
    *i = j;
    return true;
}

/* Everything else: numbers, words, parameters, operators and stray
 * punctuation. Always consumes at least one byte, so the lexer cannot stall. */
static void lex_plain(const char *s, size_t *i, SqlToken *tok)
{
    size_t j = *i;

    if (is_digit(s[j]) || (s[j] == '.' && is_digit(s[j + 1u]))) {
        j = scan_number(s, j);
        tok->kind = SQL_TOK_NUMBER;
    } else if (is_ident_start(s[j])) {
        while (is_ident(s[j])) {
            j++;
        }
        tok->kind = SQL_TOK_IDENT;
    } else if (s[j] == '?') {
        j++;
        while (is_digit(s[j])) {
            j++;
        }
        tok->kind = SQL_TOK_PARAM;
    } else if ((s[j] == ':' || s[j] == '@' || s[j] == '$') && is_ident(s[j + 1u])) {
        j++;
        while (is_ident(s[j])) {
            j++;
        }
        tok->kind = SQL_TOK_PARAM;
    } else if (is_op_char(s[j])) {
        j += two_char_op(s, j) ? 2u : 1u;
        tok->kind = SQL_TOK_OPERATOR;
    } else {
        j++;
        tok->kind = SQL_TOK_PUNCT;
    }
    *i = j;
}

bool sql_lex_next(SqlLexer *lx, SqlToken *tok)
{
    const char *s;
    size_t i;

    if (lx == NULL || tok == NULL || lx->text == NULL) {
        return false;
    }
    s = lx->text;
    i = lx->pos;
    while (is_space(s[i])) {
        i++;
    }
    if (s[i] == '\0') {
        lx->pos = i;
        return false;
    }

    tok->off = i;
    tok->terminated = true;
    if (!lex_delimited(s, &i, tok)) {
        lex_plain(s, &i, tok);
    }
    tok->len = i - tok->off;
    lx->pos = i;
    return true;
}

/* --- keywords ----------------------------------------------------------- */

/* Only the keywords that shape the grammar are listed. This is not a
 * completion source: those are enumerated from the library at run time. */
typedef enum {
    KW_NONE = 0,
    KW_SELECT,
    KW_DISTINCT,
    KW_FROM,
    KW_JOIN,
    KW_ON,
    KW_USING,
    KW_WHERE,
    KW_GROUP,
    KW_ORDER,
    KW_BY,
    KW_HAVING,
    KW_LIMIT,
    KW_OFFSET,
    KW_INSERT,
    KW_INTO,
    KW_VALUES,
    KW_UPDATE,
    KW_SET,
    KW_DELETE,
    KW_CREATE,
    KW_DROP,
    KW_ALTER,
    KW_TABLE,
    KW_PRAGMA,
    KW_AS,
    KW_CONJUNCTION, /* AND OR NOT */
    KW_COMPARISON,  /* IN IS LIKE BETWEEN GLOB MATCH REGEXP */
    KW_OTHER        /* a keyword, but one that only ends an alias */
} Keyword;

typedef struct {
    const char *name;
    Keyword kw;
} KeywordEntry;

static const KeywordEntry g_keywords[] = {
    {"select", KW_SELECT},
    {"distinct", KW_DISTINCT},
    {"all", KW_DISTINCT},
    {"from", KW_FROM},
    {"join", KW_JOIN},
    {"on", KW_ON},
    {"using", KW_USING},
    {"where", KW_WHERE},
    {"group", KW_GROUP},
    {"order", KW_ORDER},
    {"by", KW_BY},
    {"having", KW_HAVING},
    {"limit", KW_LIMIT},
    {"offset", KW_OFFSET},
    {"insert", KW_INSERT},
    {"into", KW_INTO},
    {"values", KW_VALUES},
    {"update", KW_UPDATE},
    {"set", KW_SET},
    {"delete", KW_DELETE},
    {"create", KW_CREATE},
    {"drop", KW_DROP},
    {"alter", KW_ALTER},
    {"table", KW_TABLE},
    {"view", KW_TABLE},
    {"index", KW_OTHER},
    {"trigger", KW_OTHER},
    {"pragma", KW_PRAGMA},
    {"as", KW_AS},
    {"and", KW_CONJUNCTION},
    {"or", KW_CONJUNCTION},
    {"not", KW_CONJUNCTION},
    {"in", KW_COMPARISON},
    {"is", KW_COMPARISON},
    {"like", KW_COMPARISON},
    {"between", KW_COMPARISON},
    {"glob", KW_COMPARISON},
    {"match", KW_COMPARISON},
    {"regexp", KW_COMPARISON},
    {"natural", KW_OTHER},
    {"left", KW_OTHER},
    {"right", KW_OTHER},
    {"full", KW_OTHER},
    {"inner", KW_OTHER},
    {"cross", KW_OTHER},
    {"outer", KW_OTHER},
    {"union", KW_OTHER},
    {"intersect", KW_OTHER},
    {"except", KW_OTHER},
    {"case", KW_OTHER},
    {"when", KW_OTHER},
    {"then", KW_OTHER},
    {"else", KW_OTHER},
    {"end", KW_OTHER},
    {"with", KW_OTHER},
    {"explain", KW_OTHER},
    {"exists", KW_OTHER},
    {"asc", KW_OTHER},
    {"desc", KW_OTHER},
    {"replace", KW_OTHER},
    {"if", KW_OTHER},
    {"temp", KW_OTHER},
    {"temporary", KW_OTHER},
    {"virtual", KW_OTHER},
    {"collate", KW_OTHER},
    {"escape", KW_OTHER},
    {"null", KW_OTHER},
    {"cast", KW_OTHER},
    {"begin", KW_OTHER},
    {"commit", KW_OTHER},
    {"rollback", KW_OTHER},
    {"attach", KW_OTHER},
    {"detach", KW_OTHER},
    {"analyze", KW_OTHER},
    {"vacuum", KW_OTHER},
    {"reindex", KW_OTHER},
    {"returning", KW_OTHER},
    {"window", KW_OTHER},
    {"over", KW_OTHER},
    {"filter", KW_OTHER},
    {"recursive", KW_OTHER},
};

/* Case-insensitive match of a token span against a NUL-terminated lowercase
 * name. */
static bool span_is(const char *text, const SqlToken *tok, const char *name)
{
    size_t i;

    for (i = 0u; i < tok->len; i++) {
        if (name[i] == '\0' || lower(text[tok->off + i]) != name[i]) {
            return false;
        }
    }
    return name[i] == '\0';
}

static Keyword keyword_of(const char *text, const SqlToken *tok)
{
    size_t k;

    if (tok->kind != SQL_TOK_IDENT) {
        return KW_NONE;
    }
    for (k = 0u; k < sizeof(g_keywords) / sizeof(g_keywords[0]); k++) {
        if (span_is(text, tok, g_keywords[k].name)) {
            return g_keywords[k].kw;
        }
    }
    return KW_NONE;
}

/* --- scanner state ------------------------------------------------------ */

typedef enum {
    CL_NONE = 0,
    CL_SELECT,
    CL_FROM,
    CL_WHERE,
    CL_SET,
    CL_GROUP,
    CL_ORDER,
    CL_LIMIT,
    CL_INSERT,
    CL_UPDATE,
    CL_PRAGMA,
    CL_DDL
} Clause;

/* What sort of thing the previous token was, which is what decides whether the
 * cursor wants a new operand or a keyword to continue with. */
typedef enum {
    LK_START = 0, /* nothing yet in this statement */
    LK_OPERAND,   /* a complete value: identifier, literal, parameter, ) */
    LK_OPERATOR,
    LK_COMMA,
    LK_OPEN,
    LK_KEYWORD
} LastKind;

typedef enum { EXP_NONE = 0, EXP_TABLE, EXP_ALIAS } Expect;

typedef enum { PK_GROUP = 0, PK_CALL, PK_VALUES } ParenKind;

typedef struct {
    Clause clause;
    LastKind last;
    Expect expect;
    bool overwrite; /* the next table name replaces the last, after "db." */
    bool after_dot; /* the previous token was "." */
    unsigned depth;
    ParenKind pkind[SQL_MAX_DEPTH];
    Clause saved[SQL_MAX_DEPTH];
    size_t ntokens;             /* tokens seen in this statement */
    char operand[SQL_NAME_MAX]; /* the last bare operand, for CTX_VALUE */
} Scan;

static void scan_init(Scan *sc)
{
    memset(sc, 0, sizeof(*sc));
}

/* Copy a token's text, stripping the quotes of a quoted identifier. Truncates
 * silently: a name longer than SQL_NAME_MAX cannot be completed usefully
 * anyway, and a truncated prefix simply matches nothing. */
static void copy_name(char *dst, const char *text, size_t off, size_t len)
{
    size_t n;

    if (len >= 2u) {
        char q = text[off];

        if (q == '"' || q == '`' || q == '[') {
            off++;
            len -= 2u;
        }
    }
    n = (len < SQL_NAME_MAX - 1u) ? len : SQL_NAME_MAX - 1u;
    memcpy(dst, text + off, n);
    dst[n] = '\0';
}

static void add_table(SqlContext *ctx, Scan *sc, const char *text, const SqlToken *tok)
{
    if (sc->overwrite && ctx->ntables > 0u) {
        /* "main.t": the name recorded a moment ago was the schema. */
        copy_name(ctx->tables[ctx->ntables - 1u].name, text, tok->off, tok->len);
        sc->overwrite = false;
        return;
    }
    if (ctx->ntables < SQL_CTX_MAX_TABLES) {
        copy_name(ctx->tables[ctx->ntables].name, text, tok->off, tok->len);
        ctx->tables[ctx->ntables].alias[0] = '\0';
        ctx->ntables++;
    }
}

static void set_alias(SqlContext *ctx, const char *text, const SqlToken *tok)
{
    if (ctx->ntables > 0u) {
        copy_name(ctx->tables[ctx->ntables - 1u].alias, text, tok->off, tok->len);
    }
}

/* Apply a keyword to the clause state. */
static void scan_keyword(Scan *sc, SqlContext *ctx, Keyword kw)
{
    Expect keep = sc->expect;

    sc->last = LK_KEYWORD;
    sc->expect = EXP_NONE;

    switch (kw) {
    case KW_SELECT:
        sc->clause = CL_SELECT;
        break;
    case KW_FROM:
    case KW_JOIN:
        sc->clause = CL_FROM;
        sc->expect = EXP_TABLE;
        break;
    case KW_INTO:
        sc->expect = EXP_TABLE;
        break;
    case KW_UPDATE:
        sc->clause = CL_UPDATE;
        sc->expect = EXP_TABLE;
        break;
    case KW_TABLE:
        if (sc->clause == CL_DDL) {
            sc->expect = EXP_TABLE;
        }
        break;
    case KW_ON:
    case KW_USING:
    case KW_WHERE:
    case KW_HAVING:
        sc->clause = CL_WHERE;
        break;
    case KW_SET:
        sc->clause = CL_SET;
        break;
    case KW_GROUP:
        sc->clause = CL_GROUP;
        break;
    case KW_ORDER:
        sc->clause = CL_ORDER;
        break;
    case KW_LIMIT:
    case KW_OFFSET:
        sc->clause = CL_LIMIT;
        break;
    case KW_INSERT:
    case KW_VALUES:
        sc->clause = CL_INSERT;
        break;
    case KW_DELETE:
        sc->clause = CL_NONE;
        break;
    case KW_CREATE:
    case KW_DROP:
    case KW_ALTER:
        sc->clause = CL_DDL;
        break;
    case KW_PRAGMA:
        sc->clause = CL_PRAGMA;
        break;
    case KW_COMPARISON:
        /* IN, LIKE and friends are operators that happen to be spelled as
         * words; the operand before them is the column being compared. */
        sc->last = LK_OPERATOR;
        memcpy(ctx->column, sc->operand, sizeof(ctx->column));
        break;
    case KW_CONJUNCTION:
        ctx->column[0] = '\0';
        break;
    case KW_AS:
        sc->expect = keep; /* "t AS x": x is still the alias */
        break;
    case KW_BY:
    case KW_DISTINCT:
    case KW_OTHER:
    case KW_NONE:
    default:
        break;
    }
}

static void scan_open_paren(Scan *sc)
{
    ParenKind kind = PK_GROUP;

    if (sc->last == LK_OPERAND && !sc->after_dot && sc->expect == EXP_NONE) {
        kind = PK_CALL; /* an identifier immediately before "(" is a call */
    } else if (sc->last == LK_OPERATOR || sc->clause == CL_INSERT) {
        kind = PK_VALUES; /* IN (...), VALUES (...) */
    }
    if (sc->expect == EXP_ALIAS) {
        /* "INSERT INTO t (" opens a column list, not a call. */
        kind = PK_GROUP;
    }
    if (sc->depth < SQL_MAX_DEPTH) {
        sc->pkind[sc->depth] = kind;
        sc->saved[sc->depth] = sc->clause;
    }
    sc->depth++;
    sc->last = LK_OPEN;
    sc->expect = EXP_NONE;
}

static void scan_close_paren(Scan *sc)
{
    if (sc->depth > 0u) {
        sc->depth--;
        if (sc->depth < SQL_MAX_DEPTH) {
            sc->clause = sc->saved[sc->depth];
        }
    }
    sc->last = LK_OPERAND;
}

/* AFTER_DOT says the previous token was ".", so this operand is the tail of a
 * qualified name and must not be mistaken for an alias or a new operand. */
static void scan_operand(Scan *sc, SqlContext *ctx, const char *text, const SqlToken *tok,
                         bool after_dot)
{
    if (sc->expect == EXP_TABLE) {
        add_table(ctx, sc, text, tok);
        sc->expect = EXP_ALIAS;
    } else if (sc->expect == EXP_ALIAS && !after_dot) {
        set_alias(ctx, text, tok);
        sc->expect = EXP_NONE;
    }
    /* The tail of a qualified name is the operand: in "c.country = ?" the
     * column being compared is country, not c. */
    copy_name(sc->operand, text, tok->off, tok->len);
    sc->last = LK_OPERAND;
}

/* Feed one token. Returns false when the statement ended, so the caller can
 * stop. */
static bool scan_token(Scan *sc, SqlContext *ctx, const char *text, const SqlToken *tok)
{
    bool was_dot = sc->after_dot;
    Keyword kw;

    if (tok->kind == SQL_TOK_COMMENT) {
        return true; /* comments are invisible to the grammar */
    }
    sc->after_dot = false;
    sc->ntokens++;

    if (tok->kind == SQL_TOK_PUNCT) {
        char c = text[tok->off];

        if (c == ';') {
            return false;
        }
        if (c == '(') {
            scan_open_paren(sc);
        } else if (c == ')') {
            scan_close_paren(sc);
        } else if (c == ',') {
            if (sc->clause == CL_FROM && sc->depth == 0u) {
                sc->expect = EXP_TABLE;
            }
            sc->last = LK_COMMA;
            ctx->column[0] = '\0';
        } else if (c == '.') {
            sc->after_dot = true;
            if (sc->expect == EXP_ALIAS) {
                sc->expect = EXP_TABLE;
                sc->overwrite = true;
            }
        } else {
            sc->last = LK_OPERAND;
        }
        return true;
    }

    if (tok->kind == SQL_TOK_OPERATOR) {
        sc->last = LK_OPERATOR;
        memcpy(ctx->column, sc->operand, sizeof(ctx->column));
        return true;
    }

    kw = keyword_of(text, tok);
    if (kw != KW_NONE) {
        scan_keyword(sc, ctx, kw);
        return true;
    }

    scan_operand(sc, ctx, text, tok, was_dot);
    if (tok->kind != SQL_TOK_IDENT && tok->kind != SQL_TOK_QUOTED_IDENT) {
        ctx->column[0] = '\0'; /* a literal is not a column name */
    }
    return true;
}

/* --- dot commands ------------------------------------------------------- */

/* Offset of the first non-blank byte. */
static size_t skip_blank(const char *text, size_t i)
{
    while (is_space(text[i])) {
        i++;
    }
    return i;
}

static void copy_word(char *dst, const char *text, size_t off, size_t len)
{
    size_t n = (len < SQL_NAME_MAX - 1u) ? len : SQL_NAME_MAX - 1u;

    memcpy(dst, text + off, n);
    dst[n] = '\0';
}

/* Analyse a line whose first non-blank character is a dot. */
static void dot_context(const char *text, size_t cursor, SqlContext *ctx, size_t start)
{
    size_t end = start + 1u;
    size_t i;

    while (text[end] != '\0' && !is_space(text[end])) {
        end++;
    }
    copy_word(ctx->dot_command, text, start + 1u, end - (start + 1u));

    if (cursor <= end) {
        ctx->kind = CTX_DOT_COMMAND;
        ctx->word_off = start + 1u;
        if (cursor > start) {
            copy_word(ctx->word, text, start + 1u, cursor - (start + 1u));
        }
        return;
    }

    ctx->kind = CTX_DOT_ARG;
    ctx->dot_argno = 0u;
    ctx->word_off = cursor;
    i = end;
    for (;;) {
        size_t wstart;

        i = skip_blank(text, i);
        if (i >= cursor || text[i] == '\0') {
            break;
        }
        wstart = i;
        while (text[i] != '\0' && !is_space(text[i])) {
            i++;
        }
        ctx->dot_argno++;
        if (cursor <= i) {
            /* The cursor is inside or at the end of this argument. */
            ctx->word_off = wstart;
            copy_word(ctx->word, text, wstart, cursor - wstart);
            return;
        }
    }
    /* The cursor is on blank space: a fresh, empty argument. */
    ctx->dot_argno++;
}

/* --- context derivation ------------------------------------------------- */

static SqlCtxKind kind_from_clause(const Scan *sc)
{
    switch (sc->clause) {
    case CL_SELECT:
        return (sc->last == LK_OPERAND) ? CTX_KEYWORD : CTX_SELECT_LIST;
    case CL_FROM:
    case CL_UPDATE:
    case CL_DDL:
        return (sc->expect == EXP_TABLE) ? CTX_TABLE : CTX_KEYWORD;
    case CL_WHERE:
    case CL_SET:
        if (sc->last == LK_OPERATOR) {
            return CTX_VALUE;
        }
        return (sc->last == LK_OPERAND) ? CTX_KEYWORD : CTX_COLUMN;
    case CL_GROUP:
    case CL_ORDER:
        return (sc->last == LK_OPERAND) ? CTX_KEYWORD : CTX_COLUMN;
    case CL_INSERT:
        if (sc->expect == EXP_TABLE) {
            return CTX_TABLE;
        }
        return (sc->depth > 0u) ? CTX_COLUMN : CTX_KEYWORD;
    case CL_PRAGMA:
        if (sc->last == LK_KEYWORD) {
            return CTX_PRAGMA;
        }
        return (sc->last == LK_OPERAND) ? CTX_KEYWORD : CTX_UNKNOWN;
    case CL_LIMIT:
        return CTX_KEYWORD;
    case CL_NONE:
    default:
        return (sc->ntokens == 0u) ? CTX_STATEMENT_START : CTX_KEYWORD;
    }
}

static SqlCtxKind derive(const Scan *sc, const SqlContext *ctx)
{
    ParenKind pk =
        (sc->depth > 0u && sc->depth <= SQL_MAX_DEPTH) ? sc->pkind[sc->depth - 1u] : PK_GROUP;
    bool fresh = sc->last == LK_OPEN || sc->last == LK_COMMA || sc->last == LK_OPERATOR;

    if (ctx->qualifier[0] != '\0') {
        /* "x.y": a column of x, unless a table name was expected, in which
         * case x was a schema. */
        return (sc->expect == EXP_TABLE) ? CTX_TABLE : CTX_COLUMN;
    }
    if (sc->depth > 0u && fresh) {
        if (pk == PK_VALUES) {
            return CTX_VALUE;
        }
        if (pk == PK_CALL) {
            return CTX_FUNCTION;
        }
    }
    return kind_from_clause(sc);
}

/* True when CURSOR sits within TOK rather than at a boundary after it. */
static bool cursor_inside(const SqlToken *tok, size_t cursor)
{
    if (cursor <= tok->off) {
        return false;
    }
    return cursor < tok->off + tok->len || !tok->terminated;
}

static bool opaque_kind(SqlTokenKind kind)
{
    return kind == SQL_TOK_STRING || kind == SQL_TOK_BLOB || kind == SQL_TOK_COMMENT ||
           kind == SQL_TOK_QUOTED_IDENT;
}

/* Where one token stands relative to the cursor. */
typedef enum {
    HIT_BEFORE = 0, /* wholly before it: feed it to the scanner and go on */
    HIT_HERE,       /* the cursor is in or at the end of this token */
    HIT_OPAQUE      /* the cursor is inside a literal or a comment */
} CursorHit;

/* Classify TOK against the cursor, recording the partial word when there is
 * one. */
static CursorHit cursor_hit(const SqlToken *tok, size_t cursor, const char *text, SqlContext *ctx)
{
    if (tok->off >= cursor) {
        return HIT_HERE;
    }
    if (cursor_inside(tok, cursor)) {
        if (opaque_kind(tok->kind)) {
            return HIT_OPAQUE;
        }
        if (tok->kind == SQL_TOK_IDENT) {
            copy_word(ctx->word, text, tok->off, cursor - tok->off);
            ctx->word_off = tok->off;
        }
        return HIT_HERE;
    }
    if (tok->kind == SQL_TOK_IDENT && cursor == tok->off + tok->len) {
        /* The cursor sits at the end of a word with no gap: that word is the
         * prefix being completed, so it is not fed to the scanner. */
        copy_word(ctx->word, text, tok->off, tok->len);
        ctx->word_off = tok->off;
        return HIT_HERE;
    }
    return HIT_BEFORE;
}

/* A semicolon before the cursor ends a statement we no longer care about. */
static void restart(Scan *sc, SqlContext *ctx)
{
    scan_init(sc);
    memset(ctx->tables, 0, sizeof(ctx->tables));
    ctx->ntables = 0u;
    ctx->column[0] = '\0';
}

void sql_context(const char *text, size_t cursor, SqlContext *ctx)
{
    SqlLexer lx;
    SqlToken tok;
    Scan sc;
    Scan at_cursor;
    bool reached = false;
    size_t start;

    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->kind = CTX_UNKNOWN;
    if (text == NULL) {
        return;
    }
    if (cursor > strlen(text)) {
        cursor = strlen(text);
    }
    ctx->word_off = cursor;

    start = skip_blank(text, 0u);
    if (text[start] == '.') {
        dot_context(text, cursor, ctx, start);
        return;
    }

    scan_init(&sc);
    at_cursor = sc;
    sql_lex_init(&lx, text);
    while (sql_lex_next(&lx, &tok)) {
        if (!reached) {
            CursorHit hit = cursor_hit(&tok, cursor, text, ctx);

            if (hit == HIT_OPAQUE) {
                ctx->kind = CTX_UNKNOWN;
                return;
            }
            if (hit == HIT_HERE) {
                reached = true;
                at_cursor = sc;
                if (sc.after_dot) {
                    /* "c.", with or without a partial word after the dot. */
                    memcpy(ctx->qualifier, sc.operand, sizeof(ctx->qualifier));
                }
            }
        }
        if (!scan_token(&sc, ctx, text, &tok)) {
            if (reached) {
                break; /* the statement under the cursor ended */
            }
            restart(&sc, ctx);
        }
    }
    if (!reached) {
        at_cursor = sc;
    }
    ctx->kind = derive(&at_cursor, ctx);
}

const char *sql_ctx_kind_name(SqlCtxKind kind)
{
    switch (kind) {
    case CTX_DOT_COMMAND:
        return "dot-command";
    case CTX_DOT_ARG:
        return "dot-arg";
    case CTX_STATEMENT_START:
        return "statement-start";
    case CTX_SELECT_LIST:
        return "select-list";
    case CTX_TABLE:
        return "table";
    case CTX_COLUMN:
        return "column";
    case CTX_VALUE:
        return "value";
    case CTX_FUNCTION:
        return "function";
    case CTX_PRAGMA:
        return "pragma";
    case CTX_KEYWORD:
        return "keyword";
    case CTX_UNKNOWN:
    default:
        return "unknown";
    }
}
