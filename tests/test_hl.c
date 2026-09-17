/*
 * Tests for hl.c: colouring the line being typed.
 *
 * The schema is a stub rather than a database, which is the point of the
 * HlSchema indirection: every colouring decision is testable without a
 * connection, and the tests say exactly which lookup produced which colour.
 */
#include "hl.h"
#include "minunit.h"
#include "suites.h"
#include "theme.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Case-insensitive, as every real callback in main.c is: the line carries the
 * user's spelling, and "SELECT" must colour like "select". */
static bool named(const char *want, const char *name, size_t len)
{
    size_t i;

    for (i = 0u; i < len && want[i] != '\0'; i++) {
        if (tolower((unsigned char)want[i]) != tolower((unsigned char)name[i])) {
            return false;
        }
    }
    return i == len && want[i] == '\0';
}

static bool stub_keyword(void *ctx, const char *name, size_t len)
{
    static const char *const words[] = {"select", "from", "where", "and", NULL};
    size_t i;

    (void)ctx;
    for (i = 0u; words[i] != NULL; i++) {
        if (named(words[i], name, len)) {
            return true;
        }
    }
    return false;
}

static bool stub_function(void *ctx, const char *name, size_t len)
{
    (void)ctx;
    return named("count", name, len);
}

static bool stub_table(void *ctx, const char *name, size_t len, bool *is_view)
{
    (void)ctx;
    *is_view = named("v", name, len);
    return named("users", name, len) || named("v", name, len);
}

static bool stub_column(void *ctx, const char *name, size_t len, const SqlCtxTable *tables,
                        size_t ntables)
{
    (void)ctx;
    /* Only in scope when the statement actually names the table, which is
     * what the tables argument is for. */
    return ntables > 0u && strcmp(tables[0].name, "users") == 0 &&
           (named("id", name, len) || named("name", name, len));
}

static const HlSchema g_schema = {stub_keyword, stub_function, stub_table, stub_column, NULL};

/* Highlight TEXT whole and return the bytes written. The caller frees. */
static char *render_window(const char *text, size_t from, size_t to, const HlSchema *schema)
{
    FILE *tmp = tmpfile();
    char *buf;
    long size;

    if (tmp == NULL) {
        return NULL;
    }
    hl_write(tmp, text, from, to, schema);
    size = ftell(tmp);
    if (size < 0 || fseek(tmp, 0L, SEEK_SET) != 0) {
        (void)fclose(tmp);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        (void)fclose(tmp);
        return NULL;
    }
    buf[fread(buf, 1u, (size_t)size, tmp)] = '\0';
    (void)fclose(tmp);
    return buf;
}

static char *render(const char *text)
{
    return render_window(text, 0u, strlen(text), &g_schema);
}

/* True when WORD appears in OUT with no style applied to it. Every styled span
 * ends its SGR with "m" immediately before the text, and the default palette
 * deliberately leaves "identifier" unset, so an unknown name is recognised by
 * the absence of that "m" -- which is exactly the property the user sees. */
static bool unstyled(const char *out, const char *word)
{
    const char *p;

    for (p = strstr(out, word); p != NULL; p = strstr(p + 1, word)) {
        if (p == out || p[-1] != 'm') {
            return true;
        }
    }
    return false;
}

/* True when OUT contains WORD wrapped in STYLE and the reset. */
static bool styled(const char *out, ThemeStyle style, const char *word)
{
    char want[128];

    (void)snprintf(want, sizeof want, "%s%s%s", theme_sgr(style), word, theme_sgr(THEME_RESET));
    return strstr(out, want) != NULL;
}

static const char *test_token_kinds(void)
{
    char *out;
    bool ok;

    theme_reset();
    theme_set_colour(true);
    out = render("SELECT count(id) FROM users WHERE name = 'x' -- note\n");
    mu_assert("render", out != NULL);
    ok = styled(out, THEME_KEYWORD, "SELECT") && styled(out, THEME_FUNCTION, "count") &&
         styled(out, THEME_COLUMN, "id") && styled(out, THEME_KEYWORD, "FROM") &&
         styled(out, THEME_TABLE, "users") && styled(out, THEME_COLUMN, "name") &&
         styled(out, THEME_OPERATOR, "=") && styled(out, THEME_STRING, "'x'") &&
         styled(out, THEME_COMMENT, "-- note");
    free(out);
    theme_set_colour(false);
    mu_assert("each token kind must get its own style", ok);
    return NULL;
}

static const char *test_unknown_names_and_views(void)
{
    char *out;
    bool ok;

    theme_set_colour(true);
    out = render("SELECT nosuch FROM v, nosuchtable");
    mu_assert("render", out != NULL);
    /* The whole point of schema awareness: a name the database does not know
     * is visibly not a name the database knows. */
    ok = unstyled(out, "nosuch") && styled(out, THEME_VIEW, "v") && unstyled(out, "nosuchtable");
    free(out);
    theme_set_colour(false);
    mu_assert("unknown names must be plain and a view must differ from a table", ok);
    return NULL;
}

