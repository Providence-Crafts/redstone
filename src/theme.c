#include "theme.h"

#include "plat.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Long enough for "0;1;3;4;7;38;2;255;255;255;48;2;255;255;255m" and the CSI
 * around it, with room to spare. A style that would not fit is rejected by the
 * parser rather than truncated. */
#define THEME_SGR_MAX 64u
#define THEME_LINE_MAX 256u

static char g_style[THEME_STYLE_COUNT][THEME_SGR_MAX];
static bool g_colour = false;
static bool g_ready = false;

static void set_builtin_default(void);

/* The palette is built on first use rather than by an init call, so that a
 * caller which only ever asks for one style -- a test, or a dot command run
 * before any drawing -- still gets the default rather than empty strings. */
static void ensure(void)
{
    if (!g_ready) {
        g_ready = true;
        set_builtin_default();
    }
}

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

    ensure();
    if (no_colour != NULL && no_colour[0] != '\0') {
        g_colour = false;
        return;
    }
    /* Windows consoles set no TERM yet understand VT; POSIX without TERM is
     * not a terminal we know. */
    if ((term == NULL && !plat_truecolor_default()) ||
        (term != NULL && strcmp(term, "dumb") == 0)) {
        g_colour = false;
        return;
    }
    g_colour = plat_isatty(out);
}

const char *theme_sgr(ThemeStyle style)
{
    ensure();
    if (!g_colour || (unsigned)style >= (unsigned)THEME_STYLE_COUNT) {
        return "";
    }
    return g_style[style];
}

/* --------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------ */

