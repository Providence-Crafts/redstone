#include "brand.h"

#include "out.h"
#include "plat.h"
#include "theme.h"

#include <stdlib.h>
#include <string.h>

/* clang-format off */
/* 24 x 26 pixels, 61 colours. '.' is transparent; the other characters index
 * g_palette in the order 0-9, A-Z, a-z. */
#define BRAND_ART_W 24
#define BRAND_ART_H 26

static const unsigned char g_palette[61][3] = {
    {191, 160, 160},
    {205, 13, 13},
    {86, 86, 86},
    {226, 85, 85},
    {113, 22, 22},
    {155, 104, 104},
    {165, 52, 52},
    {103, 129, 129},
    {255, 52, 52},
    {119, 64, 64},
    {151, 6, 6},
    {143, 138, 138},
    {72, 60, 60},
    {184, 81, 81},
    {118, 97, 97},
    {143, 78, 78},
    {221, 50, 50},
    {184, 118, 118},
    {239, 107, 107},
    {194, 39, 39},
    {137, 42, 42},
    {101, 48, 48},
    {122, 123, 123},
    {156, 28, 28},
    {103, 78, 78},
    {175, 145, 145},
    {185, 17, 17},
    {74, 76, 76},
    {101, 101, 101},
    {93, 9, 9},
    {198, 59, 59},
    {78, 42, 42},
    {91, 70, 70},
    {177, 99, 99},
    {128, 20, 20},
    {144, 91, 91},
    {131, 131, 131},
    {164, 127, 127},
    {136, 4, 4},
    {112, 111, 111},
    {171, 66, 66},
    {144, 61, 61},
    {238, 78, 78},
    {220, 106, 106},
    {131, 111, 111},
    {198, 89, 89},
    {178, 27, 27},
    {90, 32, 32},
    {189, 47, 47},
    {94, 94, 94},
    {112, 34, 34},
    {117, 85, 85},
    {133, 29, 29},
    {114, 47, 47},
    {81, 80, 81},
    {97, 57, 57},
    {132, 68, 68},
    {187, 65, 65},
    {151, 114, 114},
    {72, 69, 69},
    {130, 52, 52},
};

static const char *const g_art[BRAND_ART_H] = {
    "...........aB...........",
    ".........MaMMBB.........",
    ".......dBaiFSSMaM.......",
    ".....EUDEMw1QQKEaad.....",
    "...MaMvDFMMHI8GKSMaMd...",
    ".aBaiud7MaaaBBBZidMMMaa.",
    "MMMabgG6pdaBFfS7imZMMMdn",
    "SSddaPPX9dM7bgJ6iwwaSsRR",
    "S2nnnSMaMMweb0h3jdSsxxss",
    "nn2nnSndaaBX5MaMSsRsssRR",
    "SS9Wt2nnndMBaMd2RssRRRR2",
    "SSycc42n22SddnsRxVCCR4r2",
    "2ndN1mrsSnSn2RRxTY4tsr2s",
    "O92EXU6tnnnnsRxlAkp2RRtW",
    "OkK2SiiSnWnnssoc6F2RVY6O",
    "2yuSnnSSnNuSssW9p2W4q69s",
    "SSSnSdEtsnpS22ssx2OWZE2s",
    "SnW22SSKYLnSRxVs2n2s2OsR",
    "dSrNLsndDmmOWNrssxs2oKRn",
    "SndvJq2niXvEtcORCVxsOnnn",
    ".SSibpsSOidnxRRVYoxR2ns.",
    "...SddnnK6SnsslkFn2n2...",
    ".....ndSdE2n2sWFEs2.....",
    ".......dddSSns2ns.......",
    ".........Sdd2ss.........",
    "...........S2...........",
};
/* clang-format on */

#define BRAND_NAME "redstone"
#define BRAND_TAGLINE "SQLite shell with zsh-style completion"

/* Columns between the art and the captions, and the widest caption line:
 * "Pure C99 • sqlite3 drop-in • v" plus a version of up to a dozen characters. */
#define BRAND_GAP 3u
#define BRAND_CAPTION_W 45u

/* The caption rows, counted in text rows from the top of the art: the middle
 * three of thirteen, as librarian puts its caption on the middle of its art. */
#define BRAND_CAPTION_ROW 5u