static const char *test_columns_need_a_table_in_scope(void)
{
    char *out;
    bool ok;

    theme_set_colour(true);
    /* "id" is a column of users, but nothing here says users. */
    out = render("SELECT id");
    mu_assert("render", out != NULL);
    ok = unstyled(out, "id");
    free(out);
    theme_set_colour(false);
    mu_assert("a column out of scope must not be coloured as one", ok);
    return NULL;
}

static const char *test_errors(void)
{
    char *unclosed;
    char *paren;
    char *stray;
    bool ok;

    theme_set_colour(true);
    unclosed = render("SELECT 'abc");
    paren = render("SELECT count(id");
    stray = render("SELECT 1)");
    mu_assert("render", unclosed != NULL && paren != NULL && stray != NULL);
    ok = styled(unclosed, THEME_ERROR, "'abc") && styled(paren, THEME_ERROR, "(") &&
         styled(stray, THEME_ERROR, ")");
    free(unclosed);
    free(paren);
    free(stray);
    theme_set_colour(false);
    mu_assert("an unclosed quote or paren must be shown in the error colour", ok);
    return NULL;
}

static const char *test_balanced_parens_are_not_errors(void)
{
    char *out;
    bool ok;

    theme_set_colour(true);
    out = render("SELECT count((id))");
    mu_assert("render", out != NULL);
    ok = !styled(out, THEME_ERROR, "(") && styled(out, THEME_OPERATOR, "(");
    free(out);
    theme_set_colour(false);
    mu_assert("balanced parentheses must be ordinary punctuation", ok);
    return NULL;
}

static const char *test_dot_line(void)
{
    char *out;
    bool ok;

    theme_set_colour(true);
    out = render(".mode box");
    mu_assert("render", out != NULL);
    /* A dot command is not SQL: "box" must not come back coloured as a name,
     * and ".mode" must not be lexed as punctuation plus a word. */
    ok = styled(out, THEME_DOT, ".mode") && unstyled(out, "box");
    free(out);
    theme_set_colour(false);
    mu_assert("a dot command must colour only its command word", ok);
    return NULL;
}

static const char *test_colour_off_is_byte_for_byte(void)
{
    static const char sql[] = "SELECT count(id) FROM users;";
    char *out;
    bool same;

    theme_set_colour(false);
    out = render(sql);
    mu_assert("render", out != NULL);
    same = strcmp(out, sql) == 0;
    free(out);
    mu_assert("with colour off the line must be written unchanged", same);
    return NULL;
}

static const char *test_window_clipping(void)
{
    static const char sql[] = "SELECT id FROM users";
    char *out;
    bool ok;

    theme_set_colour(false);
    /* The window is a horizontal scroll of one line: only the visible bytes
     * are written, but the analysis still sees the whole line. */
    out = render_window(sql, 7u, 9u, &g_schema);
    mu_assert("render", out != NULL);
    ok = strcmp(out, "id") == 0;
    free(out);
    mu_assert("only the window may be written", ok);

    theme_set_colour(true);
    out = render_window(sql, 7u, 9u, &g_schema);
    mu_assert("render", out != NULL);
    ok = styled(out, THEME_COLUMN, "id");
    free(out);
    theme_set_colour(false);
    mu_assert("a token in the window keeps the style its whole line gives it", ok);
    return NULL;
}

static const char *test_no_schema_still_lexes(void)
{
    char *out;
    bool ok;

    theme_set_colour(true);
    out = render_window("SELECT 'x' FROM t", 0u, 17u, NULL);
    mu_assert("render", out != NULL);
    ok = styled(out, THEME_STRING, "'x'") && !styled(out, THEME_KEYWORD, "SELECT");
    free(out);
    theme_set_colour(false);
    mu_assert("without a schema the lexical colours must still apply", ok);
    return NULL;
}

/* Highlighting runs on every keystroke, so a long line must not make the
 * terminal wait. The budget is deliberately loose -- it is a guard against an
 * accidental quadratic, not a benchmark. */
static const char *test_long_line_is_fast(void)
{
    char line[4096];
    size_t i;
    clock_t start;
    double ms;

    for (i = 0u; i + 24u < sizeof line; i += 24u) {
        memcpy(line + i, "SELECT id FROM users; ", 22u);
        line[i + 22u] = 'x';
        line[i + 23u] = ' ';
    }
    line[i] = '\0';
    theme_set_colour(true);
    start = clock();
    for (i = 0u; i < 20u; i++) {
        char *out = render_window(line, 0u, 200u, &g_schema);

        if (out == NULL) {
            theme_set_colour(false);
            return "render";
        }
        free(out);
    }
    ms = ((double)(clock() - start) * 1000.0) / (double)CLOCKS_PER_SEC / 20.0;
    theme_set_colour(false);
    mu_assert("one redraw of a 4KB line must stay well under a frame", ms < 20.0);
    return NULL;
}

const char *hl_suite(void)
{
    mu_run_test(test_token_kinds);
    mu_run_test(test_unknown_names_and_views);
    mu_run_test(test_columns_need_a_table_in_scope);
    mu_run_test(test_errors);
    mu_run_test(test_balanced_parens_are_not_errors);
    mu_run_test(test_dot_line);
    mu_run_test(test_colour_off_is_byte_for_byte);
    mu_run_test(test_window_clipping);
    mu_run_test(test_no_schema_still_lexes);
    mu_run_test(test_long_line_is_fast);
    return NULL;
}
