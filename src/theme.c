#include "theme.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 256-colour codes rather than the 8 basic ones: the basic set is remapped by
 * most terminal themes, which is how a "blue" comment ends up unreadable on a
 * blue background. These are chosen from the mid range, legible on light and
 * dark grounds alike. */
static const char *const g_sgr[THEME_STYLE_COUNT] = {
    "\x1b[0m",          /* RESET */
    "\x1b[7m",          /* SELECTED: reverse video, so it follows the terminal */
    "\x1b[1;38;5;39m",  /* MATCH */
    "\x1b[38;5;245m",   /* DETAIL */
    "\x1b[1;38;5;244m", /* GROUP */
    "\x1b[38;5;244m",   /* NOTE */
    "\x1b[38;5;75m",    /* TABLE */
    "\x1b[38;5;79m",    /* VIEW */
    "\x1b[38;5;223m",   /* COLUMN */
    "\x1b[38;5;180m",   /* VALUE */
    "\x1b[38;5;141m",   /* FUNCTION */
    "\x1b[38;5;108m",   /* PRAGMA */
    "\x1b[38;5;110m",   /* KEYWORD */
    "\x1b[38;5;215m"    /* DOT */
};

static bool g_colour = false;

void theme_set_colour(bool on)
{
    g_colour = on;
}

bool theme_colour(void)
{
    return g_colour;
}

void theme_detect(FILE *out)
{
    const char *no_colour = getenv("NO_COLOR");
    const char *term = getenv("TERM");
    int fd = out != NULL ? fileno(out) : -1;

    if (no_colour != NULL && no_colour[0] != '\0') {
        g_colour = false;
        return;
    }
    if (term == NULL || strcmp(term, "dumb") == 0) {
        g_colour = false;
        return;
    }
    g_colour = fd >= 0 && isatty(fd) == 1;
}

const char *theme_sgr(ThemeStyle style)
{
    if (!g_colour || (unsigned)style >= (unsigned)THEME_STYLE_COUNT) {
        return "";
    }
    return g_sgr[style];
}
