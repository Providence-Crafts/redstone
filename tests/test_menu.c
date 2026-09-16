/*
 * Tests for the menu's layout and selection.
 *
 * menu.c does no I/O, so everything here is "give it candidates and a size,
 * read the bytes back". The candidates come from the real generator against
 * tests/test.db rather than from handmade lists, so the layout is exercised on
 * the widths and group mixes it will actually meet. Cursor placement is the
 * one thing that needs a terminal, and that is asserted in test_line.c.
 */
#include "menu.h"
#include "minunit.h"
#include "suites.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define FIXTURE_DB "tests/test.db"

static CompList *candidates(Db *db, const char *text)
{
    SqlContext ctx;

    sql_context(text, strlen(text), &ctx);
    return comp_generate(db, &ctx, NULL);
}

/* True when S contains a Select Graphic Rendition sequence, i.e. a CSI ending
 * in 'm'. Cursor and erase sequences are not colour and must survive NO_COLOR. */
static bool has_sgr(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] == '\x1b' && s[i + 1u] == '[') {
            size_t j = i + 2u;

            while (s[j] != '\0' && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                j++;
            }
            if (s[j] == 'm') {
                return true;
            }
        }
    }
    return false;
}

static unsigned count_rows(const char *s)
{
    unsigned rows = 0u;
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] == '\n') {
            rows++;
        }
    }
    return rows;
}

/* Longest run of visible cells on any line, to check nothing overflows the
 * terminal. Escape sequences occupy no cells. */
static size_t widest_line(const char *s)
{
    size_t widest = 0u;
    size_t w = 0u;
    size_t i = 0u;

    while (s[i] != '\0') {
        if (s[i] == '\x1b' && s[i + 1u] == '[') {
            i += 2u;
            while (s[i] != '\0' && (s[i] < '@' || s[i] > '~')) {
                i++;
            }
            if (s[i] != '\0') {
                i++;
            }
            continue;
        }
        if (s[i] == '\n' || s[i] == '\r') {
            if (w > widest) {
                widest = w;
            }
            w = 0u;
        } else if (((unsigned char)s[i] & 0xc0u) != 0x80u) {
            w++;
        }
        i++;
    }
    return w > widest ? w : widest;
}

static const char *test_display_width(void)
{
    mu_assert("ascii width wrong", menu_display_width("select") == 6u);
    mu_assert("utf-8 width should count characters", menu_display_width("caf\xc3\xa9") == 4u);
    mu_assert("empty width wrong", menu_display_width("") == 0u);
    mu_assert("NULL width wrong", menu_display_width(NULL) == 0u);
    return NULL;
}

static const char *test_empty_stays_closed(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    unsigned rows = 99u;

    mu_assert("setup failed", m != NULL && db != NULL);
    menu_open(m, NULL);
    mu_assert("a NULL list should leave the menu closed", !menu_active(m));
    menu_open(m, candidates(db, "SELECT * FROM zzz_nothing"));
    mu_assert("an empty list should leave the menu closed", !menu_active(m));
    mu_assert("a closed menu should render nothing", strcmp(menu_render(m, &rows), "") == 0);
    mu_assert("a closed menu should occupy no rows", rows == 0u);
    menu_free(m);
    db_close(db);
    return NULL;
}

static const char *test_selection_wraps(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    CompList *list;
    size_t n;
    bool first;
    bool second;
    bool wrapped;
    bool back;

    mu_assert("setup failed", m != NULL && db != NULL);
    list = candidates(db, "SELECT * FROM ");
    n = comp_count(list);
    menu_open(m, list);
    mu_assert("expected several tables", n > 2u);

    first = menu_selected_index(m) == 0u;
    menu_move(m, MENU_NEXT);
    second = menu_selected_index(m) == 1u;
    menu_move(m, MENU_PREV);
    menu_move(m, MENU_PREV);
    wrapped = menu_selected_index(m) == n - 1u;
    menu_move(m, MENU_NEXT);
    back = menu_selected_index(m) == 0u;
    menu_free(m);
    db_close(db);

    mu_assert("the first candidate should be selected on open", first);
    mu_assert("next should advance", second);
    mu_assert("prev past the start should wrap to the end", wrapped);
    mu_assert("next past the end should wrap to the start", back);
    return NULL;
}

/* Down and up move a whole grid row, which is only distinguishable from
 * next/prev when the grid really has several columns. */
static const char *test_row_movement(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    unsigned rows = 0u;
    size_t after_down;
    bool moved_row;
    bool returned;

    mu_assert("setup failed", m != NULL && db != NULL);
    menu_open(m, candidates(db, "SELECT * FROM employees WHERE "));
    menu_set_size(m, 200u, 10u);
    (void)menu_render(m, &rows);

    menu_move(m, MENU_DOWN);
    after_down = menu_selected_index(m);
    moved_row = after_down > 1u;
    menu_move(m, MENU_UP);
    returned = menu_selected_index(m) == 0u;
    menu_free(m);
    db_close(db);

    mu_assert("down should skip a whole row, not one cell", moved_row);
    mu_assert("up should come back to where down started", returned);
    return NULL;
}

