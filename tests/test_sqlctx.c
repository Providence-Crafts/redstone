/*
 * Tests for the tokenizer and the cursor-context analyser.
 *
 * Context cases are written as one string with a caret marking the cursor.
 * The caret is stripped before analysis, so the case reads exactly like the
 * line a user would have half-typed. Nothing here needs a terminal or a
 * database, which is the whole point of keeping this module pure.
 */
#include "minunit.h"
#include "sqlctx.h"
#include "suites.h"

#include <stdio.h>
#include <string.h>

#define CURSOR_MARK '^'

/* Strip the caret out of SRC into DST and return its offset. */
static size_t split_cursor(const char *src, char *dst, size_t cap)
{
    size_t i;
    size_t j = 0u;
    size_t cursor = 0u;

    for (i = 0u; src[i] != '\0' && j + 1u < cap; i++) {
        if (src[i] == CURSOR_MARK) {
            cursor = j;
        } else {
            dst[j] = src[i];
            j++;
        }
    }
    dst[j] = '\0';
    return cursor;
}

static void analyze(const char *marked, SqlContext *ctx)
{
    char buf[256];
    size_t cursor = split_cursor(marked, buf, sizeof(buf));

    sql_context(buf, cursor, ctx);
}

/* --- lexer -------------------------------------------------------------- */

static const char *test_lex_kinds(void)
{
    static const struct {
        const char *text;
        SqlTokenKind kind;
    } cases[] = {
        {"name", SQL_TOK_IDENT},       {"\"odd name\"", SQL_TOK_QUOTED_IDENT},
        {"`x`", SQL_TOK_QUOTED_IDENT}, {"[x y]", SQL_TOK_QUOTED_IDENT},
        {"'text'", SQL_TOK_STRING},    {"x'00ff'", SQL_TOK_BLOB},
        {"12", SQL_TOK_NUMBER},        {"1.5e-3", SQL_TOK_NUMBER},
        {"0xff", SQL_TOK_NUMBER},      {"?1", SQL_TOK_PARAM},
        {":name", SQL_TOK_PARAM},      {"@a", SQL_TOK_PARAM},
        {"$a", SQL_TOK_PARAM},         {"<=", SQL_TOK_OPERATOR},
        {"||", SQL_TOK_OPERATOR},      {",", SQL_TOK_PUNCT},
        {"-- c", SQL_TOK_COMMENT},     {"/* c */", SQL_TOK_COMMENT},
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SqlLexer lx;
        SqlToken tok;
        static char msg[128];

        sql_lex_init(&lx, cases[i].text);
        (void)snprintf(msg, sizeof(msg), "wrong kind or span for %s", cases[i].text);
        mu_assert(msg, sql_lex_next(&lx, &tok));
        mu_assert(msg, tok.kind == cases[i].kind);
        mu_assert(msg, tok.off == 0u && tok.len == strlen(cases[i].text));
        mu_assert(msg, !sql_lex_next(&lx, &tok));
    }
    return NULL;
}

/* Doubled quotes are an escape, not a close; getting this wrong makes the rest
 * of the line lex as garbage. */
static const char *test_lex_escapes(void)
{
    SqlLexer lx;
    SqlToken tok;

    sql_lex_init(&lx, "'it''s' \"a\"\"b\"");
    mu_assert("string with '' failed", sql_lex_next(&lx, &tok));
    mu_assert("string with '' has the wrong length", tok.len == 7u && tok.terminated);
    mu_assert("quoted ident failed", sql_lex_next(&lx, &tok));
    mu_assert("quoted ident with \"\" has the wrong length", tok.len == 6u);
    mu_assert("unexpected trailing token", !sql_lex_next(&lx, &tok));
    return NULL;
}

static const char *test_lex_unterminated(void)
{
    static const char *const cases[] = {"'abc", "\"abc", "/* abc", "-- abc", "x'0"};
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SqlLexer lx;
        SqlToken tok;

        sql_lex_init(&lx, cases[i]);
        mu_assert("unterminated token did not lex", sql_lex_next(&lx, &tok));
        mu_assert("unterminated token reported as closed", !tok.terminated);
        mu_assert("unterminated token should span to the end",
                  tok.off + tok.len == strlen(cases[i]));
        mu_assert("unterminated token should be the last", !sql_lex_next(&lx, &tok));
    }
    return NULL;
}

