/*
 * width.h - display width of UTF-8 text.
 *
 * Every aligned output in redstone -- the completion menu, the columnar output
 * modes -- needs to know how many terminal cells a string occupies, which is
 * not its byte count and not its character count: combining marks take none
 * and CJK ideographs take two.
 *
 * The estimate has to agree with sqlite3(1)'s, or `redstone --compat` would
 * misalign exactly where upstream does not, so the table in width.c is
 * upstream's own. It is data, not a dependency: see the note there.
 */
#ifndef REDSTONE_WIDTH_H
#define REDSTONE_WIDTH_H

#include <stddef.h>
#include <stdint.h>

/* Decode one UTF-8 character starting at S, storing the bytes consumed in
 * *USED (always at least 1, so a decoder loop always advances). An invalid
 * byte decodes to itself, which is what every terminal does with one. */
uint32_t width_decode(const char *s, size_t *used);

/* Cells occupied by the single code point C: 0, 1 or 2. */
unsigned width_char(uint32_t c);

/* Cells occupied by S. Embedded newlines are not interpreted; use
 * width_longest_line for text that may contain them. */
size_t width_of(const char *s);

/* Width of the widest line of S, and, when PLINES is not NULL, the number of
 * lines it spans (1 for text with no newline). */
size_t width_longest_line(const char *s, size_t *plines);

/* Length of the VT100 escape sequence starting at S, which must begin with
 * ESC, or 0 when what follows is not one. Escapes occupy no cells, which is
 * what lets a coloured cell still line up with an uncoloured one. */
size_t width_vt100(const char *s);

/* Bytes of S that fit in MAX cells without splitting a character. A character
 * two cells wide is left out rather than half printed. */
size_t width_fit(const char *s, size_t max);

#endif /* REDSTONE_WIDTH_H */