static const char g_index[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static bool truecolor(void)
{
    const char *v = getenv("COLORTERM");

    return plat_truecolor_default() ||
           (v != NULL && (strstr(v, "truecolor") != NULL || strstr(v, "24bit") != NULL));
}

/* The palette entry for pixel (X, Y), or NULL where it is transparent. Y may be
 * one past the last row, because the art has an odd number of half-block rows
 * to pair up. */
static const unsigned char *pixel(unsigned x, unsigned y)
{
    const char *at;

    if (y >= BRAND_ART_H || g_art[y][x] == '.') {
        return NULL;
    }
    at = strchr(g_index, g_art[y][x]);
    return at != NULL ? g_palette[at - g_index] : NULL;
}

/* One text row of the art: two pixel rows folded into half blocks, upper half
 * as foreground and lower half as background. A half that is transparent is
 * left to the terminal's own background, which is what lets the block sit on
 * any ground without a box around it. */
static void art_row(FILE *out, unsigned row)
{
    unsigned x;

    for (x = 0u; x < BRAND_ART_W; x++) {
        const unsigned char *top = pixel(x, row * 2u);
        const unsigned char *bot = pixel(x, (row * 2u) + 1u);

        if (top != NULL && bot != NULL) {
            fprintf(out, "\x1b[38;2;%u;%u;%um\x1b[48;2;%u;%u;%um\xe2\x96\x80", top[0], top[1],
                    top[2], bot[0], bot[1], bot[2]);
        } else if (top != NULL) {
            fprintf(out, "\x1b[38;2;%u;%u;%um\x1b[49m\xe2\x96\x80", top[0], top[1], top[2]);
        } else if (bot != NULL) {
            fprintf(out, "\x1b[38;2;%u;%u;%um\x1b[49m\xe2\x96\x84", bot[0], bot[1], bot[2]);
        } else {
            fputs("\x1b[0m ", out);
        }
    }
    fputs("\x1b[0m", out);
}

void brand_banner(FILE *out, unsigned cols, const char *version)
{
    const char *reset = theme_sgr(THEME_RESET);
    bool utf8 = out_utf8_locale();
    bool art =
        theme_colour() && utf8 && truecolor() && cols >= BRAND_ART_W + BRAND_GAP + BRAND_CAPTION_W;
    const char *dot = utf8 ? "\xe2\x80\xa2" : "-";
    unsigned rows = (BRAND_ART_H + 1u) / 2u;
    unsigned r;

    if (!art) {
        fprintf(out, "\n%s%s%s %s%s%s %s\n%s%s%s\n\n", theme_sgr(THEME_BRAND_GLYPH),
                utf8 ? "\xe2\x97\x8f" : "*", reset, theme_sgr(THEME_BRAND_NAME), BRAND_NAME, reset,
                version, theme_sgr(THEME_BRAND_TAGLINE), BRAND_TAGLINE, reset);
        return;
    }
    fputc('\n', out);
    for (r = 0u; r < rows; r++) {
        art_row(out, r);
        if (r == BRAND_CAPTION_ROW) {
            fprintf(out, "%*s%s\xe2\x97\x8f  R E D S T O N E%s", (int)BRAND_GAP, "",
                    theme_sgr(THEME_BRAND_NAME), reset);
        } else if (r == BRAND_CAPTION_ROW + 1u) {
            fprintf(out, "%*s%s%s%s", (int)BRAND_GAP, "", theme_sgr(THEME_BRAND_TAGLINE),
                    BRAND_TAGLINE, reset);
        } else if (r == BRAND_CAPTION_ROW + 2u) {
            fprintf(out, "%*s%sPure C99 %s sqlite3 drop-in %s v%s%s", (int)BRAND_GAP, "",
                    theme_sgr(THEME_BRAND_FACTS), dot, dot, version, reset);
        }
        fputc('\n', out);
    }
    fputc('\n', out);
}

void brand_prompt(char *buf, size_t size)
{
    const char *reset = theme_sgr(THEME_RESET);
    bool utf8 = out_utf8_locale();

    if (size == 0u) {
        return;
    }
    if (!utf8) {
        (void)snprintf(buf, size, "%s%s%s> ", theme_sgr(THEME_BRAND_NAME), BRAND_NAME, reset);
        return;
    }
    (void)snprintf(buf, size, "%s\xe2\x97\x8f%s %s%s%s %s\xe2\x9d\xaf%s ",
                   theme_sgr(THEME_BRAND_GLYPH), reset, theme_sgr(THEME_BRAND_NAME), BRAND_NAME,
                   reset, theme_sgr(THEME_BRAND_GLYPH), reset);
}
