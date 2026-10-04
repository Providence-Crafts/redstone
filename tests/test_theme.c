/*
 * Tests for theme.c: the style table and the theme file.
 *
 * The palette is process-global, so every test that changes it puts it back:
 * a leaked style would otherwise reach the menu and output suites as a
 * mysterious difference rather than as a failure here.
 */
#include "minunit.h"
#include "suites.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

/* Read everything written to a fresh stream, NUL-terminated. The caller
 * frees; NULL on any failure, which every test treats as a failure. */
static char *capture(void (*emit)(FILE *), long *nbyte)
{
    FILE *tmp = tmpfile();
    char *buf;
    long size;

    if (tmp == NULL) {
        return NULL;
    }
    emit(tmp);
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
    if (fread(buf, 1u, (size_t)size, tmp) != (size_t)size) {
        free(buf);
        (void)fclose(tmp);
        return NULL;
    }
    buf[size] = '\0';
    (void)fclose(tmp);
    if (nbyte != NULL) {
        *nbyte = size;
    }
    return buf;
}

static char *dump_to_string(void)
{
    return capture(theme_dump, NULL);
}

static const char *test_colour_switch(void)
{
    theme_reset();
    theme_set_colour(false);
    mu_assert("colour off must yield no escape at all", theme_sgr(THEME_KEYWORD)[0] == '\0');
    theme_set_colour(true);
    mu_assert("the default palette must define the keyword style",
              theme_sgr(THEME_KEYWORD)[0] == '\x1b');
    mu_assert("reset is always the plain SGR reset",
              strcmp(theme_sgr(THEME_RESET), "\x1b[0m") == 0);
    mu_assert("an out-of-range style must not read past the table",
              theme_sgr((ThemeStyle)THEME_STYLE_COUNT)[0] == '\0');
    theme_set_colour(false);
    return NULL;
}

