/*
 * Tests for the out module: result formatting, independent of db.c.
 *
 * db.c's own tests (test_db.c) still exercise a handful of these paths
 * end-to-end through db_exec, because that is what proves the two modules
 * are wired together correctly. Everything about what a given mode/option
 * combination actually produces belongs here instead, one call into out.h
 * at a time, so a formatting regression points straight at this file rather
 * than at a SELECT statement three layers up.
 */
#include "minunit.h"
#include "out.h"
#include "suites.h"

#include <stdlib.h>
#include <string.h>

/* Save NAME's current value (or NULL if unset) so a test that pokes the
 * environment can put it back afterwards -- getenv's buffer is not ours to
 * hold on to across a setenv, so it is copied rather than pointed at. */
static char *save_env(const char *name)
{
    const char *v = getenv(name);
    char *copy;
    size_t n;

    if (v == NULL) {
        return NULL;
    }
    n = strlen(v) + 1u;
    copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, v, n);
    }
    return copy;
}

static void restore_env(const char *name, char *saved)
{
    if (saved != NULL) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

/* Run one result set (NROWS rows of NCOL values each, row-major in ROWS)
 * through OUT and capture the bytes it writes, by way of a temporary file.
 * Simpler and more portable under C99 than open_memstream (see test_db.c,
 * which does the same for db_exec). Returns NULL on any failure. */
static char *capture(Out *out, int ncol, const char *const *names, const OutValue *rows,
                     size_t nrows)
{
    FILE *tmp = tmpfile();
    char *buf;
    long size;
    size_t r;

    if (tmp == NULL) {
        return NULL;
    }
    out_set_stream(out, tmp);
    if (!out_begin(out, ncol, names)) {
        fclose(tmp);
        return NULL;
    }
    for (r = 0u; r < nrows; r++) {
        if (!out_row(out, &rows[r * (size_t)ncol])) {
            break;
        }
    }
    out_end(out);
    fflush(tmp);

    if (fseek(tmp, 0L, SEEK_END) != 0) {
        fclose(tmp);
        return NULL;
    }
    size = ftell(tmp);
    if (size < 0 || fseek(tmp, 0L, SEEK_SET) != 0) {
        fclose(tmp);
        return NULL;
    }
    buf = malloc((size_t)size + 1u);
    if (buf != NULL) {
        size_t got = fread(buf, 1u, (size_t)size, tmp);
        buf[got] = '\0';
    }
    fclose(tmp);
    return buf;
}

/* A single call into out_command, for the option-parsing tests: they care
 * only about the return value and side effects, never about output. */
static bool command1(Out *out, const char *arg)
{
    const char *argv[1];

    argv[0] = arg;
    return out_command(out, 1, argv, NULL);
}

static bool command2(Out *out, const char *a, const char *b)
{
    const char *argv[2];

    argv[0] = a;
    argv[1] = b;
    return out_command(out, 2, argv, NULL);
}

/* --------------------------------------------------------------------------
 * One fixture shared by the classic-mode tests: two columns, two rows, no
 * NULLs or blobs -- those get their own test since several modes render
 * them specially.
 * ------------------------------------------------------------------------ */

static const char *const g_names[] = {"a", "b"};

static const OutValue g_rows[] = {
    {OUT_INT, "1", NULL, 0u},
    {OUT_TEXT, "x", NULL, 0u},
    {OUT_INT, "2", NULL, 0u},
    {OUT_TEXT, "y", NULL, 0u},
};

static const char *test_mode_list(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "list"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* list's own preset turns headers off; no --headers call needed. */
    mu_assert("list output wrong", got != NULL && strcmp(got, "1|x\n2|y\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_csv(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "csv"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* csv's row separator is CRLF, not a bare "\n" -- upstream's choice, and
     * one that a straight port from list mode would miss. */
    mu_assert("csv output wrong", got != NULL && strcmp(got, "1,x\r\n2,y\r\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_line(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "line"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* Every row repeats the column names; rows are separated by a blank
     * line, not by a header. */
    mu_assert("line output wrong", got != NULL && strcmp(got, "a: 1\nb: x\n\na: 2\nb: y\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_json(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "json"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("json output wrong",
              got != NULL && strcmp(got, "[{\"a\":1,\"b\":\"x\"},\n{\"a\":2,\"b\":\"y\"}]\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_insert(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "insert"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* No --tablename set: falls back to "tab", the same as upstream. One
     * statement per row, which is what sqlite3(1) 3.53.3 emits. */
    mu_assert("insert output wrong",
              got != NULL &&
                  strcmp(got, "INSERT INTO tab VALUES(1,'x');\nINSERT INTO tab VALUES(2,'y');\n") ==
                      0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_multiinsert(void)
{
    static const char *const argv[] = {"insert", "--tablename", "t", "--multiinsert", "3000"};
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("mode command failed", out_command(out, 5, argv, NULL));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("multiinsert output wrong",
              got != NULL && strcmp(got, "INSERT INTO t VALUES(1,'x'),(2,'y');\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_markdown(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "markdown"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("markdown output wrong",
              got != NULL && strcmp(got, "| a | b |\n|---|---|\n| 1 | x |\n| 2 | y |\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

/* box draws its Unicode border only when out->compat is set or the locale
 * looks like UTF-8 (see border_for in out.c); compat is the only one of
 * those a test can pin down, so it is used here purely to make the border
 * choice deterministic, not because the test is about --compat. */
static const char *test_mode_box(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    out_set_compat(out);
    mu_assert("bad mode name", out_set_mode(out, "box"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("box output wrong",
              got != NULL &&
                  strcmp(got, "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac"
                              "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae\n"
                              "\xe2\x94\x82 a \xe2\x94\x82 b \xe2\x94\x82\n"
                              "\xe2\x95\x9e\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xaa"
                              "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa1\n"
                              "\xe2\x94\x82 1 \xe2\x94\x82 x \xe2\x94\x82\n"
                              "\xe2\x94\x82 2 \xe2\x94\x82 y \xe2\x94\x82\n"
                              "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4"
                              "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_column(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "column"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* No border at all: two spaces between columns, a hyphen rule under the
     * headers, and trailing spaces trimmed (there is no right edge to keep
     * them square against). */
    mu_assert("column output wrong", got != NULL && strcmp(got, "a  b\n-  -\n1  x\n2  y\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_tabs(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "tabs"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("tabs output wrong", got != NULL && strcmp(got, "1\tx\n2\ty\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_html(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "html"));
    got = capture(out, 2, g_names, g_rows, 2u);
    /* Upstream never closes <TH>/<TD>: verified against sqlite3(1), not
     * assumed -- a "fixed" closing tag here would be a parity regression. */
    mu_assert("html output wrong", got != NULL && strcmp(got, "<TR>\n<TH>a\n<TH>b\n</TR>\n"
                                                              "<TR>\n<TD>1\n<TD>x\n</TR>\n"
                                                              "<TR>\n<TD>2\n<TD>y\n</TR>\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_mode_quote(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "quote"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("quote output wrong", got != NULL && strcmp(got, "1,'x'\n2,'y'\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Options that compose with any mode: headers, separators, NULL text.
 * ------------------------------------------------------------------------ */

static const char *test_headers_on_off(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "list"));

    out_set_headers(out, true);
    mu_assert("headers getter wrong", out_headers(out));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("headers-on output wrong", got != NULL && strcmp(got, "a|b\n1|x\n2|y\n") == 0);
    free(got);

    out_set_headers(out, false);
    mu_assert("headers getter wrong", !out_headers(out));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("headers-off output wrong", got != NULL && strcmp(got, "1|x\n2|y\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

static const char *test_custom_separator_and_null(void)
{
    Out *out = out_new(NULL);
    char *got;
    const OutValue rows[] = {
        {OUT_INT, "1", NULL, 0u},
        {OUT_NULL, NULL, NULL, 0u},
    };

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "list"));
    out_set_headers(out, false);
    out_set_colsep(out, " :: ");
    out_set_null_text(out, "N/A");
    mu_assert("colsep getter wrong", strcmp(out_colsep(out), " :: ") == 0);
    mu_assert("null text getter wrong", strcmp(out_null_text(out), "N/A") == 0);

    got = capture(out, 2, g_names, rows, 1u);
    mu_assert("custom separator/null output wrong", got != NULL && strcmp(got, "1 :: N/A\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

/* An empty result set must print nothing at all in the streaming styles --
 * not even a header row -- which is what sqlite3(1) does (verified against
 * the binary, see the comment by list_header in out.c). Buffered styles
 * (box here) short-circuit the same way in columnar_end. */
static const char *test_empty_result(void)
{
    static const char *const modes[] = {"list", "html", "json", "insert", "box"};
    size_t i;

    for (i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        Out *out = out_new(NULL);
        char *got;

        mu_assert("out_new failed", out != NULL);
        mu_assert("bad mode name", out_set_mode(out, modes[i]));
        out_set_headers(out, true); /* even with headers on, still nothing */
        got = capture(out, 2, g_names, NULL, 0u);
        mu_assert("empty result should print nothing", got != NULL && got[0] == '\0');
        free(got);
        out_free(out);
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Blob and NULL rendering.
 * ------------------------------------------------------------------------ */

static const char *test_blob_and_null(void)
{
    Out *out = out_new(NULL);
    char *got;
    static const unsigned char blob[] = {0x41u, 0x42u, 0x43u}; /* "ABC" */
    const OutValue rows_list[] = {
        {OUT_BLOB, NULL, blob, sizeof(blob)},
        {OUT_NULL, NULL, NULL, 0u},
    };
    const OutValue rows_insert[] = {
        {OUT_BLOB, NULL, blob, sizeof(blob)},
        {OUT_NULL, NULL, NULL, 0u},
    };

    mu_assert("out_new failed", out != NULL);

    /* list's "auto" blob quoting is BL_TEXT: the raw bytes, since the
     * surrounding text encoding is plain. */
    mu_assert("bad mode name", out_set_mode(out, "list"));
    out_set_headers(out, false);
    got = capture(out, 2, g_names, rows_list, 1u);
    mu_assert("list blob/null output wrong", got != NULL && strcmp(got, "ABC|\n") == 0);
    free(got);

    /* insert's "auto" blob quoting is BL_SQL (hex literal), because its text
     * encoding is SQL; its NULL text is "NULL", from the preset. */
    mu_assert("bad mode name", out_set_mode(out, "insert"));
    got = capture(out, 2, g_names, rows_insert, 1u);
    mu_assert("insert blob/null output wrong",
              got != NULL && strcmp(got, "INSERT INTO tab VALUES(x'414243',NULL);\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

/* --------------------------------------------------------------------------
 * A UTF-8/CJK value must not throw off box's border alignment: the layout
 * pass sizes columns in display cells (width_of), not bytes, so a wide
 * character earns exactly the padding a same-width run of latin letters
 * would.
 * ------------------------------------------------------------------------ */
static const char *test_box_cjk_alignment(void)
{
    Out *out = out_new(NULL);
    char *got;
    static const char *const names[] = {"s"};
    /* "日本語": three ideographs, two cells each, six cells total -- wider
     * than its one-cell header "s", so the column widens to fit the value. */
    const OutValue rows[] = {{OUT_TEXT, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", NULL, 0u}};

    mu_assert("out_new failed", out != NULL);
    out_set_compat(out); /* deterministic border, see test_mode_box */
    mu_assert("bad mode name", out_set_mode(out, "box"));
    got = capture(out, 1, names, rows, 1u);
    mu_assert("box/CJK alignment wrong",
              got != NULL &&
                  strcmp(got,
                         "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                         "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae\n"
                         "\xe2\x94\x82   s    \xe2\x94\x82\n"
                         "\xe2\x95\x9e\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"
                         "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa1\n"
                         "\xe2\x94\x82 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xe2\x94\x82\n"
                         "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
                         "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

/* --------------------------------------------------------------------------
 * `.mode` option parsing (out_command).
 * ------------------------------------------------------------------------ */

static const char *test_command_noquote_and_quote(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);

    /* --noquote drops quote mode's SQL text encoding back to plain. */
    mu_assert("bad mode name", out_set_mode(out, "quote"));
    mu_assert("--noquote rejected", command1(out, "--noquote"));
    got = capture(out, 1, g_names, (const OutValue[]){{OUT_TEXT, "x", NULL, 0u}}, 1u);
    mu_assert("--noquote had no effect", got != NULL && strcmp(got, "x\n") == 0);
    free(got);

    /* --quote json switches the text encoding of an otherwise plain mode. */
    mu_assert("bad mode name", out_set_mode(out, "list"));
    mu_assert("--quote json rejected", command2(out, "--quote", "json"));
    got = capture(out, 1, g_names, (const OutValue[]){{OUT_TEXT, "x", NULL, 0u}}, 1u);
    mu_assert("--quote json had no effect", got != NULL && strcmp(got, "\"x\"\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

static const char *test_command_escape(void)
{
    Out *out = out_new(NULL);
    char *got;
    const OutValue rows[] = {{OUT_TEXT,
                              "a\x01"
                              "b",
                              NULL, 0u}};

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "list"));

    /* Default escaping (ES_AUTO) renders a control byte as a caret escape. */
    got = capture(out, 1, g_names, rows, 1u);
    mu_assert("default escape wrong", got != NULL && strcmp(got, "a^Ab\n") == 0);
    free(got);

    /* --escape off passes the same byte through untouched. */
    mu_assert("--escape off rejected", command2(out, "--escape", "off"));
    got = capture(out, 1, g_names, rows, 1u);
    mu_assert("--escape off had no effect", got != NULL && strcmp(got, "a\x01"
                                                                       "b\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

static const char *test_command_wrap_and_wordwrap(void)
{
    Out *out = out_new(NULL);
    char *got;
    static const char *const names[] = {"v"};
    const OutValue abcde[] = {{OUT_TEXT, "abcde", NULL, 0u}};
    const OutValue ab_cd[] = {{OUT_TEXT, "ab cd", NULL, 0u}};

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "column"));

    /* --wrap 3 breaks mid-word wherever the column runs out of cells. */
    mu_assert("--wrap rejected", command2(out, "--wrap", "3"));
    got = capture(out, 1, names, abcde, 1u);
    mu_assert("--wrap output wrong", got != NULL && strcmp(got, " v\n---\nabc\nde\n") == 0);
    free(got);

    /* --wordwrap on backs a break up to the nearest space instead. */
    mu_assert("bad mode name", out_set_mode(out, "column"));
    mu_assert("--wrap rejected", command2(out, "--wrap", "3"));
    mu_assert("--wordwrap rejected", command2(out, "--wordwrap", "on"));
    got = capture(out, 1, names, ab_cd, 1u);
    mu_assert("--wordwrap output wrong", got != NULL && strcmp(got, " v\n---\nab\ncd\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

/* --------------------------------------------------------------------------
 * --screenwidth / --sw: shrinking columnar output to fit the terminal.
 * ------------------------------------------------------------------------ */

static size_t count_lines(const char *s)
{
    size_t n = 0u;

    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            n++;
        }
    }
    return n;
}

/* The width (in the rule line) of the COL-th column (0-based) of a
 * table-mode rule line "+---+-----+...+": the run of fill characters between
 * its opening and closing '+', i.e. the on-screen width of that column
 * counting whatever margin the rule was drawn with. */
static size_t rule_run(const char *line, int col)
{
    const char *p = line;
    int skip = col + 1;

    while (skip-- > 0) {
        p = strchr(p, '+');
        if (p == NULL) {
            return 0u; /* fewer columns than the caller expected */
        }
        p++;
    }
    return strcspn(p, "+");
}

static const char *test_command_screenwidth_values(void)
{
    Out *out = out_new(NULL);

    mu_assert("out_new failed", out != NULL);
    mu_assert("--screenwidth off rejected", command2(out, "--screenwidth", "off"));
    mu_assert("--screenwidth auto rejected", command2(out, "--screenwidth", "auto"));
    mu_assert("--screenwidth 40 rejected", command2(out, "--screenwidth", "40"));
    mu_assert("--sw 40 rejected", command2(out, "--sw", "40"));
    mu_assert("bad --screenwidth value should be rejected",
              !command2(out, "--screenwidth", "bogus"));

    out_free(out);
    return NULL;
}

static const char *test_screenwidth_wraps_wide_columns(void)
{
    Out *out = out_new(NULL);
    static const char *const names[] = {"a", "b"};
    const OutValue row[] = {{OUT_TEXT, "x", NULL, 0u},
                            {OUT_TEXT, "a value much too long to fit a narrow terminal", NULL, 0u}};
    char *got;
    size_t baseline;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "table"));

    /* No restriction: the query's one row is one physical line. */
    got = capture(out, 2, names, row, 1u);
    mu_assert("unrestricted capture failed", got != NULL);
    baseline = count_lines(got);
    free(got);

    /* Too narrow for the natural widths: column b wraps onto more lines,
     * which is the whole point -- the table stays readable instead of
     * running off the edge of the terminal. */
    mu_assert("--screenwidth rejected", command2(out, "--screenwidth", "20"));
    got = capture(out, 2, names, row, 1u);
    mu_assert("narrow capture failed", got != NULL);
    mu_assert("a narrow screen should wrap onto more lines", count_lines(got) > baseline);
    free(got);

    /* --screenwidth off is the escape hatch back to the unrestricted layout. */
    mu_assert("--screenwidth off rejected", command2(out, "--screenwidth", "off"));
    got = capture(out, 2, names, row, 1u);
    mu_assert("off capture failed", got != NULL);
    mu_assert("off should restore the unrestricted layout", count_lines(got) == baseline);
    free(got);

    out_free(out);
    return NULL;
}

/* A column pinned by --widths is exactly what the user asked for; shrinking
 * it further to satisfy --screenwidth would silently override that request,
 * so only the other, unpinned column may give up width. */
static const char *test_screenwidth_leaves_fixed_widths_alone(void)
{
    Out *out = out_new(NULL);
    static const char *const names[] = {"a", "b"};
    const OutValue row[] = {{OUT_TEXT, "0123456789012345678901234567890123456789", NULL, 0u},
                            {OUT_TEXT, "9876543210987654321098765432109876543210", NULL, 0u}};
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "table"));
    mu_assert("--widths rejected", command2(out, "--widths", "30,0"));
    mu_assert("--screenwidth rejected", command2(out, "--screenwidth", "25"));

    got = capture(out, 2, names, row, 1u);
    mu_assert("capture failed", got != NULL);
    /* The top rule is the first line; column a's run of "-" is its width. */
    mu_assert("the pinned column should keep its requested width", rule_run(got, 0) == 30u);
    mu_assert("the unpinned column should have given up width instead", rule_run(got, 1) < 30u);
    free(got);

    out_free(out);
    return NULL;
}

/* A bad value must be rejected, reported, and leave the formatter in a state
 * that still works -- not half-applied, not crashed. */
static const char *test_command_rejects_bad_value(void)
{
    Out *out = out_new(NULL);
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "list"));

    mu_assert("bad --quote value should be rejected", !command2(out, "--quote", "bogus"));
    mu_assert("bad --wordwrap value should be rejected", !command2(out, "--wordwrap", "maybe"));
    mu_assert("unknown option should be rejected", !command1(out, "--this-option-does-not-exist"));

    /* Still usable afterwards, with list's own defaults intact. */
    got = capture(out, 1, g_names, (const OutValue[]){{OUT_TEXT, "x", NULL, 0u}}, 1u);
    mu_assert("out unusable after a rejected option", got != NULL && strcmp(got, "x\n") == 0);
    free(got);

    out_free(out);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Locale-driven border fallback, colour suppression, and the pretty default.
 * ------------------------------------------------------------------------ */

/* box's border comes from utf8_locale() (out.c) whenever compat is off: a
 * non-UTF-8 LC_ALL/LC_CTYPE/LANG falls back to the ASCII "+"/"-"/"|" border
 * of g_table, a UTF-8 one draws the Unicode box-drawing border of g_box. All
 * three variables are neutralised for each case since utf8_locale() checks
 * them in that order and stops at the first one that is set. */
/* The body is separate so that the caller can restore the environment on
 * every exit, including the one an assertion takes. */
static const char *box_border_locale_body(void)
{
    Out *out;
    char *got;

    mu_assert("setenv LC_ALL failed", setenv("LC_ALL", "C", 1) == 0);
    mu_assert("setenv LC_CTYPE failed", setenv("LC_CTYPE", "C", 1) == 0);
    mu_assert("setenv LANG failed", setenv("LANG", "C", 1) == 0);
    out = out_new(NULL);
    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "box"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("ascii border expected in C locale", got != NULL && strcmp(got, "+---+---+\n"
                                                                              "| a | b |\n"
                                                                              "+---+---+\n"
                                                                              "| 1 | x |\n"
                                                                              "| 2 | y |\n"
                                                                              "+---+---+\n") == 0);
    free(got);
    out_free(out);

    mu_assert("setenv LC_ALL failed", setenv("LC_ALL", "en_US.UTF-8", 1) == 0);
    out = out_new(NULL);
    mu_assert("out_new failed", out != NULL);
    mu_assert("bad mode name", out_set_mode(out, "box"));
    got = capture(out, 2, g_names, g_rows, 2u);
    mu_assert("unicode border expected in utf8 locale",
              got != NULL &&
                  strcmp(got, "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac"
                              "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae\n"
                              "\xe2\x94\x82 a \xe2\x94\x82 b \xe2\x94\x82\n"
                              "\xe2\x95\x9e\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xaa"
                              "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\xa1\n"
                              "\xe2\x94\x82 1 \xe2\x94\x82 x \xe2\x94\x82\n"
                              "\xe2\x94\x82 2 \xe2\x94\x82 y \xe2\x94\x82\n"
                              "\xe2\x95\xb0\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xb4"
                              "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x95\xaf\n") == 0);
    free(got);
    out_free(out);
    return NULL;
}

static const char *test_box_border_locale_fallback(void)
{
    char *old_all = save_env("LC_ALL");
    char *old_ctype = save_env("LC_CTYPE");
    char *old_lang = save_env("LANG");
    const char *msg = box_border_locale_body();

    restore_env("LC_ALL", old_all);
    restore_env("LC_CTYPE", old_ctype);
    restore_env("LANG", old_lang);
    return msg;
}

/* Colour is opt-in (out_set_colour) and only ever touches cells, via sgr() in
 * out.c; with it off, no mode may emit an SGR escape ("\033["), no matter
 * what value is being rendered. box and list are checked, headers on, with
 * an int, a text value and a NULL -- exactly the storage classes the theme
 * assigns distinct colours to. */
static const char *test_no_colour_no_escapes(void)
{
    static const char *const modes[] = {"box", "list"};
    static const char *const names[] = {"a", "b", "c"};
    const OutValue rows[] = {
        {OUT_INT, "1", NULL, 0u},
        {OUT_TEXT, "x", NULL, 0u},
        {OUT_NULL, NULL, NULL, 0u},
    };
    size_t i;

    for (i = 0u; i < sizeof(modes) / sizeof(modes[0]); i++) {
        Out *out = out_new(NULL);
        char *got;

        mu_assert("out_new failed", out != NULL);
        mu_assert("bad mode name", out_set_mode(out, modes[i]));
        out_set_headers(out, true);
        out_set_colour(out, false);
        got = capture(out, 3, names, rows, 1u);
        mu_assert("output missing", got != NULL);
        mu_assert("SGR escape present with colour off", strstr(got, "\033[") == NULL);
        free(got);
        out_free(out);
    }
    return NULL;
}

/* out_new with nothing configured is redstone's own default, not upstream's:
 * box mode with headers on, not list mode with headers off. A regression to
 * upstream's defaults must fail this test. */
static const char *test_pretty_by_default(void)
{
    Out *out = out_new(NULL);

    mu_assert("out_new failed", out != NULL);
    mu_assert("default mode should be box", strcmp(out_mode_name(out), "box") == 0);
    mu_assert("default headers should be on", out_headers(out));
    out_free(out);
    return NULL;
}

/* box mode's default --linelimit (5) truncates a cell that wraps into more
 * rows than that, but the wrapped lines past the limit were never freed --
 * only ASan/LSan can see the leak, so the test just has to exercise the
 * path; a leak-checked run of the suite is what actually catches a
 * regression. */
static const char *test_linelimit_frees_truncated_lines(void)
{
    Out *out = out_new(NULL);
    static const char *const names[] = {"v"};
    const OutValue tall[] = {{OUT_TEXT, "one two three four five six seven", NULL, 0u}};
    char *got;

    mu_assert("out_new failed", out != NULL);
    mu_assert("--width rejected", command2(out, "--width", "3"));
    out_set_headers(out, false);
    got = capture(out, 1, names, tall, 1u);
    mu_assert("box output missing", got != NULL);
    /* At width 3 with no word-wrap, the 34-character cell splits into far
     * more than 5 lines; the default --linelimit (5) must clamp the
     * rendered rows to top + 5 + bottom = 7 lines. */
    mu_assert("linelimit did not clamp the wrapped cell to 5 lines", count_lines(got) == 7u);
    free(got);

    out_free(out);
    return NULL;
}

const char *out_suite(void)
{
    mu_run_test(test_mode_list);
    mu_run_test(test_mode_csv);
    mu_run_test(test_mode_line);
    mu_run_test(test_mode_json);
    mu_run_test(test_mode_insert);
    mu_run_test(test_mode_multiinsert);
    mu_run_test(test_mode_markdown);
    mu_run_test(test_mode_box);
    mu_run_test(test_mode_column);
    mu_run_test(test_mode_tabs);
    mu_run_test(test_mode_html);
    mu_run_test(test_mode_quote);
    mu_run_test(test_headers_on_off);
    mu_run_test(test_custom_separator_and_null);
    mu_run_test(test_empty_result);
    mu_run_test(test_blob_and_null);
    mu_run_test(test_box_cjk_alignment);
    mu_run_test(test_command_noquote_and_quote);
    mu_run_test(test_command_escape);
    mu_run_test(test_command_wrap_and_wordwrap);
    mu_run_test(test_command_screenwidth_values);
    mu_run_test(test_screenwidth_wraps_wide_columns);
    mu_run_test(test_screenwidth_leaves_fixed_widths_alone);
    mu_run_test(test_command_rejects_bad_value);
    mu_run_test(test_box_border_locale_fallback);
    mu_run_test(test_no_colour_no_escapes);
    mu_run_test(test_pretty_by_default);
    mu_run_test(test_linelimit_frees_truncated_lines);
    return NULL;
}