static const char *test_lex_sequence(void)
{
    SqlLexer lx;
    SqlToken tok;
    size_t n = 0u;

    /* SELECT a . b FROM t WHERE x >= 1 ; and the comment: twelve in all. */
    sql_lex_init(&lx, "SELECT a.b FROM t WHERE x >= 1; -- done");
    while (sql_lex_next(&lx, &tok)) {
        n++;
    }
    mu_assert("wrong token count", n == 12u);
    return NULL;
}

/* --- context table ------------------------------------------------------ */

static const char *test_context_table(void)
{
    static const struct {
        const char *input;
        SqlCtxKind kind;
    } cases[] = {
        {"^", CTX_STATEMENT_START},
        {"SEL^", CTX_STATEMENT_START},
        {"SELECT 1; ^", CTX_STATEMENT_START},
        {"SELECT ^", CTX_SELECT_LIST},
        {"SELECT na^ FROM t", CTX_SELECT_LIST},
        {"SELECT a, ^ FROM t", CTX_SELECT_LIST},
        {"SELECT DISTINCT ^ FROM t", CTX_SELECT_LIST},
        {"SELECT a ^", CTX_KEYWORD},
        {"SELECT * FROM ^", CTX_TABLE},
        {"SELECT * FROM comp^", CTX_TABLE},
        {"SELECT * FROM main.^", CTX_TABLE},
        {"SELECT * FROM t ^", CTX_KEYWORD},
        {"SELECT * FROM a JOIN ^", CTX_TABLE},
        {"SELECT * FROM a, ^", CTX_TABLE},
        {"SELECT * FROM t WHERE ^", CTX_COLUMN},
        {"SELECT * FROM a JOIN b ON ^", CTX_COLUMN},
        {"SELECT * FROM t GROUP BY ^", CTX_COLUMN},
        {"SELECT * FROM t ORDER BY ^", CTX_COLUMN},
        {"SELECT * FROM t WHERE id = ^", CTX_VALUE},
        {"SELECT * FROM t WHERE id IN (^", CTX_VALUE},
        {"SELECT * FROM t WHERE name LIKE ^", CTX_VALUE},
        {"SELECT * FROM t WHERE id = 5 ^", CTX_KEYWORD},
        {"SELECT * FROM t WHERE id = 5 AND ^", CTX_COLUMN},
        {"SELECT count(^", CTX_FUNCTION},
        {"SELECT * FROM (SELECT ^", CTX_SELECT_LIST},
        {"SELECT c.^ FROM companies c", CTX_COLUMN},
        {"SELECT c.na^ FROM companies c", CTX_COLUMN},
        {"PRAGMA ^", CTX_PRAGMA},
        {"PRAGMA table_^", CTX_PRAGMA},
        {"UPDATE ^", CTX_TABLE},
        {"UPDATE t SET ^", CTX_COLUMN},
        {"UPDATE t SET a = ^", CTX_VALUE},
        {"INSERT INTO ^", CTX_TABLE},
        {"INSERT INTO t (^", CTX_COLUMN},
        {"INSERT INTO t VALUES (^", CTX_VALUE},
        {"DROP TABLE ^", CTX_TABLE},
        {"CREATE TABLE ^", CTX_TABLE},
        {".ta^", CTX_DOT_COMMAND},
        {"  .^", CTX_DOT_COMMAND},
        {".schema com^", CTX_DOT_ARG},
        {".schema ^", CTX_DOT_ARG},
        /* Case must not matter. */
        {"select * from t where ^", CTX_COLUMN},
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SqlContext ctx;
        static char msg[256];

        analyze(cases[i].input, &ctx);
        (void)snprintf(msg, sizeof(msg), "%s: expected %s, got %s", cases[i].input,
                       sql_ctx_kind_name(cases[i].kind), sql_ctx_kind_name(ctx.kind));
        mu_assert(msg, ctx.kind == cases[i].kind);
    }
    return NULL;
}

/* A cursor inside a literal, a comment or a quoted name must give up rather
 * than guess: offering table names inside a string is worse than offering
 * nothing. */