static const char *test_apply_values(void)
{
    theme_reset();
    theme_set_colour(true);
    mu_assert("a valid file must apply cleanly", theme_apply("[syntax]\n"
                                                             "keyword = bold red\n"
                                                             "comment = dim\n"
                                                             "error = underline #ff0080 on 52\n"
                                                             "operator = none\n"
                                                             "[output]\n"
                                                             "string = 151\n",
                                                             "<test>", NULL));
    /* Every style starts with a reset so that a bold or a background left on
     * by the previous style cannot bleed into this one. */
    mu_assert("named colour wrong", strcmp(theme_sgr(THEME_KEYWORD), "\x1b[0;1;31m") == 0);
    mu_assert("attribute-only style wrong", strcmp(theme_sgr(THEME_COMMENT), "\x1b[0;2m") == 0);
    mu_assert("palette index wrong", strcmp(theme_sgr(THEME_STRING), "\x1b[0;38;5;151m") == 0);
    mu_assert("truecolour and background wrong",
              strcmp(theme_sgr(THEME_ERROR), "\x1b[0;4;38;2;255;0;128;48;5;52m") == 0);
    mu_assert("\"none\" must mean no escape", theme_sgr(THEME_OPERATOR)[0] == '\0');
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

static const char *test_bright_and_case(void)
{
    theme_reset();
    theme_set_colour(true);
    mu_assert(
        "bright and mixed case must both be accepted",
        theme_apply("[SYNTAX]\nKeyword = BRIGHT Blue\nnumber = bright yellow\n", "<test>", NULL));
    mu_assert("bright foreground wrong", strcmp(theme_sgr(THEME_KEYWORD), "\x1b[0;94m") == 0);
    mu_assert("bright yellow wrong", strcmp(theme_sgr(THEME_NUMBER), "\x1b[0;93m") == 0);
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

static const char *test_bad_input_is_diagnosed(void)
{
    FILE *err = tmpfile();
    char before[64];
    char report[512];
    long n;

    theme_reset();
    theme_set_colour(true);
    (void)snprintf(before, sizeof before, "%s", theme_sgr(THEME_KEYWORD));
    mu_assert("tmpfile", err != NULL);
    mu_assert("a file with bad lines must report failure",
              !theme_apply("[nosuch]\nkeyword = red\n"
                           "[syntax]\nnosuchkey = red\n"
                           "number = chartreuse\n"
                           "table\n",
                           "theme", err));
    /* The point of the whole design: a typo costs the user that one line, not
     * the palette and not the shell. */
    mu_assert("a rejected file must leave the rest of the palette alone",
              strcmp(theme_sgr(THEME_KEYWORD), before) == 0);
    n = ftell(err);
    mu_assert("every bad line must be reported", n > 0);
    (void)fseek(err, 0L, SEEK_SET);
    report[fread(report, 1u, sizeof report - 1u, err)] = '\0';
    (void)fclose(err);
    mu_assert("the section is named", strstr(report, "nosuch") != NULL);
    mu_assert("the key is named", strstr(report, "nosuchkey") != NULL);
    mu_assert("the value is named", strstr(report, "chartreuse") != NULL);
    mu_assert("a line with no \"=\" is reported", strstr(report, "theme:6:") != NULL);
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

static const char *test_comments_and_blanks(void)
{
    theme_reset();
    theme_set_colour(true);
    mu_assert("comments and blank lines must be ignored, not rejected",
              theme_apply("# a comment\n"
                          "\n"
                          "[syntax]   -- trailing SQL-style comment\n"
                          "keyword = red   -- why not\n",
                          "<test>", NULL));
    mu_assert("the value before the comment must still apply",
              strcmp(theme_sgr(THEME_KEYWORD), "\x1b[0;31m") == 0);
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

/* The dump is not a listing that happens to look like a file: it is a file.
 * If this round-trip ever breaks, ".theme > ~/.config/redstone/theme" -- the
 * documented way to start editing a theme -- silently produces a broken one. */
static const char *test_dump_round_trips(void)
{
    size_t i;

    theme_set_colour(true);
    for (i = 0u; theme_name_at(i) != NULL; i++) {
        char *first;
        char *second;
        bool same;

        if (!theme_load(theme_name_at(i), NULL)) {
            theme_set_colour(false);
            theme_reset();
            return "a built-in theme failed to load";
        }
        first = dump_to_string();
        if (first == NULL || !theme_apply(first, "<dump>", NULL)) {
            free(first);
            theme_set_colour(false);
            theme_reset();
            return "a dumped palette was not accepted back";
        }
        second = dump_to_string();
        same = second != NULL && strcmp(first, second) == 0;
        free(first);
        free(second);
        if (!same) {
            theme_set_colour(false);
            theme_reset();
            return "a palette changed when its own dump was applied to it";
        }
    }
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

static const char *test_load_by_name(void)
{
    char dark[64];

    theme_set_colour(true);
    mu_assert("the dark theme must load", theme_load("dark", NULL));
    (void)snprintf(dark, sizeof dark, "%s", theme_sgr(THEME_KEYWORD));
    mu_assert("the themes must actually differ", theme_load("light", NULL));
    mu_assert("light and dark must not be the same palette",
              strcmp(dark, theme_sgr(THEME_KEYWORD)) != 0);
    /* Loading starts from the default every time, so a theme that leaves a
     * style unset gets the default's rather than the last theme's. */
    mu_assert("an unknown name is an error, not a silent no-op",
              !theme_load("no-such-theme", NULL));
    theme_set_colour(false);
    theme_reset();
    return NULL;
}

static const char *test_theme_path(void)
{
    char *path;

    mu_assert("setenv", setenv("XDG_CONFIG_HOME", "/tmp/xdg", 1) == 0);
    path = theme_path();
    mu_assert("XDG_CONFIG_HOME must be honoured",
              path != NULL && strcmp(path, "/tmp/xdg/redstone/theme") == 0);
    free(path);
    mu_assert("unsetenv", unsetenv("XDG_CONFIG_HOME") == 0);
    mu_assert("setenv", setenv("HOME", "/tmp/home", 1) == 0);
    path = theme_path();
    mu_assert("HOME must be the fallback",
              path != NULL && strcmp(path, "/tmp/home/.config/redstone/theme") == 0);
    free(path);
    return NULL;
}

static const char *test_missing_file_is_not_an_error(void)
{
    theme_reset();
    mu_assert("a missing theme file is the normal case",
              theme_load_file("/nonexistent/redstone/theme", NULL));
    return NULL;
}

const char *theme_suite(void)
{
    mu_run_test(test_colour_switch);
    mu_run_test(test_apply_values);
    mu_run_test(test_bright_and_case);
    mu_run_test(test_bad_input_is_diagnosed);
    mu_run_test(test_comments_and_blanks);
    mu_run_test(test_dump_round_trips);
    mu_run_test(test_load_by_name);
    mu_run_test(test_theme_path);
    mu_run_test(test_missing_file_is_not_an_error);
    return NULL;
}
