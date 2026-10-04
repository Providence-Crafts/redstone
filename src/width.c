#include "width.h"

/* The width table below is taken verbatim from sqlite's ext/qrf/qrf.c, as
 * shipped in the amalgamation this project pins (see reference/shell.c). It is
 * public domain, like the rest of sqlite.
 *
 * It is copied rather than approximated for one reason: `redstone --compat` has
 * to align columns byte-for-byte the way sqlite3(1) does, and any table that
 * disagreed about one code point would show up as a parity failure. Treating
 * character width as a matter of opinion is how two "correct" implementations
 * end up with different output.
 *
 * Each entry says: from this code point onwards, characters are W cells wide,
 * until the next entry. Code points below the first entry are one cell.
 */
static const struct {
    unsigned char w; /* width in cells of the span starting at first */
    int32_t first;
} g_width[] = {
    /* clang-format off: the table is hand-aligned five entries to a row;
     * reflowing it would cost the grid that makes a range easy to find. */
    {0, 0x00300}, {1, 0x00370}, {0, 0x00483}, {1, 0x00487}, {0, 0x00488}, {1, 0x0048a},
    {0, 0x00591}, {1, 0x005be}, {0, 0x005bf}, {1, 0x005c0}, {0, 0x005c1}, {1, 0x005c3},
    {0, 0x005c4}, {1, 0x005c6}, {0, 0x005c7}, {1, 0x005c8}, {0, 0x00600}, {1, 0x00604},
    {0, 0x00610}, {1, 0x00616}, {0, 0x0064b}, {1, 0x0065f}, {0, 0x00670}, {1, 0x00671},
    {0, 0x006d6}, {1, 0x006e5}, {0, 0x006e7}, {1, 0x006e9}, {0, 0x006ea}, {1, 0x006ee},
    {0, 0x0070f}, {1, 0x00710}, {0, 0x00711}, {1, 0x00712}, {0, 0x00730}, {1, 0x0074b},
    {0, 0x007a6}, {1, 0x007b1}, {0, 0x007eb}, {1, 0x007f4}, {0, 0x00901}, {1, 0x00903},
    {0, 0x0093c}, {1, 0x0093d}, {0, 0x00941}, {1, 0x00949}, {0, 0x0094d}, {1, 0x0094e},
    {0, 0x00951}, {1, 0x00955}, {0, 0x00962}, {1, 0x00964}, {0, 0x00981}, {1, 0x00982},
    {0, 0x009bc}, {1, 0x009bd}, {0, 0x009c1}, {1, 0x009c5}, {0, 0x009cd}, {1, 0x009ce},
    {0, 0x009e2}, {1, 0x009e4}, {0, 0x00a01}, {1, 0x00a03}, {0, 0x00a3c}, {1, 0x00a3d},
    {0, 0x00a41}, {1, 0x00a43}, {0, 0x00a47}, {1, 0x00a49}, {0, 0x00a4b}, {1, 0x00a4e},
    {0, 0x00a70}, {1, 0x00a72}, {0, 0x00a81}, {1, 0x00a83}, {0, 0x00abc}, {1, 0x00abd},
    {0, 0x00ac1}, {1, 0x00ac6}, {0, 0x00ac7}, {1, 0x00ac9}, {0, 0x00acd}, {1, 0x00ace},
    {0, 0x00ae2}, {1, 0x00ae4}, {0, 0x00b01}, {1, 0x00b02}, {0, 0x00b3c}, {1, 0x00b3d},
    {0, 0x00b3f}, {1, 0x00b40}, {0, 0x00b41}, {1, 0x00b44}, {0, 0x00b4d}, {1, 0x00b4e},
    {0, 0x00b56}, {1, 0x00b57}, {0, 0x00b82}, {1, 0x00b83}, {0, 0x00bc0}, {1, 0x00bc1},
    {0, 0x00bcd}, {1, 0x00bce}, {0, 0x00c3e}, {1, 0x00c41}, {0, 0x00c46}, {1, 0x00c49},
    {0, 0x00c4a}, {1, 0x00c4e}, {0, 0x00c55}, {1, 0x00c57}, {0, 0x00cbc}, {1, 0x00cbd},
    {0, 0x00cbf}, {1, 0x00cc0}, {0, 0x00cc6}, {1, 0x00cc7}, {0, 0x00ccc}, {1, 0x00cce},
    {0, 0x00ce2}, {1, 0x00ce4}, {0, 0x00d41}, {1, 0x00d44}, {0, 0x00d4d}, {1, 0x00d4e},
    {0, 0x00dca}, {1, 0x00dcb}, {0, 0x00dd2}, {1, 0x00dd5}, {0, 0x00dd6}, {1, 0x00dd7},
    {0, 0x00e31}, {1, 0x00e32}, {0, 0x00e34}, {1, 0x00e3b}, {0, 0x00e47}, {1, 0x00e4f},
    {0, 0x00eb1}, {1, 0x00eb2}, {0, 0x00eb4}, {1, 0x00eba}, {0, 0x00ebb}, {1, 0x00ebd},
    {0, 0x00ec8}, {1, 0x00ece}, {0, 0x00f18}, {1, 0x00f1a}, {0, 0x00f35}, {1, 0x00f36},
    {0, 0x00f37}, {1, 0x00f38}, {0, 0x00f39}, {1, 0x00f3a}, {0, 0x00f71}, {1, 0x00f7f},
    {0, 0x00f80}, {1, 0x00f85}, {0, 0x00f86}, {1, 0x00f88}, {0, 0x00f90}, {1, 0x00f98},
    {0, 0x00f99}, {1, 0x00fbd}, {0, 0x00fc6}, {1, 0x00fc7}, {0, 0x0102d}, {1, 0x01031},
    {0, 0x01032}, {1, 0x01033}, {0, 0x01036}, {1, 0x0103b}, {0, 0x01058}, {1, 0x0105a},
    {2, 0x01100}, {0, 0x01160}, {1, 0x01200}, {0, 0x0135f}, {1, 0x01360}, {0, 0x01712},
    {1, 0x01715}, {0, 0x01732}, {1, 0x01735}, {0, 0x01752}, {1, 0x01754}, {0, 0x01772},
    {1, 0x01774}, {0, 0x017b4}, {1, 0x017b6}, {0, 0x017b7}, {1, 0x017be}, {0, 0x017c6},
    {1, 0x017c7}, {0, 0x017c9}, {1, 0x017d4}, {0, 0x017dd}, {1, 0x017de}, {0, 0x0180b},
    {1, 0x0180e}, {0, 0x018a9}, {1, 0x018aa}, {0, 0x01920}, {1, 0x01923}, {0, 0x01927},
    {1, 0x01929}, {0, 0x01932}, {1, 0x01933}, {0, 0x01939}, {1, 0x0193c}, {0, 0x01a17},
    {1, 0x01a19}, {0, 0x01b00}, {1, 0x01b04}, {0, 0x01b34}, {1, 0x01b35}, {0, 0x01b36},
    {1, 0x01b3b}, {0, 0x01b3c}, {1, 0x01b3d}, {0, 0x01b42}, {1, 0x01b43}, {0, 0x01b6b},
    {1, 0x01b74}, {0, 0x01dc0}, {1, 0x01dcb}, {0, 0x01dfe}, {1, 0x01e00}, {0, 0x0200b},
    {1, 0x02010}, {0, 0x0202a}, {1, 0x0202f}, {0, 0x02060}, {1, 0x02064}, {0, 0x0206a},
    {1, 0x02070}, {0, 0x020d0}, {1, 0x020f0}, {2, 0x02329}, {1, 0x0232b}, {2, 0x02e80},
    {0, 0x0302a}, {2, 0x03030}, {1, 0x0303f}, {2, 0x03040}, {0, 0x03099}, {2, 0x0309b},
    {1, 0x0a4d0}, {0, 0x0a806}, {1, 0x0a807}, {0, 0x0a80b}, {1, 0x0a80c}, {0, 0x0a825},
    {1, 0x0a827}, {2, 0x0ac00}, {1, 0x0d7a4}, {2, 0x0f900}, {1, 0x0fb00}, {0, 0x0fb1e},
    {1, 0x0fb1f}, {0, 0x0fe00}, {2, 0x0fe10}, {1, 0x0fe1a}, {0, 0x0fe20}, {1, 0x0fe24},
    {2, 0x0fe30}, {1, 0x0fe70}, {0, 0x0feff}, {2, 0x0ff00}, {1, 0x0ff61}, {2, 0x0ffe0},
    {1, 0x0ffe7}, {0, 0x0fff9}, {1, 0x0fffc}, {0, 0x10a01}, {1, 0x10a04}, {0, 0x10a05},
    {1, 0x10a07}, {0, 0x10a0c}, {1, 0x10a10}, {0, 0x10a38}, {1, 0x10a3b}, {0, 0x10a3f},
    {1, 0x10a40}, {0, 0x1d167}, {1, 0x1d16a}, {0, 0x1d173}, {1, 0x1d183}, {0, 0x1d185},
    {1, 0x1d18c}, {0, 0x1d1aa}, {1, 0x1d1ae}, {0, 0x1d242}, {1, 0x1d245}, {2, 0x20000},
    {1, 0x2fffe}, {2, 0x30000}, {1, 0x3fffe}, {0, 0xe0001}, {1, 0xe0002}, {0, 0xe0020},
    {1, 0xe0080}, {0, 0xe0100}, {1, 0xe01f0}
    /* clang-format on */
};