static const char *test_context_opaque(void)
{
    static const char *const cases[] = {
        "SELECT * FROM t WHERE a = 'ab^c'", "SELECT * FROM t WHERE a = 'ab^",
        "SELECT * FROM t -- a com^ment",    "SELECT /* a com^ment */ 1",
        "SELECT \"col^umn\" FROM t",        "SELECT x'00^ff'",
    };
    size_t i;

    for (i = 0u; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SqlContext ctx;
        static char msg[256];

        analyze(cases[i], &ctx);
        (void)snprintf(msg, sizeof(msg), "%s: expected unknown, got %s", cases[i],
                       sql_ctx_kind_name(ctx.kind));
        mu_assert(msg, ctx.kind == CTX_UNKNOWN);
    }
    return NULL;
}

static const char *test_context_word(void)
{
    SqlContext ctx;

    analyze("SELECT nam^e FROM t", &ctx);
    mu_assert("a cursor mid-word should yield the prefix", strcmp(ctx.word, "nam") == 0);
    mu_assert("word_off should point at the start of the word", ctx.word_off == 7u);

    analyze("SELECT * FROM comp^", &ctx);
    mu_assert("word wrong at end of input", strcmp(ctx.word, "comp") == 0);
    mu_assert("word_off wrong at end of input", ctx.word_off == 14u);

    analyze("SELECT * FROM ^", &ctx);
    mu_assert("no word means an empty prefix", ctx.word[0] == '\0');
    mu_assert("word_off should be the cursor when there is no word", ctx.word_off == 14u);

    analyze("SELECT c.na^ FROM companies c", &ctx);
    mu_assert("qualifier not picked up", strcmp(ctx.qualifier, "c") == 0);
    mu_assert("qualified word wrong", strcmp(ctx.word, "na") == 0);
    return NULL;
}

/* The FROM clause is usually typed after the select list is edited again, so
 * tables have to be collected from the whole statement, not just the part
 * before the cursor. */
static const char *test_context_tables(void)
{
    SqlContext ctx;

    analyze("SELECT ^ FROM companies", &ctx);
    mu_assert("table after the cursor was missed", ctx.ntables == 1u);
    mu_assert("table name wrong", strcmp(ctx.tables[0].name, "companies") == 0);
    mu_assert("no alias expected", ctx.tables[0].alias[0] == '\0');

    analyze("SELECT ^ FROM companies AS c", &ctx);
    mu_assert("AS alias missed", strcmp(ctx.tables[0].alias, "c") == 0);

    analyze("SELECT ^ FROM companies c", &ctx);
    mu_assert("bare alias missed", strcmp(ctx.tables[0].alias, "c") == 0);

    analyze("SELECT * FROM a x, b y WHERE ^", &ctx);
    mu_assert("comma join: wrong table count", ctx.ntables == 2u);
    mu_assert("comma join: second table wrong", strcmp(ctx.tables[1].name, "b") == 0);
    mu_assert("comma join: second alias wrong", strcmp(ctx.tables[1].alias, "y") == 0);

    analyze("SELECT * FROM a JOIN b ON a.id = b.id WHERE ^", &ctx);
    mu_assert("JOIN: wrong table count", ctx.ntables == 2u);
    mu_assert("JOIN: second table wrong", strcmp(ctx.tables[1].name, "b") == 0);

    analyze("SELECT * FROM main.companies WHERE ^", &ctx);
    mu_assert("schema qualifier should not become the table",
              strcmp(ctx.tables[0].name, "companies") == 0);
    mu_assert("schema-qualified table counted twice", ctx.ntables == 1u);

    analyze("SELECT * FROM (SELECT * FROM inner_t) WHERE ^", &ctx);
    mu_assert("subquery table missed", ctx.ntables == 1u);
    mu_assert("subquery table wrong", strcmp(ctx.tables[0].name, "inner_t") == 0);

    /* A statement before the cursor must not leak its tables in. */
    analyze("SELECT * FROM old; SELECT * FROM new WHERE ^", &ctx);
    mu_assert("tables leaked across a statement boundary", ctx.ntables == 1u);
    mu_assert("wrong statement's table", strcmp(ctx.tables[0].name, "new") == 0);
    return NULL;
}

static const char *test_context_value_column(void)
{
    SqlContext ctx;

    analyze("SELECT * FROM t WHERE country = ^", &ctx);
    mu_assert("value column wrong", strcmp(ctx.column, "country") == 0);

    analyze("SELECT * FROM t WHERE c.country IN (^", &ctx);
    mu_assert("qualified value column wrong", strcmp(ctx.column, "country") == 0);

    analyze("SELECT * FROM t WHERE a = 1 AND b LIKE ^", &ctx);
    mu_assert("value column should follow the latest comparison", strcmp(ctx.column, "b") == 0);

    analyze("UPDATE t SET size = ^", &ctx);
    mu_assert("SET value column wrong", strcmp(ctx.column, "size") == 0);
    return NULL;
}