static bool ieq(const char *a, const char *b)
{
    size_t i;

    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

/* Append "NUM" to BUF, separated from what is there by a semicolon. False when
 * it would not fit, which is how an absurd value is rejected rather than
 * silently cut in half. */
static bool add_part(char *buf, size_t size, const char *part)
{
    size_t have = strlen(buf);
    size_t want = strlen(part) + (have > 0u ? 1u : 0u);

    if (have + want + 1u > size) {
        return false;
    }
    if (have > 0u) {
        buf[have] = ';';
        have++;
    }
    memcpy(buf + have, part, strlen(part) + 1u);
    return true;
}

static const char *const g_colour_names[] = {"black",   "red",  "green", "yellow", "blue",
                                             "magenta", "cyan", "white", NULL};

/* Turn one colour word into its SGR parameters. BASE is 30 for a foreground
 * and 40 for a background; BRIGHT shifts the eight named colours to the
 * high-intensity set. False when WORD is not a colour at all. */
static bool colour_part(const char *word, bool bright, int base, char *out, size_t size)
{
    size_t i;

    for (i = 0u; g_colour_names[i] != NULL; i++) {
        if (ieq(word, g_colour_names[i])) {
            int code = base + (int)i + (bright ? 60 : 0);

            return snprintf(out, size, "%d", code) > 0;
        }
    }
    if (ieq(word, "default")) {
        return snprintf(out, size, "%d", base + 9) > 0;
    }
    if (word[0] == '#' && strlen(word) == 7u) {
        char *end = NULL;
        unsigned long rgb = strtoul(word + 1, &end, 16);

        if (end == NULL || *end != '\0') {
            return false;
        }
        return snprintf(out, size, "%d;2;%lu;%lu;%lu", base + 8, (rgb >> 16) & 0xffu,
                        (rgb >> 8) & 0xffu, rgb & 0xffu) > 0;
    }
    if (isdigit((unsigned char)word[0])) {
        char *end = NULL;
        unsigned long n = strtoul(word, &end, 10);

        if (end == NULL || *end != '\0' || n > 255ul) {
            return false;
        }
        return snprintf(out, size, "%d;5;%lu", base + 8, n) > 0;
    }
    return false;
}

/* Parse one value -- "bold blue on 236" -- into the SGR body for a style.
 * Returns false, leaving OUT untouched, when any word is unusable: half a
 * theme line is worse than none, because the half that applied is invisible. */
static bool parse_value(const char *value, char *out, size_t size)
{
    char body[THEME_SGR_MAX];
    char part[24];
    char word[32];
    bool bright = false;
    bool background = false;
    size_t i = 0u;

    body[0] = '\0';
    while (value[i] != '\0') {
        size_t n = 0u;

        while (value[i] != '\0' && isspace((unsigned char)value[i])) {
            i++;
        }
        while (value[i] != '\0' && !isspace((unsigned char)value[i])) {
            if (n + 1u >= sizeof word) {
                return false;
            }
            word[n] = value[i];
            n++;
            i++;
        }
        if (n == 0u) {
            break;
        }
        word[n] = '\0';

        if (ieq(word, "none")) {
            out[0] = '\0';
            return true;
        }
        if (ieq(word, "bright")) {
            bright = true;
            continue;
        }
        if (ieq(word, "on")) {
            background = true;
            bright = false;
            continue;
        }
        if (ieq(word, "bold")) {
            strcpy(part, "1");
        } else if (ieq(word, "dim")) {
            strcpy(part, "2");
        } else if (ieq(word, "italic")) {
            strcpy(part, "3");
        } else if (ieq(word, "underline")) {
            strcpy(part, "4");
        } else if (ieq(word, "reverse")) {
            strcpy(part, "7");
        } else if (!colour_part(word, bright, background ? 40 : 30, part, sizeof part)) {
            return false;
        } else {
            background = false;
            bright = false;
        }
        if (!add_part(body, sizeof body, part)) {
            return false;
        }
    }
    if (body[0] == '\0') {
        out[0] = '\0';
        return true;
    }
    /* Every style begins with a reset, so that whatever the previous style
     * left on -- a bold, a background -- does not bleed into this one. */
    return snprintf(out, size, "\x1b[0;%sm", body) > 0;
}

/* Strip a trailing comment and the whitespace around a field, in place. */
static char *trim(char *s)
{
    size_t n;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    n = strlen(s);
    while (n > 0u && isspace((unsigned char)s[n - 1u])) {
        n--;
    }
    s[n] = '\0';
    return s;
}

static ThemeStyle style_for(const char *section, const char *key, bool *found)
{
    size_t i;

    *found = false;
    for (i = 1u; i < (size_t)THEME_STYLE_COUNT; i++) {
        if (ieq(theme_names[i].section, section) && ieq(theme_names[i].key, key)) {
            *found = true;
            return (ThemeStyle)i;
        }
    }
    return THEME_RESET;
}

/* One line of a theme file. SECTION is carried between calls. */
static bool parse_line(char *line, char *section, size_t seclen, const char *origin, int lineno,
                       FILE *err)
{
    char *eq;
    const char *key;
    const char *value;
    ThemeStyle style;
    bool found;

    {
        /* "#" comments only a whole line: inside a value it introduces a
         * #rrggbb colour, and a value is the one place a user writes one. An
         * end-of-line comment is spelled "--", as it is in SQL. */
        char *dash = strstr(line, "--");
        const char *first = line;

        /* The analyser calls a byte read from the file a tainted index into
         * ctype's table; the cast to unsigned char is what makes that lookup
         * defined, and there is nothing further to check.
         * NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
        while (*first != '\0' && isspace((unsigned char)*first)) {
            first++;
        }
        if (*first == '#') {
            return true;
        }
        if (dash != NULL) {
            *dash = '\0';
        }
    }
    line = trim(line);
    if (line[0] == '\0') {
        return true;
    }
    if (line[0] == '[') {
        char *close = strchr(line, ']');

        if (close == NULL) {
            if (err != NULL) {
                fprintf(err, "%s:%d: unterminated section header\n", origin, lineno);
            }
            return false;
        }
        *close = '\0';
        (void)snprintf(section, seclen, "%s", trim(line + 1));
        return true;
    }
    eq = strchr(line, '=');
    if (eq == NULL) {
        if (err != NULL) {
            fprintf(err, "%s:%d: expected \"key = value\"\n", origin, lineno);
        }
        return false;
    }
    *eq = '\0';
    key = trim(line);
    value = trim(eq + 1);
    style = style_for(section, key, &found);
    if (!found) {
        if (err != NULL) {
            fprintf(err, "%s:%d: unknown style [%s] %s\n", origin, lineno, section, key);
        }
        return false;
    }
    if (!parse_value(value, g_style[style], THEME_SGR_MAX)) {
        if (err != NULL) {
            fprintf(err, "%s:%d: bad value for [%s] %s: %s\n", origin, lineno, section, key, value);
        }
        return false;
    }
    return true;
}

/* The body of theme_apply, without the ensure(): the built-in default is
 * applied through this too, and calling ensure() from there would be a cycle
 * back into the very initialisation that is running. */
static bool apply_text(const char *text, const char *origin, FILE *err)
{
    char line[THEME_LINE_MAX];
    char section[32];
    const char *p = text;
    int lineno = 0;
    bool ok = true;

    section[0] = '\0';
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);

        lineno++;
        if (n + 1u > sizeof line) {
            if (err != NULL) {
                fprintf(err, "%s:%d: line too long\n", origin, lineno);
            }
            ok = false;
            n = sizeof line - 1u;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        if (!parse_line(line, section, sizeof section, origin, lineno, err)) {
            ok = false;
        }
        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return ok;
}

bool theme_apply(const char *text, const char *origin, FILE *err)
{
    ensure();
    return apply_text(text, origin, err);
}

static void set_builtin_default(void)
{
    size_t i;

    for (i = 0u; i < (size_t)THEME_STYLE_COUNT; i++) {
        g_style[i][0] = '\0';
    }
    (void)snprintf(g_style[THEME_RESET], THEME_SGR_MAX, "\x1b[0m");
    /* The built-in text is known good, so a failure here is a bug in this
     * file rather than in a user's theme; it is reported nowhere because
     * there is no stream to report it on at first use. */
    (void)apply_text(theme_builtins[0].text, "<default>", NULL);
}

void theme_reset(void)
{
    g_ready = true;
    set_builtin_default();
}

bool theme_load_file(const char *path, FILE *err)
{
    char line[THEME_LINE_MAX];
    char section[32];
    FILE *f = fopen(path, "r");
    int lineno = 0;
    bool ok = true;

    ensure();
    if (f == NULL) {
        return true; /* no theme file is the normal case, not a failure */
    }
    section[0] = '\0';
    while (fgets(line, (int)sizeof line, f) != NULL) {
        lineno++;
        if (!parse_line(line, section, sizeof section, path, lineno, err)) {
            ok = false;
        }
    }
    (void)fclose(f);
    return ok;
}

const char *theme_name_at(size_t i)
{
    size_t n = 0u;

    while (n < i && theme_builtins[n].name != NULL) {
        n++;
    }
    return theme_builtins[n].name;
}

bool theme_load(const char *name, FILE *err)
{
    size_t i;

    theme_reset();
    if (name == NULL || name[0] == '\0' || ieq(name, "default")) {
        return true;
    }
    for (i = 0u; theme_builtins[i].name != NULL; i++) {
        if (ieq(name, theme_builtins[i].name)) {
            return theme_apply(theme_builtins[i].text, theme_builtins[i].name, err);
        }
    }
    {
        FILE *f = fopen(name, "r");

        if (f == NULL) {
            if (err != NULL) {
                fprintf(err, "%s: no such theme: %s\n", THEME_PROGRAM, name);
            }
            return false;
        }
        (void)fclose(f);
    }
    return theme_load_file(name, err);
}

char *theme_path(void)
{
    char *dir = plat_config_dir();
    size_t n;
    char *path;

    if (dir == NULL) {
        return NULL;
    }
    n = strlen(dir) + sizeof("/" THEME_PROGRAM "/theme");
    path = malloc(n);
    if (path != NULL) {
        (void)snprintf(path, n, "%s/" THEME_PROGRAM "/theme", dir);
    }
    free(dir);
    return path;
}

/* Past the digits of a number, so that a multi-part colour is consumed whole
 * rather than leaving its parameters to be read as further styles. */
static const char *skip_number(const char *p)
{
    while (isdigit((unsigned char)*p)) {
        p++;
    }
    return p;
}

/* Turn an SGR body back into the words that produced it. Only the forms this
 * parser emits are recognised, which is all theme_dump ever sees. */
static void unparse(const char *sgr, char *out, size_t size)
{
    const char *p;
    size_t used = 0u;

    out[0] = '\0';
    if (sgr[0] == '\0') {
        (void)snprintf(out, size, "none");
        return;
    }
    p = strchr(sgr, '[');
    p = p != NULL ? p + 1 : sgr;
    while (*p != '\0' && *p != 'm') {
        char word[32];
        unsigned long n = strtoul(p, NULL, 10);

        word[0] = '\0';
        switch (n) {
        case 0ul:
            break;
        case 1ul:
            (void)snprintf(word, sizeof word, "bold");
            break;
        case 2ul:
            (void)snprintf(word, sizeof word, "dim");
            break;
        case 3ul:
            (void)snprintf(word, sizeof word, "italic");
            break;
        case 4ul:
            (void)snprintf(word, sizeof word, "underline");
            break;
        case 7ul:
            (void)snprintf(word, sizeof word, "reverse");
            break;
        default:
            /* A colour: "38;5;N", "48;5;N", "38;2;R;G;B", or one of the
             * sixteen direct codes. The background forms are prefixed with
             * "on", which is what parse_value expects back. */
            {
                const char *rest = strchr(p, ';');
                bool bg = n == 48ul || (n >= 40ul && n <= 49ul) || (n >= 100ul && n <= 107ul);
                const char *lead = bg ? "on " : "";

                if ((n == 38ul || n == 48ul) && rest != NULL) {
                    unsigned long kind = strtoul(rest + 1, NULL, 10);
                    const char *nums = strchr(rest + 1, ';');

                    if (kind == 5ul && nums != NULL) {
                        (void)snprintf(word, sizeof word, "%s%lu", lead,
                                       strtoul(nums + 1, NULL, 10));
                        p = skip_number(nums + 1);
                    } else if (kind == 2ul && nums != NULL) {
                        unsigned long r = strtoul(nums + 1, NULL, 10);
                        const char *g = strchr(nums + 1, ';');
                        const char *b = g != NULL ? strchr(g + 1, ';') : NULL;

                        if (b != NULL) {
                            (void)snprintf(word, sizeof word, "%s#%02lx%02lx%02lx", lead, r,
                                           strtoul(g + 1, NULL, 10), strtoul(b + 1, NULL, 10));
                            p = skip_number(b + 1);
                        }
                    }
                } else if (n == 39ul || n == 49ul) {
                    (void)snprintf(word, sizeof word, "%sdefault", lead);
                } else {
                    unsigned long base = bg ? 40ul : 30ul;
                    /* 39 and 49 are handled above, so anything left at 90 or
                     * over is one of the eight bright colours. */
                    const char *bright = "";

                    /* 39 and 49 are handled above, so anything left at 90 or
                     * over is one of the eight bright colours. */
                    if (n >= 90ul) {
                        base = bg ? 100ul : 90ul;
                        bright = "bright ";
                    }
                    if (n - base < 8ul) {
                        (void)snprintf(word, sizeof word, "%s%s%s", lead, bright,
                                       g_colour_names[n - base]);
                    }
                }
            }
            break;
        }
        if (word[0] != '\0' && used + strlen(word) + 2u < size) {
            if (used > 0u) {
                out[used] = ' ';
                used++;
            }
            memcpy(out + used, word, strlen(word) + 1u);
            used += strlen(word);
        }
        p = strchr(p, ';');
        if (p == NULL) {
            break;
        }
        p++;
    }
    if (out[0] == '\0') {
        (void)snprintf(out, size, "none");
    }
}

void theme_dump(FILE *out)
{
    const char *section = "";
    size_t i;

    ensure();
    for (i = 1u; i < (size_t)THEME_STYLE_COUNT; i++) {
        char words[96];

        if (!ieq(section, theme_names[i].section)) {
            section = theme_names[i].section;
            fprintf(out, "%s[%s]\n", i > 1u ? "\n" : "", section);
        }
        unparse(g_style[i], words, sizeof words);
        fprintf(out, "%-10s = %s\n", theme_names[i].key, words);
    }
}
