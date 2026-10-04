/*
 * Tests for brand.c: the banner and the prompt.
 *
 * The banner's behaviour depends on four things outside it -- the theme's
 * colour switch, the locale, COLORTERM and the terminal width -- so each test
 * sets all four it cares about and the last one restores them.
 */
#include "brand.h"
#include "line.h"
#include "minunit.h"
#include "plat.h"
#include "suites.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

#define BUF_SIZE 32768u

/* Run the banner into a buffer. Returns the byte count, 0 on failure. */
static size_t capture_banner(unsigned cols, char *buf)
{
    FILE *tmp = tmpfile();
    size_t n;

    if (tmp == NULL) {
        return 0u;
    }
    brand_banner(tmp, cols, "9.9.9");
    n = (size_t)ftell(tmp);
    if (n >= BUF_SIZE || fseek(tmp, 0L, SEEK_SET) != 0) {
        (void)fclose(tmp);
        return 0u;
    }
    n = fread(buf, 1u, n, tmp);
    buf[n] = '\0';
    (void)fclose(tmp);
    return n;
}

static size_t count_char(const char *s, char c)
{
    size_t n = 0u;

    for (; *s != '\0'; s++) {
        n += *s == c ? 1u : 0u;
    }
    return n;
}

static void set_env(bool utf8, const char *colorterm, bool colour)
{
    (void)plat_setenv("LC_ALL", utf8 ? "en_US.UTF-8" : "C");
    if (colorterm != NULL) {
        (void)plat_setenv("COLORTERM", colorterm);
    } else {
        (void)plat_unsetenv("COLORTERM");
    }
    theme_reset();
    theme_set_colour(colour);
}

static const char *test_prompt_width_ignores_escapes(void)
{
    mu_assert("plain text is one cell per character", line_prompt_width("abc> ") == 5u);
    mu_assert("an SGR sequence takes no cells",
              line_prompt_width("\x1b[1;38;5;160mab\x1b[0m") == 2u);
    /* U+25CF is one cell and three bytes: counting bytes would put the cursor
     * two columns too far right. */
    mu_assert("a three-byte single-cell glyph is one cell",
              line_prompt_width("\xe2\x97\x8f") == 1u);
    return NULL;
}

static const char *test_prompt_width_matches_brand_prompt(void)
{
    char buf[160];

    set_env(true, NULL, true);
    brand_prompt(buf, sizeof(buf));
    mu_assert("the branded prompt carries colour", strchr(buf, '\x1b') != NULL);
    mu_assert("'* redstone > ' is thirteen cells with or without colour",
              line_prompt_width(buf) == 13u);
    set_env(true, NULL, false);
    brand_prompt(buf, sizeof(buf));
    mu_assert("colour off must leave no escape in the prompt", strchr(buf, '\x1b') == NULL);
    mu_assert("colour off keeps the same width", line_prompt_width(buf) == 13u);
    set_env(false, NULL, false);
    brand_prompt(buf, sizeof(buf));
    mu_assert("a non-UTF-8 locale gets a plain ASCII prompt", strcmp(buf, "redstone> ") == 0);
    return NULL;
}

static const char *test_prompt_truncates(void)
{
    char buf[8];

    set_env(true, NULL, true);
    brand_prompt(buf, sizeof(buf));
    mu_assert("a short buffer is still terminated", memchr(buf, '\0', sizeof(buf)) != NULL);
    return NULL;
}

static const char *test_banner_without_colour_has_no_escapes(void)
{
    char buf[BUF_SIZE];

    set_env(true, "truecolor", false);
    mu_assert("banner should write", capture_banner(200u, buf) > 0u);
    mu_assert("no colour, no escape", strchr(buf, '\x1b') == NULL);
    mu_assert("no colour, no art", strstr(buf, "\xe2\x96\x80") == NULL);
    mu_assert("the name is still there", strstr(buf, "redstone") != NULL);
    mu_assert("so is the version", strstr(buf, "9.9.9") != NULL);
    return NULL;
}

static const char *test_banner_art_needs_truecolor_utf8_and_room(void)
{
    char buf[BUF_SIZE];

    set_env(true, "truecolor", true);
    mu_assert("banner should write", capture_banner(120u, buf) > 0u);
    mu_assert("truecolor art is drawn with 24-bit backgrounds", strstr(buf, "\x1b[48;2;") != NULL);
    mu_assert("the art is thirteen rows plus a blank line either side",
              count_char(buf, '\n') == 15u);
    mu_assert("the spaced name sits beside the art", strstr(buf, "R E D S T O N E") != NULL);

    mu_assert("banner should write", capture_banner(60u, buf) > 0u);
    mu_assert("too narrow for the captions: no art", strstr(buf, "\x1b[48;2;") == NULL);

    set_env(true, NULL, true);
    mu_assert("banner should write", capture_banner(120u, buf) > 0u);
#ifdef _WIN32
    /* Windows 10 consoles always draw 24-bit colour and plat_init switches
     * them to UTF-8, so neither COLORTERM nor the locale gates the art. */
    mu_assert("Windows: the art needs no COLORTERM", strstr(buf, "\x1b[48;2;") != NULL);
#else
    mu_assert("no COLORTERM: 24-bit codes might print as garbage, so no art",
              strstr(buf, "\x1b[48;2;") == NULL);

    set_env(false, "truecolor", true);
    mu_assert("banner should write", capture_banner(120u, buf) > 0u);
    mu_assert("not UTF-8: half blocks would not render, so no art",
              strstr(buf, "\xe2\x96\x80") == NULL);
#endif
    return NULL;
}

static const char *test_brand_theme_slots_round_trip(void)
{
    set_env(true, NULL, true);
    mu_assert("a theme file can restyle the prompt",
              theme_apply("[brand]\nglyph = #00ff00\nname = bold\n", "test", NULL));
    mu_assert("the glyph took the new colour",
              strstr(theme_sgr(THEME_BRAND_GLYPH), "0;255;0") != NULL);
    theme_reset();
    (void)plat_unsetenv("LC_ALL");
    (void)plat_unsetenv("COLORTERM");
    theme_set_colour(false);
    return NULL;
}

const char *brand_suite(void)
{
    mu_run_test(test_prompt_width_ignores_escapes);
    mu_run_test(test_prompt_width_matches_brand_prompt);
    mu_run_test(test_prompt_truncates);
    mu_run_test(test_banner_without_colour_has_no_escapes);
    mu_run_test(test_banner_art_needs_truecolor_utf8_and_room);
    mu_run_test(test_brand_theme_slots_round_trip);
    return NULL;
}
