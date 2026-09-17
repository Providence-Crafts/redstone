#include "hl.h"

#include "theme.h"

#include <ctype.h>
#include <string.h>

/* Unbalanced parentheses are reported by offset, and there is no allocation in
 * this module, so the number tracked is capped. A line with more than this many
 * unclosed parentheses has one obvious problem already; colouring the first
 * thirty-two of them says so just as clearly as colouring all of them. */
#define HL_MAX_ERRORS 32u

typedef struct {
    size_t off[HL_MAX_ERRORS];
    size_t n;
} ErrorSet;

static void err_add(ErrorSet *set, size_t off)
{
    if (set->n < HL_MAX_ERRORS) {
        set->off[set->n] = off;
        set->n++;
    }
}

static bool err_has(const ErrorSet *set, size_t off)
{
    size_t i;

    for (i = 0u; i < set->n; i++) {
        if (set->off[i] == off) {
            return true;
        }
    }
    return false;
}

/* A first pass for the errors that are only visible once the whole line has
 * been read: a "(" never closed, and a ")" that closes nothing. Both are
 * common while typing, which is why they are a colour rather than a message --
 * and why the error colour doubles as the answer to "why is the prompt still
 * asking for a continuation line?". */
static void find_unbalanced(const char *text, ErrorSet *errs)
{
    size_t open[HL_MAX_ERRORS];
    size_t depth = 0u;
    SqlLexer lx;
    SqlToken tok;

    sql_lex_init(&lx, text);
    while (sql_lex_next(&lx, &tok)) {
        if (tok.kind != SQL_TOK_PUNCT || tok.len != 1u) {
            continue;
        }
        if (text[tok.off] == '(') {
            if (depth < HL_MAX_ERRORS) {
                open[depth] = tok.off;
            }
            depth++;
        } else if (text[tok.off] == ')') {
            if (depth == 0u) {
                err_add(errs, tok.off);
            } else {
                depth--;
            }
        }
    }
    while (depth > 0u) {
        depth--;
        if (depth < HL_MAX_ERRORS) {
            err_add(errs, open[depth]);
        }
    }
}

/* Write the part of TEXT[OFF, OFF+LEN) that falls inside the window, wrapped
 * in STYLE. Clipping here rather than at the call sites is what lets the
 * analysis run over the whole line while only the visible span is drawn. */
static void emit(FILE *out, const char *text, size_t off, size_t len, size_t from, size_t to,
                 ThemeStyle style)
{
    size_t begin = off > from ? off : from;
    size_t end = off + len < to ? off + len : to;
    const char *sgr;

    if (begin >= end) {
        return;
    }
    /* THEME_RESET as a style means "unstyled": the gaps between tokens, and
     * everything on a line the highlighter has nothing to say about. */
    sgr = style == THEME_RESET ? "" : theme_sgr(style);
    if (sgr[0] != '\0') {
        fputs(sgr, out);
    }
    (void)fwrite(text + begin, 1u, end - begin, out);
    if (sgr[0] != '\0') {
        fputs(theme_sgr(THEME_RESET), out);
    }
}

/* The character that follows a token, ignoring whitespace: "count" is a
 * function only when a "(" comes next, and a bare "count" is just a name. */
static char next_visible(const char *text, size_t after)
{
    while (text[after] != '\0' && isspace((unsigned char)text[after])) {
        after++;
    }
    return text[after];
}

static ThemeStyle style_of_ident(const char *text, const SqlToken *tok, const SqlContext *ctx,
                                 const HlSchema *schema)
{
    const char *name = text + tok->off;
    bool is_view = false;

    if (schema == NULL) {
        return THEME_IDENT;
    }
    /* Keyword first: SELECT is a keyword even in a database that has a table
     * of that name, because that is how the parser will read it. */
    if (schema->is_keyword != NULL && schema->is_keyword(schema->ctx, name, tok->len)) {
        return THEME_KEYWORD;
    }
    if (next_visible(text, tok->off + tok->len) == '(' && schema->is_function != NULL &&
        schema->is_function(schema->ctx, name, tok->len)) {
        return THEME_FUNCTION;
    }
    if (schema->is_table != NULL && schema->is_table(schema->ctx, name, tok->len, &is_view)) {
        return is_view ? THEME_VIEW : THEME_TABLE;
    }
    if (schema->is_column != NULL &&
        schema->is_column(schema->ctx, name, tok->len, ctx->tables, ctx->ntables)) {
        return THEME_COLUMN;
    }
    return THEME_IDENT;
}

static ThemeStyle style_of(const char *text, const SqlToken *tok, const SqlContext *ctx,
                           const HlSchema *schema, const ErrorSet *errs)
{
    if (!tok->terminated) {
        return THEME_ERROR;
    }
    switch (tok->kind) {
    case SQL_TOK_STRING:
        return THEME_STRING;
    case SQL_TOK_BLOB:
        return THEME_BLOB;
    case SQL_TOK_NUMBER:
        return THEME_NUMBER;
    case SQL_TOK_PARAM:
        return THEME_PARAM;
    case SQL_TOK_COMMENT:
        return THEME_COMMENT;
    case SQL_TOK_OPERATOR:
        return THEME_OPERATOR;
    case SQL_TOK_PUNCT:
        return err_has(errs, tok->off) ? THEME_ERROR : THEME_OPERATOR;
    case SQL_TOK_QUOTED_IDENT:
    case SQL_TOK_IDENT:
    default:
        return style_of_ident(text, tok, ctx, schema);
    }
}

/* A dot command is not SQL and must not be lexed as it: ".mode box" would come
 * out as punctuation followed by two bare words. The command itself is the
 * only part with a known vocabulary, so it is the only part coloured. */
static bool write_dot_line(FILE *out, const char *text, size_t from, size_t to)
{
    size_t i = 0u;
    size_t start;

    while (text[i] != '\0' && isspace((unsigned char)text[i])) {
        i++;
    }
    if (text[i] != '.') {
        return false;
    }
    start = i;
    i++;
    while (text[i] != '\0' && !isspace((unsigned char)text[i])) {
        i++;
    }
    emit(out, text, 0u, start, from, to, THEME_RESET);
    emit(out, text, start, i - start, from, to, THEME_DOT);
    emit(out, text, i, strlen(text) - i, from, to, THEME_RESET);
    return true;
}

void hl_write(FILE *out, const char *text, size_t from, size_t to, const HlSchema *schema)
{
    SqlContext ctx;
    SqlLexer lx;
    SqlToken tok;
    ErrorSet errs;
    size_t len = strlen(text);
    size_t pos = 0u;

    if (to > len) {
        to = len;
    }
    if (from >= to) {
        return;
    }
    if (!theme_colour()) {
        (void)fwrite(text + from, 1u, to - from, out);
        return;
    }
    if (write_dot_line(out, text, from, to)) {
        return;
    }

    errs.n = 0u;
    find_unbalanced(text, &errs);
    /* The context is computed once for the whole line, not once per token: all
     * the highlighter wants from it is which tables are in scope, which is a
     * property of the statement rather than of the cursor. */
    sql_context(text, len, &ctx);

    sql_lex_init(&lx, text);
    while (sql_lex_next(&lx, &tok)) {
        /* The gap before the token is whitespace by construction, so it is
         * written unstyled rather than lexed. */
        emit(out, text, pos, tok.off - pos, from, to, THEME_RESET);
        emit(out, text, tok.off, tok.len, from, to, style_of(text, &tok, &ctx, schema, &errs));
        pos = tok.off + tok.len;
    }
    emit(out, text, pos, len - pos, from, to, THEME_RESET);
}