uint32_t width_decode(const char *s, size_t *used)
{
    const unsigned char *z = (const unsigned char *)s;

    if ((z[0] & 0x80u) == 0u) {
        *used = 1u;
        return z[0];
    }
    if ((z[0] & 0xe0u) == 0xc0u && (z[1] & 0xc0u) == 0x80u) {
        *used = 2u;
        return (uint32_t)(z[0] & 0x1fu) << 6 | (uint32_t)(z[1] & 0x3fu);
    }
    if ((z[0] & 0xf0u) == 0xe0u && (z[1] & 0xc0u) == 0x80u && (z[2] & 0xc0u) == 0x80u) {
        *used = 3u;
        return (uint32_t)(z[0] & 0x0fu) << 12 | (uint32_t)(z[1] & 0x3fu) << 6 |
               (uint32_t)(z[2] & 0x3fu);
    }
    if ((z[0] & 0xf8u) == 0xf0u && (z[1] & 0xc0u) == 0x80u && (z[2] & 0xc0u) == 0x80u &&
        (z[3] & 0xc0u) == 0x80u) {
        *used = 4u;
        return (uint32_t)(z[0] & 0x07u) << 18 | (uint32_t)(z[1] & 0x3fu) << 12 |
               (uint32_t)(z[2] & 0x3fu) << 6 | (uint32_t)(z[3] & 0x3fu);
    }
    /* A stray continuation or an over-long form. Upstream renders it as one
     * cell and moves on, and so does every terminal. */
    *used = 1u;
    return 0u;
}

