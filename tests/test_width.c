/*
 * Tests for width.c: display width of UTF-8 text.
 *
 * width_char's binary search used to loop forever for code points near the
 * low end of the table (an unsigned index that should have gone to -1
 * wrapped instead); several of these tests exist specifically to sit right
 * on that boundary, so a regression hangs the test binary rather than
 * failing an assertion quietly.
 */
#include "minunit.h"
#include "suites.h"
#include "width.h"

static const char *test_width_char_ascii(void)
{
    mu_assert("'A' should be one cell", width_char('A') == 1u);
    mu_assert("digit should be one cell", width_char('7') == 1u);
    /* Everything below the table's first entry is one cell without a table
     * lookup at all -- the common case width_char short-circuits. */
    mu_assert("just below the table should be one cell", width_char(0x2ffu) == 1u);
    return NULL;
}

static const char *test_width_char_combining(void)
{
    /* U+0300 COMBINING GRAVE ACCENT: the table's first entry, zero cells. */
    mu_assert("combining grave should be zero cells", width_char(0x300u) == 0u);
    /* U+200B ZERO WIDTH SPACE: a format character, also zero cells. */
    mu_assert("zero-width space should be zero cells", width_char(0x200bu) == 0u);
    return NULL;
}

static const char *test_width_char_cjk(void)
{
    /* U+1100 HANGUL CHOSEONG KIYEOK: the table's first double-wide span. */
    mu_assert("hangul jamo should be two cells", width_char(0x1100u) == 2u);
    /* U+4E2D, "middle" -- an ordinary CJK ideograph. */
    mu_assert("CJK ideograph should be two cells", width_char(0x4e2du) == 2u);
    return NULL;
}

/* An emoji outside the table's double-wide spans: the table sqlsh carries
 * (upstream's own, see the note in width.c) does not special-case the emoji
 * block, so these fall through to whatever span precedes them -- one cell
 * here. This documents that actual behaviour; it is not a claim that one
 * cell is the "right" width for an emoji, only that a change to it would be
 * a change in the table sqlsh is required to match byte-for-byte. */
static const char *test_width_char_emoji(void)
{
    mu_assert("emoji width should match the table's span, not a special case",
              width_char(0x1f600u) == 1u);
    return NULL;
}

/* The regression this phase fixed: a binary search whose last index could
 * reach 0 and then, with an unsigned type, wrap instead of stopping. Every
 * code point here sits at or beside one of the table's first few entries,
 * which is where that would have hung the search. If this test times out
 * rather than failing an assertion, the bug is back. */
static const char *test_width_char_low_boundary(void)
{
    mu_assert("first entry (0x300) starts a zero-width span", width_char(0x00300u) == 0u);
    mu_assert("just below the second entry is still zero-width", width_char(0x0036fu) == 0u);
    mu_assert("second entry (0x370) starts a one-width span", width_char(0x00370u) == 1u);
    mu_assert("third entry (0x483) starts a zero-width span", width_char(0x00483u) == 0u);
    mu_assert("fourth entry (0x487) starts a one-width span", width_char(0x00487u) == 1u);
    mu_assert("fifth entry (0x488) starts a zero-width span", width_char(0x00488u) == 0u);
    return NULL;
}

/* The same shape of boundary at the opposite end of the table, plus a code
 * point past the last entry, which must fall back to the last entry's width
 * rather than reading off the end of the array. */
static const char *test_width_char_high_boundary(void)
{
    mu_assert("second-to-last entry (0xe0100) is zero-width", width_char(0xe0100u) == 0u);
    mu_assert("just below the last entry is still zero-width", width_char(0xe01efu) == 0u);
    mu_assert("last entry (0xe01f0) is one-width", width_char(0xe01f0u) == 1u);
    mu_assert("past the end of the table falls back to the last entry",
              width_char(0x10ffffu) == 1u);
    return NULL;
}

static const char *test_width_of(void)
{
    mu_assert("ascii string width wrong", width_of("hello") == 5u);
    mu_assert("empty string width is zero", width_of("") == 0u);
    /* NULL is treated as "", the same convention width_longest_line
     * documents and uses internally. */
    mu_assert("NULL width is zero", width_of(NULL) == 0u);
    /* "e" + COMBINING ACUTE ACCENT (U+0301): one visible glyph, one cell. */
    mu_assert("base letter plus combining mark is one cell", width_of("e\xcc\x81") == 1u);
    /* CJK ideograph alone. */
    mu_assert("lone CJK character is two cells", width_of("\xe4\xb8\xad") == 2u);
    return NULL;
}

/* VT100/SGR escapes occupy no cells: width.h documents this explicitly ("Escapes
 * occupy no cells, which is what lets a coloured cell still line up with an
 * uncoloured one"), and it is what out.c's box/column layout relies on to size
 * a coloured column the same as an uncoloured one. */
static const char *test_width_skips_vt100(void)
{
    size_t len;

    /* "\x1b[31mred\x1b[0m": two escapes (5 and 4 bytes) around "red" (3
     * cells); the escapes must not count. */
    mu_assert("coloured text should count only its visible cells",
              width_of("\x1b[31mred\x1b[0m") == 3u);

    len = width_vt100("\x1b[31m");
    mu_assert("SGR escape length wrong", len == 5u);
    mu_assert("a byte that is not an escape sequence has zero length",
              width_vt100("\x1bnot-an-escape") == 0u);
    return NULL;
}

const char *width_suite(void)
{
    mu_run_test(test_width_char_ascii);
    mu_run_test(test_width_char_combining);
    mu_run_test(test_width_char_cjk);
    mu_run_test(test_width_char_emoji);
    mu_run_test(test_width_char_low_boundary);
    mu_run_test(test_width_char_high_boundary);
    mu_run_test(test_width_of);
    mu_run_test(test_width_skips_vt100);
    return NULL;
}
