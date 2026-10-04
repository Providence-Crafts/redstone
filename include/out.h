/*
 * out.h - result formatting.
 *
 * Everything redstone prints for a query goes through here. db.c executes and
 * hands over values; this module decides what they look like. The split is
 * what keeps `--compat` honest: parity is a property of one module with one
 * test suite, not of formatting code scattered through the executor.
 *
 * The model is upstream's, not a mode-per-printer: one specification (a
 * style, a quoting for text, titles and blobs, separators, limits, borders)
 * of which each named mode is a preset. That is why the `.mode` option flags
 * compose with every mode instead of only the ones that thought of them.
 *
 * Parity is claimed, and enforced by tests/parity.sh, for the modes people
 * script against: ascii box column csv html insert json line list markdown
 * quote table tabs. The rest are rendered on a best-effort basis; see
 * docs/notes/phase5-output-parity.md.
 */
#ifndef REDSTONE_OUT_H
#define REDSTONE_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* The storage class of one value, as sqlite reports it. */
typedef enum { OUT_NULL = 0, OUT_INT, OUT_REAL, OUT_TEXT, OUT_BLOB } OutType;

/* One value of one row. TEXT points at NUL-terminated UTF-8; BLOB at NBYTE
 * raw bytes. Both are borrowed for the duration of the out_row call. */
typedef struct {
    OutType type;
    const char *text;
    const unsigned char *blob;
    size_t nbyte;
} OutValue;

typedef struct Out Out;

/* STREAM may be NULL and set later; nothing is written until out_row. */
Out *out_new(FILE *stream);
void out_free(Out *out);
void out_set_stream(Out *out, FILE *stream);

/* Select a named mode. Unknown names are rejected, leaving the mode alone.
 * A mode is a preset: it resets separators, the NULL text, the quoting and
 * whether headers show. */
bool out_set_mode(Out *out, const char *name);
const char *out_mode_name(const Out *out);

/* Names of all the modes, in order, terminated by NULL. For `.mode --list`
 * and for completion. */
const char *const *out_mode_names(void);

/* The two starting points. `pretty` is what redstone does with no arguments:
 * box drawing, headers, colour when the terminal takes it. `compat` is
 * sqlite3(1)'s batch default exactly: list mode, "|", no headers, no colour. */
void out_set_pretty(Out *out);
void out_set_compat(Out *out);
bool out_is_compat(const Out *out);

/* Does the locale name a UTF-8 charset, so box-drawing characters will render? */
bool out_utf8_locale(void);

void out_set_headers(Out *out, bool on);
bool out_headers(const Out *out);
void out_set_colsep(Out *out, const char *sep);
const char *out_colsep(const Out *out);
void out_set_rowsep(Out *out, const char *sep);
const char *out_rowsep(const Out *out);
void out_set_null_text(Out *out, const char *text);
const char *out_null_text(const Out *out);
void out_set_table_name(Out *out, const char *name);

/* Colour is only ever applied by the columnar and line styles, and only to
 * cells; separators and borders are never coloured, so the output stays
 * readable when the sequences are stripped. Off by default; main.c turns it
 * on from theme_colour(). */
void out_set_colour(Out *out, bool on);

/* The per-column widths set by .width, for .show to print back. Returns the
 * count and writes the array pointer to *WIDTHS. */
size_t out_widths(const Out *out, const short **widths);

/* The width of the terminal, for wrapping. Zero means unlimited. Setting an
 * explicit width turns off auto-detection, matching an explicit --screenwidth
 * N on the command line. */
void out_set_screen_width(Out *out, unsigned cols);

/* ON re-reads the terminal width from the tty via ioctl(TIOCGWINSZ) before
 * every result, so a live resize is honoured; OFF (the default) leaves the
 * width exactly as out_set_screen_width() last left it. main.c turns this on
 * for its own interactive, non-compat use, which is the one case where
 * byte-for-byte parity with sqlite3(1) is not the goal; --compat and
 * --screenwidth/--sw N both turn it back off. */
void out_set_auto_screen_width(Out *out, bool on);

/* Apply the arguments of a `.mode` command: an optional mode name followed by
 * option flags (--wrap N, --quote ARG, --border on|off, ...). Returns false
 * after writing a message to ERR. ARGV holds ARGC arguments, the first of
 * which is the first argument after `.mode` itself. */
bool out_command(Out *out, int argc, const char *const *argv, FILE *err);

/* Print the current mode and the options that differ from its preset, the way
 * `.mode` with no arguments does. */
void out_describe(const Out *out, FILE *stream);

/* One result set: begin, then a row per row, then end. NAMES has NCOL
 * entries, borrowed for the duration of the result set. Each returns false on
 * a write or allocation failure, after which the caller should stop. */
bool out_begin(Out *out, int ncol, const char *const *names);
bool out_row(Out *out, const OutValue *values);
bool out_end(Out *out);

#endif /* REDSTONE_OUT_H */