unsigned width_char(uint32_t c)
{
    /* Signed indices, like upstream's: the search walks the last index down
     * to zero, and an unsigned one would wrap there instead of stopping. */
    int first = 0;
    int last = (int)(sizeof(g_width) / sizeof(g_width[0])) - 1;

    if (c < 0x300u) {
        return 1u; /* the overwhelmingly common case */
    }
    while (first < last - 1) {
        int mid = (first + last) / 2;
        int32_t at = g_width[mid].first;

        if (at < (int32_t)c) {
            first = mid;
        } else if (at > (int32_t)c) {
            last = mid - 1;
        } else {
            return g_width[mid].w;
        }
    }
    if (g_width[last].first > (int32_t)c) {
        return g_width[first].w;
    }
    return g_width[last].w;
}

/* Length of the VT100 escape starting at S (which must begin with ESC), or 0
 * when it is not one. Escapes occupy no cells, which is what lets the pretty
 * output colour a cell without disturbing the alignment of the column. */
size_t width_vt100(const char *s)
{
    const unsigned char *z = (const unsigned char *)s;
    size_t i = 2u;

    if (z[1] != '[') {
        return 0u;
    }
    while (z[i] >= 0x30u && z[i] <= 0x3fu) {
        i++;
    }
    while (z[i] >= 0x20u && z[i] <= 0x2fu) {
        i++;
    }
    if (z[i] < 0x40u || z[i] > 0x7eu) {
        return 0u;
    }
    return i + 1u;
}

size_t width_longest_line(const char *s, size_t *plines)
{
    size_t widest = 0u;
    size_t cells = 0u;
    size_t lines = 1u;
    size_t i = 0u;

    if (s == NULL) {
        s = "";
    }
    while (s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];

        if (c >= ' ') {
            size_t used;

            cells += width_char(width_decode(s + i, &used));
            i += used;
            continue;
        }
        if (c == 0x1bu) {
            size_t esc = width_vt100(s + i);

            if (esc > 0u) {
                i += esc;
                continue;
            }
        }
        if (c == '\t') {
            cells = (cells + 8u) & ~(size_t)7u;
        } else if (c == '\n' || c == '\r') {
            lines++;
            if (cells > widest) {
                widest = cells;
            }
            cells = 0u;
        }
        i++;
    }
    if (cells > widest) {
        widest = cells;
    }
    if (plines != NULL) {
        *plines = lines;
    }
    return widest;
}

size_t width_of(const char *s)
{
    return width_longest_line(s, NULL);
}

size_t width_fit(const char *s, size_t max)
{
    size_t cells = 0u;
    size_t i = 0u;

    while (s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        size_t used = 1u;
        unsigned w = 1u;

        if (c == 0x1bu) {
            size_t esc = width_vt100(s + i);

            if (esc > 0u) {
                i += esc;
                continue;
            }
        }
        if (c >= ' ') {
            w = width_char(width_decode(s + i, &used));
        } else {
            w = 0u;
        }
        if (cells + w > max) {
            break;
        }
        cells += w;
        i += used;
    }
    return i;
}
