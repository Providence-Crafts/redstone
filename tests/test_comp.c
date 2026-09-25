/*
 * Tests for schema introspection and candidate generation.
 *
 * These run against tests/test.db, whose shapes were chosen to break naive
 * completion: a column named with a reserved word, an identifier needing
 * quotes, a view, and a table whose name is a prefix of another. The fixture
 * is opened read-only; the few tests that need DDL build their own in-memory
 * database.
 */
#include "comp.h"
#include "minunit.h"
#include "suites.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FIXTURE_DB "tests/test.db"
#define CURSOR_MARK '^'

/* --- helpers ------------------------------------------------------------ */

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

static CompList *complete(Db *db, const char *marked, const CompDotSource *dots)
{
    char buf[256];
    size_t cursor = split_cursor(marked, buf, sizeof(buf));
    SqlContext ctx;

    sql_context(buf, cursor, &ctx);
    return comp_generate(db, &ctx, dots);
}

/* Index of DISPLAY in LIST, or -1. */
static long find_display(const CompList *list, const char *display)
{
    size_t i;

    for (i = 0u; i < comp_count(list); i++) {
        if (strcmp(comp_at(list, i)->display, display) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static bool has(const CompList *list, const char *display)
{
    return find_display(list, display) >= 0;
}

static const char *text_of(const CompList *list, const char *display)
{
    long i = find_display(list, display);

    return i < 0 ? NULL : comp_at(list, (size_t)i)->text;
}

static Db *open_fixture(void)
{
    return db_open(FIXTURE_DB, stderr);
}

static long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}

/* --- introspection ------------------------------------------------------ */

static const char *test_fixture_present(void)
{
    Db *db = open_fixture();
    const DbList *tables;
    bool ok;

    mu_assert("could not open " FIXTURE_DB " (run: make fixtures)", db != NULL);
    tables = db_tables(db);
    ok = db_list_count(tables) >= 5u;
    db_close(db);
    mu_assert(FIXTURE_DB " has no tables; regenerate it with `make fixtures`", ok);
    return NULL;
}

static const char *test_tables_and_views(void)
{
    Db *db = open_fixture();
    const DbList *tables;
    bool found_table = false;
    bool found_view = false;
    int master = 0;
    int schema = 0;
    size_t i;

    mu_assert("open failed", db != NULL);
    tables = db_tables(db);
    for (i = 0u; i < db_list_count(tables); i++) {
        const char *name = db_list_name(tables, i);

        if (strcmp(name, "employees") == 0 && strcmp(db_list_detail(tables, i), "table") == 0) {
            found_table = true;
        }
        if (strcmp(name, "active_employees") == 0 &&
            strcmp(db_list_detail(tables, i), "view") == 0) {
            found_view = true;
        }
        master += strcmp(name, "sqlite_master") == 0;
        schema += strcmp(name, "sqlite_schema") == 0;
    }
    db_close(db);
    mu_assert("employees missing from db_tables", found_table);
    mu_assert("the view is missing or mislabelled", found_view);
    /* Neither is a row of sqlite_master, so they are listed by hand; exactly
     * once, or a database that ever did describe them would offer them twice. */
    mu_assert("sqlite_master must be offered once", master == 1);
    mu_assert("sqlite_schema must be offered once", schema == 1);
    return NULL;
}

static const char *test_columns_and_types(void)
{
    Db *db = open_fixture();
    const DbList *cols;
    bool salary_is_real = false;
    bool has_quoted = false;
    size_t i;

    mu_assert("open failed", db != NULL);
    cols = db_columns(db, "employees");
    for (i = 0u; i < db_list_count(cols); i++) {
        if (strcmp(db_list_name(cols, i), "salary") == 0 &&
            strcmp(db_list_detail(cols, i), "REAL") == 0) {
            salary_is_real = true;
        }
    }
    cols = db_columns(db, "order_items");
    for (i = 0u; i < db_list_count(cols); i++) {
        if (strcmp(db_list_name(cols, i), "total amount") == 0) {
            has_quoted = true;
        }
    }
    mu_assert("declared type not reported", salary_is_real);
    mu_assert("the space-containing column is missing", has_quoted);

    cols = db_columns(db, "no_such_table");
    mu_assert("an unknown table should give an empty list, not NULL",
              cols != NULL && db_list_count(cols) == 0u);
    db_close(db);
    return NULL;
}

/* The three library-derived sources must agree with the library itself. A
 * hardcoded list would pass on the day it was written and quietly rot; these
 * assertions are what stop one creeping back in. */
static const char *test_library_sources_match(void)
{
    Db *db = open_fixture();
    sqlite3 *raw = NULL;
    sqlite3_stmt *stmt = NULL;
    size_t functions;
    size_t pragmas;
    int want_functions = 0;
    int want_pragmas = 0;
    bool ok;

    mu_assert("open failed", db != NULL);
    mu_assert("keyword count does not match the library",
              db_list_count(db_keywords(db)) == (size_t)sqlite3_keyword_count());

    functions = db_list_count(db_functions(db));
    pragmas = db_list_count(db_pragmas(db));
    db_close(db);

    mu_assert("no functions enumerated", functions > 0u);
    mu_assert("no pragmas enumerated", pragmas > 0u);

    mu_assert("raw open failed", sqlite3_open(":memory:", &raw) == SQLITE_OK);
    ok = sqlite3_prepare_v2(raw,
                            "SELECT (SELECT count(DISTINCT name) FROM pragma_function_list),"
                            "       (SELECT count(*) FROM pragma_pragma_list)",
                            -1, &stmt, NULL) == SQLITE_OK;
    if (ok && sqlite3_step(stmt) == SQLITE_ROW) {
        want_functions = sqlite3_column_int(stmt, 0);
        want_pragmas = sqlite3_column_int(stmt, 1);
    }
    (void)sqlite3_finalize(stmt);
    (void)sqlite3_close(raw);

    mu_assert("function count does not match pragma_function_list",
              functions == (size_t)want_functions);
    mu_assert("pragma count does not match pragma_pragma_list", pragmas == (size_t)want_pragmas);
    return NULL;
}

/* --- candidates --------------------------------------------------------- */

static const char *test_table_candidates(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool ok;
    long view;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT * FROM ^", NULL);
    ok = has(list, "employees") && has(list, "orders") && has(list, "order_items");
    view = find_display(list, "active_employees");
    ok = ok && view >= 0 && comp_at(list, (size_t)view)->kind == COMP_VIEW;
    comp_free(list);

    /* A table name that is a prefix of another must not hide it. */
    list = complete(db, "SELECT * FROM order^", NULL);
    ok = ok && comp_count(list) == 2u && has(list, "orders") && has(list, "order_items");
    comp_free(list);
    db_close(db);

    mu_assert("table candidates wrong", ok);
    return NULL;
}

/* The headline requirement: WHERE offers the columns of the tables in the
 * FROM clause, and only those. */
static const char *test_column_scoping(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool scoped;
    bool negative;
    bool joined;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT * FROM employees WHERE ^", NULL);
    scoped = has(list, "salary") && has(list, "status") && has(list, "manager_id");
    negative = !has(list, "sku") && !has(list, "building") && !has(list, "placed_on");
    comp_free(list);

    list = complete(db, "SELECT * FROM employees e JOIN departments d ON ^", NULL);
    joined = has(list, "salary") && has(list, "building") && has(list, "e") && has(list, "d");
    comp_free(list);

    /* With no FROM clause there is no scope, so there are no columns. */
    list = complete(db, "SELECT * FROM ^", NULL);
    negative = negative && !has(list, "salary");
    comp_free(list);
    db_close(db);

    mu_assert("columns of the FROM table are missing", scoped);
    mu_assert("a column of an out-of-scope table was offered", negative);
    mu_assert("a join should scope to both tables and offer their aliases", joined);
    return NULL;
}

static const char *test_qualified_scoping(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool ok;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT e.^ FROM employees e JOIN departments d ON d.id = e.dept_id", NULL);
    ok = has(list, "salary") && !has(list, "building");
    comp_free(list);

    list = complete(db, "SELECT departments.^ FROM departments", NULL);
    ok = ok && has(list, "building") && !has(list, "salary");
    comp_free(list);
    db_close(db);

    mu_assert("a qualifier should scope to exactly that table", ok);
    return NULL;
}

/* A column named with a reserved word, and one containing a space, must be
 * inserted quoted or the completed statement will not parse. */
static const char *test_identifier_quoting(void)
{
    Db *db = open_fixture();
    CompList *list;
    const char *reserved;
    const char *spaced;
    const char *plain;
    bool ok;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT ^ FROM orders", NULL);
    reserved = text_of(list, "order");
    plain = text_of(list, "status");
    ok = reserved != NULL && strcmp(reserved, "\"order\"") == 0;
    ok = ok && plain != NULL && strcmp(plain, "status") == 0;
    comp_free(list);

    list = complete(db, "SELECT ^ FROM order_items", NULL);
    spaced = text_of(list, "total amount");
    ok = ok && spaced != NULL && strcmp(spaced, "\"total amount\"") == 0;
    comp_free(list);
    db_close(db);

    mu_assert("identifiers needing quotes were not quoted", ok);
    return NULL;
}

static const char *test_prefix_filtering(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool exact;
    bool folded;
    bool empty;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT * FROM employees WHERE sal^", NULL);
    exact = comp_count(list) == 1u && has(list, "salary");
    comp_free(list);

    list = complete(db, "SELECT * FROM employees WHERE SAL^", NULL);
    folded = comp_count(list) == 1u && has(list, "salary");
    comp_free(list);

    list = complete(db, "SELECT * FROM zzz_no_such^", NULL);
    empty = comp_count(list) == 0u;
    comp_free(list);
    db_close(db);

    mu_assert("prefix filtering wrong", exact);
    mu_assert("prefix matching should ignore case", folded);
    mu_assert("an unmatched prefix should give nothing", empty);
    return NULL;
}

/* Matching ignores case; insertion follows the case the user started in, so
 * someone typing lowercase SQL stays in lowercase. */
static const char *test_keyword_case(void)
{
    Db *db = open_fixture();
    CompList *list;
    const char *lower_text;
    const char *upper_text;

    mu_assert("open failed", db != NULL);
    list = complete(db, "sel^", NULL);
    lower_text = text_of(list, "SELECT");
    mu_assert("lowercase prefix should complete to lowercase",
              lower_text != NULL && strcmp(lower_text, "select") == 0);
    comp_free(list);

    list = complete(db, "SEL^", NULL);
    upper_text = text_of(list, "SELECT");
    mu_assert("uppercase prefix should keep the library's spelling",
              upper_text != NULL && strcmp(upper_text, "SELECT") == 0);
    comp_free(list);
    db_close(db);
    return NULL;
}

static const char *test_value_completion(void)
{
    Db *db = open_fixture();
    CompList *list;
    const char *active;
    const char *dept;
    bool few;
    bool numeric;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT * FROM employees WHERE status = ^", NULL);
    active = text_of(list, "active");
    few = comp_count(list) == 3u && has(list, "on_leave") && has(list, "inactive");
    mu_assert("text values should be inserted quoted",
              active != NULL && strcmp(active, "'active'") == 0);
    comp_free(list);

    list = complete(db, "SELECT * FROM employees WHERE dept_id = ^", NULL);
    dept = text_of(list, "1");
    numeric = dept != NULL && strcmp(dept, "1") == 0 && comp_count(list) == 4u;
    comp_free(list);

    /* A prefix narrows values the same way it narrows names. */
    list = complete(db, "SELECT * FROM employees WHERE status = in^", NULL);
    mu_assert("value prefix filtering wrong", comp_count(list) == 1u && has(list, "inactive"));
    comp_free(list);

    /* An alias must resolve before the column can be looked up. */
    list = complete(db, "SELECT * FROM employees e WHERE e.status = ^", NULL);
    mu_assert("values behind an alias were not found", comp_count(list) == 3u);
    comp_free(list);
    db_close(db);

    mu_assert("distinct text values wrong", few);
    mu_assert("numeric values should be inserted bare", numeric);
    return NULL;
}

/* Value completion is the only thing here that reads user data, so it is the
 * only thing that could hang a keystroke. The deadline is what makes it safe
 * to leave on by default, and this asserts on elapsed time because asserting
 * on the result alone would pass even if it took a minute. */
static const char *test_value_time_limit(void)
{
    Db *db = db_open(NULL, stderr);
    CompList *list;
    long started;
    long elapsed;
    bool ok;

    mu_assert("in-memory open failed", db != NULL);
    ok = db_exec(db,
                 "CREATE TABLE big (v TEXT);"
                 "INSERT INTO big(v) SELECT 'row-' || x FROM ("
                 "  WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 400000)"
                 "  SELECT x FROM c);",
                 stdout, stderr);
    mu_assert("could not build the large table", ok);

    started = monotonic_ms();
    list = complete(db, "SELECT * FROM big WHERE v = ^", NULL);
    elapsed = monotonic_ms() - started;

    mu_assert("the deadline should have cut the value query short", comp_truncated(list));
    mu_assert("value completion blocked for too long", elapsed < 2000L);
    comp_free(list);
    db_close(db);
    return NULL;
}

/* DDL moves the schema cookie, and the caches have to notice. Completing a
 * column that was added a second ago is the whole reason the check exists. */
static const char *test_cache_invalidation(void)
{
    Db *db = db_open(NULL, stderr);
    CompList *list;
    int cookie;
    bool before;
    bool after;

    mu_assert("in-memory open failed", db != NULL);
    mu_assert("setup failed", db_exec(db, "CREATE TABLE t (a INTEGER);", stdout, stderr));
    cookie = db_schema_version(db);

    list = complete(db, "SELECT * FROM t WHERE ^", NULL);
    before = has(list, "a") && !has(list, "b");
    comp_free(list);

    mu_assert("alter failed", db_exec(db, "ALTER TABLE t ADD COLUMN b TEXT;", stdout, stderr));
    mu_assert("DDL should move the schema cookie", db_schema_version(db) != cookie);

    list = complete(db, "SELECT * FROM t WHERE ^", NULL);
    after = has(list, "a") && has(list, "b");
    comp_free(list);

    /* A new table must show up in the table list too. */
    mu_assert("create failed", db_exec(db, "CREATE TABLE later (x);", stdout, stderr));
    list = complete(db, "SELECT * FROM ^", NULL);
    after = after && has(list, "later");
    comp_free(list);
    db_close(db);

    mu_assert("the column cache was wrong before the DDL", before);
    mu_assert("the caches were not invalidated by DDL", after);
    return NULL;
}

static const char *test_other_contexts(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool pragmas;
    bool functions;
    bool nothing;

    mu_assert("open failed", db != NULL);
    list = complete(db, "PRAGMA ^", NULL);
    pragmas = has(list, "table_info") && !has(list, "employees");
    comp_free(list);

    list = complete(db, "SELECT cou^ FROM employees", NULL);
    functions = has(list, "count");
    comp_free(list);

    /* Inside a string literal there is nothing honest to offer. */
    list = complete(db, "SELECT * FROM employees WHERE status = 'act^'", NULL);
    nothing = comp_count(list) == 0u;
    comp_free(list);
    db_close(db);

    mu_assert("pragma candidates wrong", pragmas);
    mu_assert("functions should be offered in a select list", functions);
    mu_assert("a cursor inside a string should offer nothing", nothing);
    return NULL;
}

/* "*" is offered right where SELECT expects a column, alongside real columns
 * and functions, and only there -- it means nothing anywhere else. */
static const char *test_star_candidate(void)
{
    Db *db = open_fixture();
    CompList *list;
    bool offered;
    bool scoped;
    bool elsewhere;

    mu_assert("open failed", db != NULL);
    list = complete(db, "SELECT ^ FROM employees", NULL);
    offered =
        has(list, "*") && strcmp(text_of(list, "*"), "*") == 0 && find_display(list, "*") == 0;
    comp_free(list);

    /* A prefix that isn't "*" must not drag it in. */
    list = complete(db, "SELECT sal^ FROM employees", NULL);
    scoped = !has(list, "*");
    comp_free(list);

    list = complete(db, "SELECT * FROM employees WHERE ^", NULL);
    elsewhere = !has(list, "*");
    comp_free(list);
    db_close(db);

    mu_assert("'*' should be offered in a select list", offered);
    mu_assert("'*' should be filtered out by an unrelated prefix", scoped);
    mu_assert("'*' should not be offered outside a select list", elsewhere);
    return NULL;
}

/* --- the dot-command hook ----------------------------------------------- */

static const char *const g_fake_commands[] = {"schema", "tables", "mode"};
static const char *const g_fake_modes[] = {"box", "csv", "json"};

static bool fake_command(size_t i, const char **name, const char **help)
{
    if (i >= sizeof(g_fake_commands) / sizeof(g_fake_commands[0])) {
        return false;
    }
    *name = g_fake_commands[i];
    *help = "a command";
    return true;
}

static CompKind fake_arg_kind(const char *name, size_t argno, const char *const **words,
                              size_t *nwords)
{
    if (strcmp(name, "mode") == 0 && argno == 1u) {
        *words = g_fake_modes;
        *nwords = sizeof(g_fake_modes) / sizeof(g_fake_modes[0]);
        return COMP_KEYWORD;
    }
    if (strcmp(name, "schema") == 0 && argno == 1u) {
        return COMP_TABLE;
    }
    return COMP_KEYWORD;
}

static const char *test_dot_source(void)
{
    static const CompDotSource dots = {fake_command, fake_arg_kind};
    Db *db = open_fixture();
    CompList *list;
    bool commands;
    bool tables;
    bool words;
    bool absent;

    mu_assert("open failed", db != NULL);
    list = complete(db, ".^", NULL);
    absent = comp_count(list) == 0u; /* no source registered: no candidates */
    comp_free(list);

    list = complete(db, ".^", &dots);
    commands = comp_count(list) == 3u && has(list, "schema");
    comp_free(list);

    list = complete(db, ".sch^", &dots);
    commands = commands && comp_count(list) == 1u && has(list, "schema");
    comp_free(list);

    list = complete(db, ".schema emp^", &dots);
    tables = comp_count(list) == 1u && has(list, "employees");
    comp_free(list);

    list = complete(db, ".mode ^", &dots);
    words = comp_count(list) == 3u && has(list, "csv") && !has(list, "employees");
    comp_free(list);
    db_close(db);

    mu_assert("with no dot source there should be no dot candidates", absent);
    mu_assert("dot command candidates wrong", commands);
    mu_assert("a table-taking dot argument should offer tables", tables);
    mu_assert("a fixed word list should win over the library's keywords", words);
    return NULL;
}

/* At the very start of a line, before any dot has been typed, a dot command
 * is still offered (alongside SQL keywords) -- but accepting it must insert
 * the leading dot too. Without one, the inserted text is neither a dot
 * command nor valid SQL. */
static const char *test_dot_at_statement_start(void)
{
    static const CompDotSource dots = {fake_command, fake_arg_kind};
    Db *db = open_fixture();
    CompList *list;
    bool offered;
    bool dotted;

    mu_assert("open failed", db != NULL);
    list = complete(db, "^", &dots);
    offered = has(list, ".schema");
    dotted = offered && strcmp(text_of(list, ".schema"), ".schema") == 0;
    comp_free(list);
    db_close(db);

    mu_assert("a dot command should be offered at the start of a line", offered);
    mu_assert("its insertion text must carry the leading dot", dotted);
    return NULL;
}

const char *comp_suite(void)
{
    mu_run_test(test_fixture_present);
    mu_run_test(test_tables_and_views);
    mu_run_test(test_columns_and_types);
    mu_run_test(test_library_sources_match);
    mu_run_test(test_table_candidates);
    mu_run_test(test_column_scoping);
    mu_run_test(test_qualified_scoping);
    mu_run_test(test_identifier_quoting);
    mu_run_test(test_prefix_filtering);
    mu_run_test(test_keyword_case);
    mu_run_test(test_value_completion);
    mu_run_test(test_value_time_limit);
    mu_run_test(test_cache_invalidation);
    mu_run_test(test_other_contexts);
    mu_run_test(test_star_candidate);
    mu_run_test(test_dot_source);
    mu_run_test(test_dot_at_statement_start);
    return NULL;
}