static const char *test_context_dot(void)
{
    SqlContext ctx;

    analyze(".tab^", &ctx);
    mu_assert("dot command word wrong", strcmp(ctx.word, "tab") == 0);
    mu_assert("dot word_off should skip the dot", ctx.word_off == 1u);

    analyze(".schema comp^", &ctx);
    mu_assert("dot command name wrong", strcmp(ctx.dot_command, "schema") == 0);
    mu_assert("dot arg number wrong", ctx.dot_argno == 1u);
    mu_assert("dot arg word wrong", strcmp(ctx.word, "comp") == 0);

    analyze(".import file.csv ^", &ctx);
    mu_assert("second dot arg not counted", ctx.dot_argno == 2u);
    mu_assert("fresh dot arg should have an empty word", ctx.word[0] == '\0');

    analyze(".mode ^", &ctx);
    mu_assert("first empty dot arg wrong", ctx.dot_argno == 1u);
    return NULL;
}

/* --- fuzz --------------------------------------------------------------- */

/* Every offset of every corpus entry, plus pseudo-random byte strings. The
 * assertion is simply that this terminates and that ASan and UBSan stay quiet;
 * a completion engine that can be crashed by a half-typed line is useless. */
static const char *test_fuzz_offsets(void)
{
    static const char *const corpus[] = {
        "",
        ".",
        "..",
        "'",
        "\"",
        "`",
        "[",
        "/*",
        "--",
        "x'",
        "(((((((((((((((((((((((((",
        ")))",
        "SELECT * FROM t WHERE a = 'x' AND b IN (1, 2) ORDER BY c;",
        "INSERT INTO t (a, b) VALUES (1, 'two');",
        ("select c.name, count(*) from companies c join people p on p.cid = c.id"
         " group by c.name having count(*) > 1 limit 10 offset 5"),
        ".import --csv /tmp/x.csv tbl",
        "PRAGMA main.table_info('t');",
        "CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT DEFAULT 'x');",
        "1e",
        "0x",
        "a.b.c.d.e",
        ":p @q $r ?9 ?",
    };
    static const char alphabet[] = " \t\n'\"`[]()-/*.,;=<>?:@$abcxyzABCXYZ0123_";
    unsigned long seed = 20260916uL;
    size_t i;
    size_t iter;

    for (i = 0u; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
        size_t len = strlen(corpus[i]);
        size_t c;

        for (c = 0u; c <= len + 2u; c++) {
            SqlContext ctx;

            sql_context(corpus[i], c, &ctx);
            mu_assert("word must stay NUL-terminated", ctx.word[SQL_NAME_MAX - 1u] == '\0');
            mu_assert("ntables must stay in range", ctx.ntables <= SQL_CTX_MAX_TABLES);
        }
    }

    for (iter = 0u; iter < 3000u; iter++) {
        char buf[65];
        size_t len;
        size_t k;
        size_t c;

        seed = (seed * 6364136223846793005uL) + 1442695040888963407uL;
        len = (size_t)((seed >> 33) % (sizeof(buf) - 1u));
        for (k = 0u; k < len; k++) {
            seed = (seed * 6364136223846793005uL) + 1442695040888963407uL;
            buf[k] = alphabet[(seed >> 33) % (sizeof(alphabet) - 1u)];
        }
        buf[len] = '\0';
        for (c = 0u; c <= len; c++) {
            SqlContext ctx;

            sql_context(buf, c, &ctx);
            mu_assert("fuzz: ntables out of range", ctx.ntables <= SQL_CTX_MAX_TABLES);
        }
    }
    return NULL;
}

const char *sqlctx_suite(void)
{
    mu_run_test(test_lex_kinds);
    mu_run_test(test_lex_escapes);
    mu_run_test(test_lex_unterminated);
    mu_run_test(test_lex_sequence);
    mu_run_test(test_context_table);
    mu_run_test(test_context_opaque);
    mu_run_test(test_context_word);
    mu_run_test(test_context_tables);
    mu_run_test(test_context_value_column);
    mu_run_test(test_context_dot);
    mu_run_test(test_fuzz_offsets);
    return NULL;
}