static const char *test_render_contents(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    const char *text;
    unsigned rows = 0u;
    bool names;
    bool detail;

    mu_assert("setup failed", m != NULL && db != NULL);
    theme_set_colour(false);
    menu_open(m, candidates(db, "SELECT * FROM "));
    menu_set_size(m, 100u, 10u);
    text = menu_render(m, &rows);

    names = strstr(text, "employees") != NULL && strstr(text, "orders") != NULL;
    detail = strstr(text, "view") != NULL; /* the kind of active_employees */
    mu_assert("every row should be preceded by a newline", rows == count_rows(text));
    mu_assert("candidate names missing from the render", names);
    mu_assert("the description column is missing", detail);
    menu_free(m);
    db_close(db);
    return NULL;
}

/* The menu must never be wider than the terminal: a wrapped row would push the
 * prompt off the line the cursor is about to be placed on. */
static const char *test_fits_the_terminal(void)
{
    static const unsigned widths[] = {20u, 40u, 80u, 132u};
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    size_t i;
    bool ok = true;

    mu_assert("setup failed", m != NULL && db != NULL);
    theme_set_colour(true);
    menu_open(m, candidates(db, "SELECT * FROM employees WHERE "));
    for (i = 0u; i < sizeof(widths) / sizeof(widths[0]); i++) {
        unsigned rows = 0u;

        menu_set_size(m, widths[i], 8u);
        ok = ok && widest_line(menu_render(m, &rows)) <= widths[i];
    }
    theme_set_colour(false);
    menu_free(m);
    db_close(db);
    mu_assert("the menu overflowed the terminal width", ok);
    return NULL;
}

static const char *test_scrolling(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    CompList *list;
    const char *text;
    unsigned rows = 0u;
    size_t n;
    size_t i;
    bool bounded;
    bool shows_selection;
    bool notes_more;

    mu_assert("setup failed", m != NULL && db != NULL);
    theme_set_colour(false);
    /* One narrow column and few rows, so the keyword list cannot possibly fit. */
    list = candidates(db, "SELECT * FROM employees ");
    n = comp_count(list);
    menu_open(m, list);
    menu_set_size(m, 20u, 4u);
    text = menu_render(m, &rows);
    bounded = rows <= 4u;
    notes_more = strstr(text, "more") != NULL;

    for (i = 0u; i + 1u < n; i++) {
        menu_move(m, MENU_NEXT);
    }
    text = menu_render(m, &rows);
    shows_selection = rows <= 4u && strstr(text, comp_at(menu_list(m), n - 1u)->display) != NULL;
    menu_free(m);
    db_close(db);

    mu_assert("expected more candidates than rows", n > 8u);
    mu_assert("the menu exceeded the rows it was given", bounded);
    mu_assert("a scrolled menu should say how many are hidden", notes_more);
    mu_assert("the selected candidate scrolled out of view", shows_selection);
    return NULL;
}

static const char *test_colour_is_optional(void)
{
    Menu *m = menu_new();
    Db *db = db_open(FIXTURE_DB, stderr);
    unsigned rows = 0u;
    bool plain;
    bool coloured;

    mu_assert("setup failed", m != NULL && db != NULL);
    menu_open(m, candidates(db, "SELECT * FROM "));
    menu_set_size(m, 80u, 10u);

    theme_set_colour(false);
    plain = !has_sgr(menu_render(m, &rows));
    theme_set_colour(true);
    coloured = has_sgr(menu_render(m, &rows));
    theme_set_colour(false);
    menu_free(m);
    db_close(db);

    mu_assert("colour disabled should emit no SGR sequences", plain);
    mu_assert("colour enabled should emit SGR sequences", coloured);
    return NULL;
}

/* The environment rules, checked without a terminal: theme_detect looks at a
 * stream, and a temporary file is never one, so every case below must be off.
 * The positive case needs a pty and is covered in test_line.c. */
static const char *test_theme_detection(void)
{
    FILE *tmp = tmpfile();

    mu_assert("tmpfile failed", tmp != NULL);
    theme_set_colour(true);
    theme_detect(tmp);
    mu_assert("colour should be off when the stream is not a terminal", !theme_colour());
    theme_detect(NULL);
    mu_assert("colour should be off without a stream", !theme_colour());
    (void)fclose(tmp);
    return NULL;
}

const char *menu_suite(void)
{
    mu_run_test(test_display_width);
    mu_run_test(test_empty_stays_closed);
    mu_run_test(test_selection_wraps);
    mu_run_test(test_row_movement);
    mu_run_test(test_render_contents);
    mu_run_test(test_fits_the_terminal);
    mu_run_test(test_scrolling);
    mu_run_test(test_colour_is_optional);
    mu_run_test(test_theme_detection);
    return NULL;
}
