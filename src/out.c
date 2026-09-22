#include "out.h"

#include "theme.h"
#include "width.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Upstream's defaults for the four limits, so that `--limits on` means the
 * same thing here as there. */
#define OUT_DFLT_CHAR_LIMIT 300
#define OUT_DFLT_LINE_LIMIT 5
#define OUT_DFLT_TITLE_LIMIT 20
/* One INSERT per row. Trunk batches rows up to 3000 characters, but the
 * 3.53.3 binary sqlsh is measured against does not, and .dump output is
 * compared byte for byte often enough that the difference matters.
 * "--multiinsert N" still asks for batching explicitly. */
#define OUT_DFLT_MULTI_INSERT 0u

#define OUT_MAX_COLUMNS 256

/* --------------------------------------------------------------------------
 * The specification
 * ------------------------------------------------------------------------ */

typedef enum {
    ST_BOX = 0,
    ST_COLUMN,
    ST_COUNT,
    ST_HTML,
    ST_INSERT,
    ST_JSON,
    ST_JOBJECT,
    ST_LINE,
    ST_LIST,
    ST_MARKDOWN,
    ST_OFF,
    ST_TABLE
} Style;

typedef enum { TX_PLAIN = 0, TX_SQL, TX_CSV, TX_HTML, TX_TCL, TX_JSON, TX_RELAXED } Text;
typedef enum { BL_AUTO = 0, BL_TEXT, BL_SQL, BL_HEX, BL_TCL, BL_JSON, BL_SIZE } Blob;
typedef enum { ES_AUTO = 0, ES_OFF, ES_ASCII, ES_SYMBOL } Esc;
typedef enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT } Align;

/* A growable byte buffer. Rendering a value is done into one of these because
 * the length of the result is not known before the escaping is applied. */
typedef struct {
    char *p;
    size_t n;
    size_t cap;
} Str;

/* One cell of a buffered result set. TEXT is the fully rendered display
 * string; TYPE survives so that alignment and colour can depend on it. */
typedef struct {
    char *text;
    OutType type;
} Cell;

struct Out {
    FILE *stream;

    int mode; /* index into g_preset, for out_mode_name */
    Style style;
    Text text;
    Text title;
    Blob blob;
    Esc esc;
    char *colsep;
    char *rowsep;
    char *null_text;
    char *table_name;
    bool headers;
    bool border;
    bool wordwrap;
    bool split;
    bool colour;
    bool compat;
    int wrap;         /* wrap cells wider than this; 0 = no wrapping */
    int charlimit;    /* truncate a cell at this many characters; 0 = no limit */
    int linelimit;    /* truncate a cell at this many lines; 0 = no limit */
    int titlelimit;   /* truncate a column title at this width; 0 = no limit */
    unsigned screen;  /* terminal width, 0 = unknown */
    bool screen_auto; /* re-read SCREEN from the tty before every result */
    unsigned multi_insert;
    Align dflt_align;
    Align *align; /* per-column overrides, nalign entries */
    size_t nalign;
    short *widths; /* per-column widths, nwidths entries */
    size_t nwidths;

    /* Per result set. */
    int ncol;
    char **name; /* raw column names, ncol entries */
    size_t nrow;
    Cell *cell; /* nrow * ncol, row-major, only for buffered styles */
    size_t cellcap;
    Str pending; /* accumulated INSERT statements, for --multiinsert */
    bool failed;
};

/* --------------------------------------------------------------------------
 * Presets
 *
 * One row per named mode, mirroring sqlite3's own table so that a mode means
 * the same thing in both shells. A NULL separator or NULL text means the
 * setting does not apply to the mode and is left as it was.
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *csep;
    const char *rsep;
    const char *null_text;
    unsigned char text;
    unsigned char title;
    unsigned char blob;
    unsigned char hdr; /* 0: leave alone, 1: off, 2: on */
    unsigned char style;
    unsigned char border;
    unsigned char split;
} Preset;

static const Preset g_preset[] = {
    {"ascii", "\x1f", "\x1e", "", TX_PLAIN, TX_PLAIN, BL_AUTO, 1, ST_LIST, 1, 0},
    {"box", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 2, ST_BOX, 1, 0},
    {"c", ",", "\n", "NULL", TX_TCL, TX_TCL, BL_TCL, 1, ST_LIST, 1, 0},
    {"column", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 2, ST_COLUMN, 1, 0},
    {"count", NULL, NULL, NULL, TX_PLAIN, TX_PLAIN, BL_AUTO, 0, ST_COUNT, 1, 0},
    {"csv", ",", "\r\n", "", TX_CSV, TX_CSV, BL_AUTO, 1, ST_LIST, 1, 0},
    {"html", NULL, NULL, "", TX_HTML, TX_HTML, BL_AUTO, 2, ST_HTML, 1, 0},
    {"insert", NULL, NULL, "NULL", TX_SQL, TX_SQL, BL_AUTO, 1, ST_INSERT, 1, 0},
    {"jatom", ",", "\n", "null", TX_JSON, TX_JSON, BL_AUTO, 1, ST_LIST, 1, 0},
    {"jobject", NULL, "\n", "null", TX_JSON, TX_JSON, BL_AUTO, 0, ST_JOBJECT, 1, 0},
    {"json", NULL, NULL, "null", TX_JSON, TX_JSON, BL_AUTO, 0, ST_JSON, 1, 0},
    {"line", ": ", "\n", "", TX_PLAIN, TX_PLAIN, BL_AUTO, 0, ST_LINE, 1, 0},
    {"list", "|", "\n", "", TX_PLAIN, TX_PLAIN, BL_AUTO, 1, ST_LIST, 1, 0},
    {"markdown", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 2, ST_MARKDOWN, 1, 0},
    {"off", NULL, NULL, NULL, TX_PLAIN, TX_PLAIN, BL_AUTO, 0, ST_OFF, 1, 0},
    {"psql", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 2, ST_TABLE, 0, 0},
    {"qbox", NULL, NULL, "NULL", TX_SQL, TX_PLAIN, BL_AUTO, 2, ST_BOX, 1, 0},
    {"quote", ",", "\n", "NULL", TX_SQL, TX_SQL, BL_AUTO, 1, ST_LIST, 1, 0},
    {"split", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 1, ST_COLUMN, 1, 1},
    {"table", NULL, NULL, "", TX_PLAIN, TX_PLAIN, BL_AUTO, 2, ST_TABLE, 1, 0},
    {"tabs", "\t", "\n", "", TX_CSV, TX_CSV, BL_AUTO, 1, ST_LIST, 1, 0},
    {"tcl", " ", "\n", "\"\"", TX_TCL, TX_TCL, BL_TCL, 1, ST_LIST, 1, 0}
    /* clang-format on */
};

#define OUT_NMODE (sizeof(g_preset) / sizeof(g_preset[0]))

const char *const *out_mode_names(void)
{
    static const char *names[OUT_NMODE + 1u];
    size_t i;

    for (i = 0u; i < OUT_NMODE; i++) {
        names[i] = g_preset[i].name;
    }
    names[OUT_NMODE] = NULL;
    return names;
}

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *copy = (char *)malloc(n);

    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static void replace_str(char **slot, const char *value)
{
    char *copy = dup_str(value);

    if (copy == NULL) {
        return; /* keep the old value rather than lose the setting */
    }
    free(*slot);
    *slot = copy;
}

static bool str_grow(Str *s, size_t add)
{
    size_t need = s->n + add + 1u;
    char *grown;

    /* p is tested as well as cap so that the "allocated" state is one the
     * reader -- and the static analyser -- can see, rather than an invariant
     * held only by the arithmetic above. */
    if (s->p != NULL && need <= s->cap) {
        return true;
    }
    while (s->cap < need) {
        s->cap = s->cap == 0u ? 64u : s->cap * 2u;
    }
    grown = (char *)realloc(s->p, s->cap);
    if (grown == NULL) {
        return false;
    }
    s->p = grown;
    return true;
}

static bool str_add(Str *s, const char *text, size_t n)
{
    if (!str_grow(s, n)) {
        return false;
    }
    memcpy(s->p + s->n, text, n);
    s->n += n;
    s->p[s->n] = '\0';
    return true;
}

static bool str_puts(Str *s, const char *text)
{
    return str_add(s, text, strlen(text));
}

static bool str_putc(Str *s, char c)
{
    return str_add(s, &c, 1u);
}

static void str_free(Str *s)
{
    free(s->p);
    s->p = NULL;
    s->n = 0u;
    s->cap = 0u;
}

static void str_clear(Str *s)
{
    s->n = 0u;
    if (s->p != NULL) {
        s->p[0] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * Value rendering
 *
 * Each of these appends one encoded value to a Str. They are deliberately
 * separate from the styles: the same text encoding is used by half a dozen
 * modes, and `--quote json` has to mean the same thing in all of them.
 * ------------------------------------------------------------------------ */

static const char g_hex[] = "0123456789abcdef";

/* Decimal, without pulling in printf for a number we already know is small. */
static bool str_num(Str *dst, unsigned long n)
{
    char buf[32];
    size_t i = sizeof(buf);

    do {
        buf[--i] = (char)('0' + (n % 10ul));
        n /= 10ul;
    } while (n > 0ul && i > 0u);
    return str_add(dst, buf + i, sizeof(buf) - i);
}

static bool add_hex(Str *dst, unsigned char c)
{
    return str_putc(dst, g_hex[(c >> 4) & 0x0fu]) && str_putc(dst, g_hex[c & 0x0fu]);
}

/* A control character that survives escaping. Upstream leaves tab, newline
 * and the CR of a CRLF pair alone, because those are layout, not noise. */
static bool is_escapable(const char *s, size_t i)
{
    unsigned char c = (unsigned char)s[i];

    if (c > 0x1fu || c == '\t' || c == '\n') {
        return false;
    }
    return !(c == '\r' && s[i + 1u] == '\n');
}

static bool needs_escaping(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (is_escapable(s, i)) {
            return true;
        }
    }
    return false;
}

static bool add_escaped(Str *dst, const char *s, Esc esc)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok;

        if (!is_escapable(s, i)) {
            ok = str_putc(dst, s[i]);
        } else if (esc == ES_SYMBOL) {
            /* U+2400 + c: the Unicode "control pictures" block. */
            ok = str_putc(dst, (char)0xe2) && str_putc(dst, (char)0x90) &&
                 str_putc(dst, (char)(0x80u + c));
        } else {
            ok = str_putc(dst, '^') && str_putc(dst, (char)(0x40u + c));
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Re-escape the tail of DST that add_text's encoder just appended (from
 * START to the end). Upstream runs this pass unconditionally after every
 * encoder, not only the plain one, so csv/html/json/tcl text can carry `^A`
 * the same as list or column output does. It is a no-op unless that tail
 * still holds a raw control byte, which is the common case since most
 * encoders already turn control bytes into their own escapes. */
static bool escape_tail(Str *dst, size_t start, Esc esc)
{
    char *tail;
    bool ok;

    if (dst->p == NULL || !needs_escaping(dst->p + start)) {
        return true;
    }
    tail = dup_str(dst->p + start);
    if (tail == NULL) {
        return false;
    }
    dst->n = start;
    dst->p[start] = '\0';
    ok = add_escaped(dst, tail, esc);
    free(tail);
    return ok;
}

/* Unlike needs_escaping(), which leaves tab/newline/CRLF alone because they
 * are layout rather than noise in most encoders, SQL's %#Q switches to
 * unistr() for ANY byte below 0x20 -- verified against the binary: a bare
 * embedded newline or tab is enough to trigger it, not just \x01-style
 * control codes. */
static bool has_ctrl_byte(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if ((unsigned char)s[i] < 0x20u) {
            return true;
        }
    }
    return false;
}

/* SQL text: a plain literal when it can be, and unistr() when it holds
 * control characters, which is what sqlite3(1) emits so that the output can
 * be fed back in. `--escape off` disables the unistr() form entirely: the
 * control bytes are emitted raw, same as `%Q` with no escaping pass to
 * follow. */
static bool add_sql_text(Str *dst, const char *s, bool escoff)
{
    size_t i;
    bool uni = !escoff && has_ctrl_byte(s);

    if (!str_puts(dst, uni ? "unistr('" : "'")) {
        return false;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok;

        if (c == '\'') {
            ok = str_puts(dst, "''");
        } else if (uni && c == '\\') {
            ok = str_puts(dst, "\\\\");
        } else if (uni && c <= 0x1fu) {
            ok = str_puts(dst, "\\u00") && add_hex(dst, c);
        } else {
            ok = str_putc(dst, (char)c);
        }
        if (!ok) {
            return false;
        }
    }
    return str_puts(dst, uni ? "')" : "'");
}

/* Upstream's qrfCsvQuote[] table, empirically: control characters and space
 * (0x00-0x20), the quote character itself, apostrophe (a legacy Excel
 * quirk), and everything from DEL up through the non-ASCII range
 * (0x7f-0xff). Bytes 0x21-0x7e other than '"' and '\'' are left bare. */
static bool csv_quote_byte(unsigned char c)
{
    return c <= 0x20u || c == '"' || c == '\'' || c >= 0x7fu;
}

static bool add_csv_text(Str *dst, const char *s, const char *sep)
{
    size_t i;
    bool quote = s[0] == '\0';

    for (i = 0u; !quote && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];

        quote = csv_quote_byte(c) || (sep[0] != '\0' && strchr(sep, (int)c) != NULL);
    }
    if (!quote) {
        return str_puts(dst, s);
    }
    if (!str_putc(dst, '"')) {
        return false;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] == '"' && !str_putc(dst, '"')) {
            return false;
        }
        if (!str_putc(dst, s[i])) {
            return false;
        }
    }
    return str_putc(dst, '"');
}

static bool add_html_text(Str *dst, const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        bool ok;

        switch (s[i]) {
        case '<':
            ok = str_puts(dst, "&lt;");
            break;
        case '>':
            ok = str_puts(dst, "&gt;");
            break;
        case '&':
            ok = str_puts(dst, "&amp;");
            break;
        case '"':
            ok = str_puts(dst, "&quot;");
            break;
        case '\'':
            ok = str_puts(dst, "&#39;");
            break;
        default:
            ok = str_putc(dst, s[i]);
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool add_tcl_text(Str *dst, const char *s)
{
    size_t i;

    if (!str_putc(dst, '"')) {
        return false;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok;

        if (c == '"' || c == '\\' || c == '$' || c == '[') {
            ok = str_putc(dst, '\\') && str_putc(dst, (char)c);
        } else if (c < 0x20u || c == 0x7fu) {
            ok = str_putc(dst, '\\') && str_putc(dst, (char)('0' + ((c >> 6) & 3u))) &&
                 str_putc(dst, (char)('0' + ((c >> 3) & 7u))) &&
                 str_putc(dst, (char)('0' + (c & 7u)));
        } else {
            ok = str_putc(dst, (char)c);
        }
        if (!ok) {
            return false;
        }
    }
    return str_putc(dst, '"');
}

static bool add_json_text(Str *dst, const char *s)
{
    size_t i;

    if (!str_putc(dst, '"')) {
        return false;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok;

        switch (c) {
        case '"':
            ok = str_puts(dst, "\\\"");
            break;
        case '\\':
            ok = str_puts(dst, "\\\\");
            break;
        case '\b':
            ok = str_puts(dst, "\\b");
            break;
        case '\f':
            ok = str_puts(dst, "\\f");
            break;
        case '\n':
            ok = str_puts(dst, "\\n");
            break;
        case '\r':
            ok = str_puts(dst, "\\r");
            break;
        case '\t':
            ok = str_puts(dst, "\\t");
            break;
        default:
            if (c < 0x20u) {
                ok = str_puts(dst, "\\u00") && add_hex(dst, c);
            } else {
                ok = str_putc(dst, (char)c);
            }
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return str_putc(dst, '"');
}

/* Encode S into DST under ENC, then run the control-character escape pass
 * over exactly the bytes the encoder just appended. Upstream applies this
 * pass after every encoder, not only the plain one, so `^A` shows up inside
 * csv/html/json/tcl text too, not just list and column output. */
static bool add_text(const Out *o, Str *dst, const char *s, Text enc)
{
    size_t start = dst->n;
    bool escoff = o->esc == ES_OFF;
    bool ok;

    switch (enc) {
    case TX_SQL:
        ok = add_sql_text(dst, s, escoff);
        break;
    case TX_RELAXED:
        /* Quote only what would otherwise be ambiguous. */
        ok = s[0] != '\0' && !needs_escaping(s) && strchr(s, '\'') == NULL
                 ? str_puts(dst, s)
                 : add_sql_text(dst, s, escoff);
        break;
    case TX_CSV:
        ok = add_csv_text(dst, s, o->colsep != NULL ? o->colsep : ",");
        break;
    case TX_HTML:
        ok = add_html_text(dst, s);
        break;
    case TX_TCL:
        ok = add_tcl_text(dst, s);
        break;
    case TX_JSON:
        ok = add_json_text(dst, s);
        break;
    case TX_PLAIN:
    default:
        ok = str_puts(dst, s);
        break;
    }
    if (!ok || escoff) {
        return ok;
    }
    return escape_tail(dst, start, o->esc == ES_SYMBOL ? ES_SYMBOL : ES_ASCII);
}

/* Which blob rendering "auto" means, given how text is being quoted. */
static Blob blob_for(Text enc)
{
    switch (enc) {
    case TX_SQL:
    case TX_RELAXED:
        return BL_SQL;
    case TX_JSON:
        return BL_JSON;
    case TX_TCL:
        return BL_TCL;
    default:
        return BL_TEXT;
    }
}

static bool add_blob(const Out *o, Str *dst, const OutValue *v, Text enc)
{
    Blob how = o->blob == BL_AUTO ? blob_for(enc) : o->blob;
    size_t i;

    switch (how) {
    case BL_SIZE:
        return str_putc(dst, '(') && str_num(dst, (unsigned long)v->nbyte) &&
               str_puts(dst, "-byte blob)");
    case BL_HEX:
    case BL_SQL:
        if (how == BL_SQL && !str_puts(dst, "x'")) {
            return false;
        }
        for (i = 0u; i < v->nbyte; i++) {
            if (!add_hex(dst, v->blob[i])) {
                return false;
            }
        }
        return how == BL_SQL ? str_putc(dst, '\'') : true;
    case BL_JSON:
        if (!str_putc(dst, '"')) {
            return false;
        }
        for (i = 0u; i < v->nbyte; i++) {
            if (!str_puts(dst, "\\u00") || !add_hex(dst, v->blob[i])) {
                return false;
            }
        }
        return str_putc(dst, '"');
    case BL_TCL:
        if (!str_putc(dst, '"')) {
            return false;
        }
        for (i = 0u; i < v->nbyte; i++) {
            unsigned char c = v->blob[i];

            if (!str_putc(dst, '\\') || !str_putc(dst, (char)('0' + ((c >> 6) & 3u))) ||
                !str_putc(dst, (char)('0' + ((c >> 3) & 7u))) ||
                !str_putc(dst, (char)('0' + (c & 7u)))) {
                return false;
            }
        }
        return str_putc(dst, '"');
    case BL_TEXT:
    case BL_AUTO:
    default: {
        /* The bytes as text, which is what sqlite3(1) shows. The copy is
         * needed because a blob is not NUL-terminated; one with an embedded
         * NUL therefore appears truncated, in both shells. */
        char *text;
        bool ok;

        if (v->blob == NULL) {
            return add_text(o, dst, v->text != NULL ? v->text : "", enc);
        }
        text = (char *)malloc(v->nbyte + 1u);
        if (text == NULL) {
            return false;
        }
        memcpy(text, v->blob, v->nbyte);
        text[v->nbyte] = '\0';
        ok = add_text(o, dst, text, enc);
        free(text);
        return ok;
    }
    }
}

/* Render VALUE into DST using the value encodings. TITLE selects the title
 * encoding instead, for column headers. */
static bool render_value(const Out *o, Str *dst, const OutValue *v, bool title)
{
    Text enc = title ? o->title : o->text;

    str_clear(dst);
    switch (v->type) {
    case OUT_NULL:
        /* Upstream hardcodes the html style's NULL rendering to "null" at
         * query-execution time (qrfInitialize), ignoring --null/-nullvalue
         * entirely; verified against the 3.53.3 binary, not just shell.c. */
        if (o->style == ST_HTML) {
            return str_puts(dst, "null");
        }
        return str_puts(dst, o->null_text != NULL ? o->null_text : "");
    case OUT_INT:
    case OUT_REAL:
        /* Numbers are never quoted, in any mode: the text sqlite produced is
         * already the canonical rendering. */
        return str_puts(dst, v->text != NULL ? v->text : "");
    case OUT_BLOB:
        return add_blob(o, dst, v, enc);
    case OUT_TEXT:
    default:
        return add_text(o, dst, v->text != NULL ? v->text : "", enc);
    }
}

/* --------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------ */

static bool w_bytes(Out *o, const char *s, size_t n)
{
    if (o->failed) {
        return false;
    }
    if (n > 0u && fwrite(s, 1u, n, o->stream) != n) {
        o->failed = true;
    }
    return !o->failed;
}

static bool w_str(Out *o, const char *s)
{
    return w_bytes(o, s, strlen(s));
}

static bool w_rep(Out *o, const char *unit, size_t times)
{
    size_t i;

    for (i = 0u; i < times; i++) {
        if (!w_str(o, unit)) {
            return false;
        }
    }
    return true;
}

static const char *sgr(const Out *o, ThemeStyle style)
{
    return o->colour ? theme_sgr(style) : "";
}

/* The palette entry a value of this type is drawn in. */
static ThemeStyle style_for(OutType type)
{
    switch (type) {
    case OUT_NULL:
        return THEME_NULL;
    case OUT_INT:
        return THEME_INTEGER;
    case OUT_REAL:
        return THEME_REAL;
    case OUT_BLOB:
        return THEME_BLOB;
    case OUT_TEXT:
    default:
        return THEME_STRING;
    }
}

/* --------------------------------------------------------------------------
 * Streaming styles
 * ------------------------------------------------------------------------ */

static bool list_header(Out *o)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = true;

    for (i = 0; ok && i < o->ncol; i++) {
        OutValue v = {OUT_TEXT, o->name[i], NULL, 0u};

        ok = (i == 0 || w_str(o, o->colsep)) && render_value(o, &buf, &v, true) &&
             w_str(o, buf.p != NULL ? buf.p : "");
    }
    str_free(&buf);
    return ok && w_str(o, o->rowsep);
}

static bool list_row(Out *o, const OutValue *values)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = true;

    for (i = 0; ok && i < o->ncol; i++) {
        ok = (i == 0 || w_str(o, o->colsep)) && render_value(o, &buf, &values[i], false) &&
             w_str(o, buf.p != NULL ? buf.p : "");
    }
    str_free(&buf);
    return ok && w_str(o, o->rowsep);
}

static bool line_row(Out *o, const OutValue *values)
{
    int i;
    size_t wide = 0u;
    Str buf = {NULL, 0u, 0u};
    bool ok = true;

    for (i = 0; i < o->ncol; i++) {
        size_t n = width_of(o->name[i]);

        if (n > wide) {
            wide = n;
        }
    }
    if (o->nrow > 0u && !w_str(o, "\n")) {
        return false;
    }
    for (i = 0; ok && i < o->ncol; i++) {
        size_t n = width_of(o->name[i]);
        const char *text;
        size_t j;

        ok = w_rep(o, " ", wide - n) && w_str(o, sgr(o, THEME_HEADER)) && w_str(o, o->name[i]) &&
             w_str(o, sgr(o, THEME_RESET)) && w_str(o, o->colsep) &&
             render_value(o, &buf, &values[i], false);
        if (!ok) {
            break;
        }
        /* Continuation lines line up under the value, not under the name. */
        text = buf.p != NULL ? buf.p : "";
        ok = w_str(o, sgr(o, style_for(values[i].type)));
        for (j = 0u; ok && text[j] != '\0'; j++) {
            ok = w_bytes(o, text + j, 1u);
            if (text[j] == '\n') {
                ok = ok && w_rep(o, " ", wide + width_of(o->colsep));
            }
        }
        ok = ok && w_str(o, sgr(o, THEME_RESET)) && w_str(o, "\n");
    }
    str_free(&buf);
    return ok;
}

static bool html_header(Out *o)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = w_str(o, "<TR>\n");

    for (i = 0; ok && i < o->ncol; i++) {
        OutValue v = {OUT_TEXT, o->name[i], NULL, 0u};

        ok = w_str(o, "<TH>") && render_value(o, &buf, &v, true) &&
             w_str(o, buf.p != NULL ? buf.p : "") && w_str(o, "\n");
    }
    str_free(&buf);
    return ok && w_str(o, "</TR>\n");
}

static bool html_row(Out *o, const OutValue *values)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = w_str(o, "<TR>\n");

    for (i = 0; ok && i < o->ncol; i++) {
        ok = w_str(o, "<TD>") && render_value(o, &buf, &values[i], false) &&
             w_str(o, buf.p != NULL ? buf.p : "") && w_str(o, "\n");
    }
    str_free(&buf);
    return ok && w_str(o, "</TR>\n");
}

static bool json_row(Out *o, const OutValue *values, const char *open, const char *close)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = w_str(o, open);

    for (i = 0; ok && i < o->ncol; i++) {
        ok = (i == 0 || w_str(o, ",")) && add_json_text(&buf, o->name[i]) &&
             w_str(o, buf.p != NULL ? buf.p : "") && w_str(o, ":") &&
             render_value(o, &buf, &values[i], false) && w_str(o, buf.p != NULL ? buf.p : "");
        str_clear(&buf);
    }
    str_free(&buf);
    return ok && w_str(o, close);
}

/* INSERT statements, batched until the pending text exceeds --multiinsert. */
static bool insert_row(Out *o, const OutValue *values)
{
    int i;
    Str buf = {NULL, 0u, 0u};
    bool ok = true;
    bool fresh = o->pending.n == 0u;

    if (fresh) {
        ok = str_puts(&o->pending, "INSERT INTO ") &&
             str_puts(&o->pending, o->table_name != NULL ? o->table_name : "tab") &&
             str_puts(&o->pending, " VALUES(");
    } else {
        ok = str_puts(&o->pending, ",(");
    }
    for (i = 0; ok && i < o->ncol; i++) {
        ok = (i == 0 || str_putc(&o->pending, ',')) && render_value(o, &buf, &values[i], false) &&
             str_puts(&o->pending, buf.p != NULL ? buf.p : "");
    }
    ok = ok && str_putc(&o->pending, ')');
    str_free(&buf);
    if (!ok) {
        return false;
    }
    if (o->pending.n >= (size_t)o->multi_insert) {
        ok = w_str(o, o->pending.p) && w_str(o, ";\n");
        str_clear(&o->pending);
    }
    return ok;
}

/* --------------------------------------------------------------------------
 * Columnar styles
 *
 * box, table, markdown and column differ only in what they draw between
 * cells, so they share one layout pass and one set of border strings.
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *v;     /* vertical between cells */
    const char *left;  /* left edge of a data row, "" when there is no border */
    const char *right; /* right edge of a data row */
    const char *top[4];
    const char *head[4]; /* separator under the headers */
    const char *mid[4];  /* separator between rows, when rows are multi-line */
    const char *bot[4];  /* left, fill, junction, right */
    bool trim;           /* right-trim lines: there is no right edge to keep */
} Border;

static const Border g_box = {"│",
                             "│",
                             "│",
                             {"╭", "─", "┬", "╮"},
                             {"╞", "═", "╪", "╡"},
                             {"├", "─", "┼", "┤"},
                             {"╰", "─", "┴", "╯"},
                             false};

static const Border g_table = {"|",
                               "|",
                               "|",
                               {"+", "-", "+", "+"},
                               {"+", "-", "+", "+"},
                               {"+", "-", "+", "+"},
                               {"+", "-", "+", "+"},
                               false};

/* psql: the same grid without the outer frame. */
static const Border g_plain = {"|",
                               "",
                               "",
                               {NULL, NULL, NULL, NULL},
                               {"", "-", "+", ""},
                               {"", "-", "+", ""},
                               {NULL, NULL, NULL, NULL},
                               true};

static const Border g_markdown = {"|",
                                  "|",
                                  "|",
                                  {NULL, NULL, NULL, NULL},
                                  {"|", "-", "|", "|"},
                                  {NULL, NULL, NULL, NULL},
                                  {NULL, NULL, NULL, NULL},
                                  false};

/* column mode: no borders at all, two spaces between columns and a rule of
 * hyphens under the headers. */
static const Border g_column = {"  ",
                                "",
                                "",
                                {NULL, NULL, NULL, NULL},
                                {"", "-", "  ", ""},
                                {NULL, NULL, NULL, NULL},
                                {NULL, NULL, NULL, NULL},
                                true};

/* Is the locale one in which box-drawing characters will render? Upstream
 * does not ask, so the fallback is suppressed in compat mode: matching
 * sqlite3(1) byte for byte matters more than a tidy screen on a terminal that
 * asked for ASCII. */
static bool utf8_locale(void)
{
    const char *const names[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    size_t i;

    for (i = 0u; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *v = getenv(names[i]);

        if (v != NULL && v[0] != '\0') {
            return strstr(v, "UTF-8") != NULL || strstr(v, "utf8") != NULL ||
                   strstr(v, "UTF8") != NULL || strstr(v, "utf-8") != NULL;
        }
    }
    return false;
}

static const Border *border_for(const Out *o)
{
    switch (o->style) {
    case ST_BOX:
        return o->compat || utf8_locale() ? &g_box : &g_table;
    case ST_TABLE:
        return o->border ? &g_table : &g_plain;
    case ST_MARKDOWN:
        return &g_markdown;
    case ST_COLUMN:
    default:
        return &g_column;
    }
}

/* Cell padding inside the vertical rules: one space either side, except in
 * column mode where the columns stand on their own. */
static size_t cell_margin(const Out *o)
{
    return o->style == ST_COLUMN ? 0u : 1u;
}

/* Expand tabs and copy up to the next newline into DST. Returns the text
 * following the newline, or NULL when the last line has been produced. */
static const char *next_line(const char *s, Str *dst, size_t *cells)
{
    size_t col = 0u;

    str_clear(dst);
    while (*s != '\0' && *s != '\n' && *s != '\r') {
        if (*s == '\t') {
            size_t stop = (col + 8u) & ~(size_t)7u;

            if (!str_add(dst, "        ", stop - col)) {
                return NULL;
            }
            col = stop;
            s++;
            continue;
        }
        if (!str_putc(dst, *s)) {
            return NULL;
        }
        col++;
        s++;
    }
    *cells = width_of(dst->p != NULL ? dst->p : "");
    if (*s == '\r' && s[1] == '\n') {
        s++;
    }
    return *s == '\0' ? NULL : s + 1;
}

static bool str_rep(Str *dst, const char *unit, size_t times)
{
    size_t i;

    for (i = 0u; i < times; i++) {
        if (!str_puts(dst, unit)) {
            return false;
        }
    }
    return true;
}

/* Append TEXT to LINE, padded to W cells under ALIGN and drawn in STYLE. */
static bool line_pad(const Out *o, Str *line, const char *text, size_t w, Align align,
                     ThemeStyle style)
{
    size_t n = width_of(text);
    size_t left = 0u;
    size_t right;

    if (n > w) {
        n = w; /* the layout has already truncated; never pad negatively */
    }
    if (align == AL_RIGHT) {
        left = w - n;
    } else if (align == AL_CENTER) {
        left = (w - n) / 2u;
    }
    right = w - n - left;
    return str_rep(line, " ", left) && str_puts(line, sgr(o, style)) && str_puts(line, text) &&
           str_puts(line, sgr(o, THEME_RESET)) && str_rep(line, " ", right);
}

static bool line_rule(const Out *o, Str *line, const char *const parts[4], const size_t *widths,
                      size_t margin)
{
    int i;

    if (parts[0] == NULL) {
        return true; /* this style draws no such rule */
    }
    str_clear(line);
    if (!str_puts(line, parts[0])) {
        return false;
    }
    for (i = 0; i < o->ncol; i++) {
        if (i > 0 && !str_puts(line, parts[2])) {
            return false;
        }
        if (!str_rep(line, parts[1], widths[(size_t)i] + (2u * margin))) {
            return false;
        }
    }
    return str_puts(line, parts[3]);
}

/* Right-trim by buffering the line: styles without a right edge must not leave
 * trailing spaces, because sqlite3(1) does not. */
static bool w_line(Out *o, const Str *line, bool trim)
{
    size_t n = line->n;

    if (trim) {
        while (n > 0u && line->p[n - 1u] == ' ') {
            n--;
        }
    }
    return w_bytes(o, line->p != NULL ? line->p : "", n) && w_str(o, "\n");
}

/* Split CELL into display lines of at most W cells each, expanding tabs and
 * breaking at embedded newlines. Returns an array of N strings the caller
 * frees with free_lines, or NULL on failure. W of 0 means no wrapping. */
static char **split_cell(const char *cell, size_t w, bool wordwrap, size_t *n)
{
    char **lines = NULL;
    size_t cap = 0u;
    size_t count = 0u;
    Str buf = {NULL, 0u, 0u};
    /* A NULL cell is an empty one: every mode renders its own null text
     * before reaching here, so there is nothing to split. */
    const char *rest = cell != NULL ? cell : "";
    bool first = true;

    while (first || rest != NULL) {
        size_t cells = 0u;
        const char *from;
        size_t offset = 0u;

        first = false;
        rest = next_line(rest != NULL ? rest : "", &buf, &cells);
        from = buf.p;
        if (from == NULL) {
            /* Nothing was buffered, so there is nothing to measure either. */
            from = "";
            cells = 0u;
        }
        do {
            size_t take = strlen(from + offset);
            char *copy;

            if (w > 0u && cells > w) {
                take = width_fit(from + offset, w);
                if (take == 0u) {
                    /* One character is wider than the column: emit it anyway
                     * rather than looping forever on a zero-length chunk. */
                    (void)width_decode(from + offset, &take);
                }
                if (wordwrap && from[offset + take] != '\0') {
                    size_t back = take;

                    while (back > 0u && from[offset + back] != ' ') {
                        back--;
                    }
                    if (back > 0u) {
                        take = back;
                    }
                }
            }
            if (count == cap) {
                size_t grown = cap == 0u ? 4u : cap * 2u;
                char **bigger = (char **)realloc((void *)lines, grown * sizeof(*bigger));

                if (bigger == NULL) {
                    goto fail;
                }
                lines = bigger;
                cap = grown;
            }
            copy = (char *)malloc(take + 1u);
            if (copy == NULL) {
                goto fail;
            }
            memcpy(copy, from + offset, take);
            copy[take] = '\0';
            lines[count++] = copy;
            offset += take;
            while (wordwrap && from[offset] == ' ') {
                offset++;
            }
            cells = width_of(from + offset);
        } while (from[offset] != '\0');
    }
    str_free(&buf);
    *n = count;
    return lines;

fail:
    while (count > 0u) {
        free(lines[--count]);
    }
    free((void *)lines);
    str_free(&buf);
    *n = 0u;
    return NULL;
}

static void free_lines(char **lines, size_t n)
{
    while (n > 0u) {
        free(lines[--n]);
    }
    free((void *)lines);
}

/* Truncate S in place to LIMIT display cells. */
static void clip(char *s, int limit)
{
    if (limit > 0) {
        s[width_fit(s, (size_t)limit)] = '\0';
    }
}

/* --charlimit: truncate S to LIMIT display cells and append "...", the way
 * qrfRenderValue's tail does. Upstream floors the limit at 4 so the ellipsis
 * always has room; S must have that room too, so this runs on the growable
 * render buffer before the cell text is duplicated to its final size. */
static bool truncate_charlimit(Str *s, int limit)
{
    size_t cut;

    if (limit <= 0 || s->p == NULL) {
        return true;
    }
    if (limit < 4) {
        limit = 4;
    }
    cut = width_fit(s->p, (size_t)limit);
    if (s->p[cut] == '\0') {
        return true; /* already within the limit */
    }
    s->n = cut;
    s->p[cut] = '\0';
    return str_puts(s, "...");
}

static Align align_of(const Out *o, size_t col)
{
    size_t r;
    bool numeric = false;

    if (col < o->nalign) {
        return o->align[col];
    }
    for (r = 0u; r < o->nrow; r++) {
        OutType t = o->cell[(r * (size_t)o->ncol) + col].type;

        if (t == OUT_INT || t == OUT_REAL) {
            numeric = true;
        } else if (t != OUT_NULL) {
            return o->dflt_align;
        }
    }
    return numeric ? AL_RIGHT : o->dflt_align;
}

/* Natural widths, capped by --widths and --wrap. NATURAL records each
 * column's width before that cap, and FIXED whether --widths pinned it
 * explicitly -- both of which restrict_screen_width() needs to decide how
 * hard a column may be squeezed further. */
static void layout(const Out *o, char *const *titles, size_t *widths, size_t *natural, bool *fixed,
                   bool *multiline)
{
    size_t i;
    size_t r;

    *multiline = false;
    for (i = 0u; i < (size_t)o->ncol; i++) {
        size_t cap = 0u;

        widths[i] = o->headers ? width_of(titles[i]) : 0u;
        for (r = 0u; r < o->nrow; r++) {
            const char *text = o->cell[(r * (size_t)o->ncol) + i].text;
            size_t lines = 1u;
            size_t n = width_longest_line(text, &lines);

            if (n > widths[i]) {
                widths[i] = n;
            }
            if (lines > 1u) {
                *multiline = true;
            }
        }
        natural[i] = widths[i];
        fixed[i] = i < o->nwidths && o->widths[i] > 0;
        if (fixed[i]) {
            cap = (size_t)o->widths[i];
        } else if (o->wrap > 0) {
            cap = (size_t)o->wrap;
        }
        if (cap > 0u && widths[i] > cap) {
            widths[i] = cap;
            *multiline = true;
        }
        if (widths[i] == 0u) {
            widths[i] = 1u; /* an all-empty column still needs a cell */
        }
    }
}

/* The terminal width to lay out against: SCREEN as last set, or, when
 * auto-detection is on, whatever ioctl(TIOCGWINSZ) reports right now -- tried
 * against the output stream first and then stdin/stderr, exactly as upstream
 * falls back, with 80 as the last resort. Auto mode deliberately does not
 * cache this: re-probing on every result is what makes a live resize work. */
static unsigned resolve_screen_width(const Out *o)
{
    struct winsize ws;

    if (!o->screen_auto) {
        return o->screen;
    }
    if (o->stream != NULL && ioctl(fileno(o->stream), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80u;
}

#define OUT_MIN_SQUOZE 8u
#define OUT_MIN_EX_SQUOZE 16u

/* Upstream's qrfRestrictScreenWidth, adapted to our border geometry instead
 * of its hard-coded per-style formulas: EDGE and SEP below come from the
 * actual border strings, so box, table, plain, markdown and column all fall
 * out of the same math. If the natural layout already fits, nothing changes.
 * Otherwise the margin is given up first (upstream's cheap first move), then
 * the widest non-fixed column is repeatedly halved -- never below
 * OUT_MIN_SQUOZE, and only while it is either wide in absolute terms
 * (OUT_MIN_EX_SQUOZE) or wide relative to its own natural width -- until the
 * row fits or nothing eligible remains. Shrinking a column below its natural
 * width is exactly what already makes columnar_row() wrap it via
 * split_cell(), so no separate wrapping path is needed here. */
static void restrict_screen_width(const Out *o, const Border *b, size_t *widths,
                                  const size_t *natural, const bool *fixed, size_t *margin,
                                  bool *multiline)
{
    unsigned screen = resolve_screen_width(o);
    size_t ncol = (size_t)o->ncol;
    size_t edge;
    size_t sep;
    size_t sumW = 0u;
    size_t targetW;
    long avail;
    size_t i;

    if (screen == 0u || ncol == 0u) {
        return; /* no restriction requested, or nothing to lay out */
    }
    edge = width_of(b->left) + width_of(b->right);
    sep = (ncol - 1u) * width_of(b->v);
    for (i = 0u; i < ncol; i++) {
        sumW += widths[i];
    }
    if (sumW + edge + sep + (ncol * 2u * *margin) <= (size_t)screen) {
        return; /* already fits */
    }

    *margin = 0u;
    avail = (long)screen - (long)edge - (long)sep;
    targetW = avail > 0 ? (size_t)avail : 0u;

    while (sumW > targetW) {
        bool found = false;
        size_t ix = 0u;
        size_t mx = 0u;
        size_t gain;

        for (i = 0u; i < ncol; i++) {
            size_t w = widths[i];

            if (!fixed[i] && w > mx && w > OUT_MIN_SQUOZE &&
                (w > OUT_MIN_EX_SQUOZE || w * 2u > natural[i])) {
                ix = i;
                mx = w;
                found = true;
            }
        }
        if (!found) {
            break; /* nothing left that may shrink further */
        }
        gain = mx >= (size_t)OUT_MIN_SQUOZE * 2u ? mx / 2u : mx - OUT_MIN_SQUOZE;
        if (sumW - gain < targetW) {
            gain = sumW - targetW;
        }
        sumW -= gain;
        widths[ix] -= gain;
        *multiline = true;
    }
}

/* A title that no longer fits its (possibly screen-shrunk) column wraps onto
 * further header lines exactly as a data cell does, via the same split_cell:
 * otherwise a narrowed column's header would overrun into its neighbour and
 * the borders below it would no longer line up with anything above them. */
static bool columnar_header(Out *o, Str *line, const Border *b, char *const *titles,
                            const size_t *widths, size_t margin)
{
    /* Zeroed so that the cleanup loop is safe whatever path reaches it. */
    char **lines[OUT_MAX_COLUMNS] = {NULL};
    size_t nlines[OUT_MAX_COLUMNS] = {0u};
    size_t tallest = 1u;
    size_t i;
    size_t l;
    bool ok = true;

    for (i = 0u; i < (size_t)o->ncol; i++) {
        /* An empty title rather than a missing one: the header row has to
         * have the same number of cells as the rule above and below it. */
        const char *title = titles[i] != NULL ? titles[i] : "";

        lines[i] = split_cell(title, widths[i], o->wordwrap, &nlines[i]);
        if (lines[i] == NULL) {
            while (i > 0u) {
                i--;
                free_lines(lines[i], nlines[i]);
            }
            return false;
        }
        if (nlines[i] > tallest) {
            tallest = nlines[i];
        }
    }

    for (l = 0u; ok && l < tallest; l++) {
        str_clear(line);
        ok = str_puts(line, b->left);
        for (i = 0u; ok && i < (size_t)o->ncol; i++) {
            const char *text = l < nlines[i] ? lines[i][l] : "";

            ok = (i == 0u || str_puts(line, b->v)) && str_rep(line, " ", margin) &&
                 line_pad(o, line, text, widths[i], AL_CENTER, THEME_HEADER) &&
                 str_rep(line, " ", margin);
        }
        ok = ok && str_puts(line, b->right) && w_line(o, line, b->trim);
    }

    for (i = 0u; i < (size_t)o->ncol; i++) {
        free_lines(lines[i], nlines[i]);
    }
    return ok;
}

static bool columnar_row(Out *o, Str *line, const Border *b, const size_t *widths, size_t margin,
                         size_t row)
{
    /* Zeroed so that the cleanup loop is safe whatever path reaches it. */
    char **lines[OUT_MAX_COLUMNS] = {NULL};
    size_t nlines[OUT_MAX_COLUMNS] = {0u};
    size_t tallest = 1u;
    size_t i;
    size_t l;
    bool ok = true;

    for (i = 0u; i < (size_t)o->ncol; i++) {
        const Cell *c = &o->cell[(row * (size_t)o->ncol) + i];

        lines[i] = split_cell(c->text, widths[i], o->wordwrap, &nlines[i]);
        if (lines[i] == NULL) {
            while (i > 0u) {
                i--;
                free_lines(lines[i], nlines[i]);
            }
            return false;
        }
        if (o->linelimit > 0 && nlines[i] > (size_t)o->linelimit) {
            nlines[i] = (size_t)o->linelimit;
        }
        if (nlines[i] > tallest) {
            tallest = nlines[i];
        }
    }

    for (l = 0u; ok && l < tallest; l++) {
        str_clear(line);
        ok = str_puts(line, b->left);
        for (i = 0u; ok && i < (size_t)o->ncol; i++) {
            const Cell *c = &o->cell[(row * (size_t)o->ncol) + i];
            const char *text = l < nlines[i] ? lines[i][l] : "";

            ok = (i == 0u || str_puts(line, b->v)) && str_rep(line, " ", margin) &&
                 line_pad(o, line, text, widths[i], align_of(o, i), style_for(c->type)) &&
                 str_rep(line, " ", margin);
        }
        ok = ok && str_puts(line, b->right) && w_line(o, line, b->trim);
    }

    for (i = 0u; i < (size_t)o->ncol; i++) {
        free_lines(lines[i], nlines[i]);
    }
    return ok;
}

static bool columnar_end(Out *o)
{
    /* Zeroed because an empty result reaches the border code without layout()
     * having run, and a border of no columns still reads the array. */
    size_t widths[OUT_MAX_COLUMNS] = {0u};
    size_t natural[OUT_MAX_COLUMNS] = {0u};
    bool fixed[OUT_MAX_COLUMNS] = {false};
    char *titles[OUT_MAX_COLUMNS] = {NULL};
    const Border *b = border_for(o);
    size_t margin = cell_margin(o);
    Str line = {NULL, 0u, 0u};
    Str buf = {NULL, 0u, 0u};
    bool multiline = false;
    bool ok = true;
    size_t r;
    size_t i;
    size_t ntitle = 0u;

    if (o->nrow == 0u) {
        return true; /* sqlite3(1) prints nothing at all for an empty result */
    }
    for (i = 0u; ok && i < (size_t)o->ncol; i++) {
        OutValue v = {OUT_TEXT, o->name[i], NULL, 0u};

        ok = render_value(o, &buf, &v, true);
        titles[i] = ok ? dup_str(buf.p != NULL ? buf.p : "") : NULL;
        ok = ok && titles[i] != NULL;
        if (ok) {
            clip(titles[i], o->titlelimit);
            ntitle++;
        }
    }
    str_free(&buf);
    if (ok) {
        layout(o, titles, widths, natural, fixed, &multiline);
        restrict_screen_width(o, b, widths, natural, fixed, &margin, &multiline);
        ok = line_rule(o, &line, b->top, widths, margin) &&
             (b->top[0] == NULL || w_line(o, &line, b->trim));
    }
    if (ok && o->headers) {
        ok = columnar_header(o, &line, b, titles, widths, margin) &&
             line_rule(o, &line, b->head, widths, margin) &&
             (b->head[0] == NULL || w_line(o, &line, b->trim));
    }
    for (r = 0u; ok && r < o->nrow; r++) {
        if (r > 0u && multiline) {
            /* Rows that span several lines need something between them, or
             * the reader cannot tell where one ends. */
            if (b->mid[0] != NULL) {
                ok = line_rule(o, &line, b->mid, widths, margin) && w_line(o, &line, b->trim);
            } else if (o->style == ST_COLUMN) {
                ok = w_str(o, "\n");
            }
        }
        ok = ok && columnar_row(o, &line, b, widths, margin, r);
    }
    ok = ok && line_rule(o, &line, b->bot, widths, margin) &&
         (b->bot[0] == NULL || w_line(o, &line, b->trim));
    for (i = 0u; i < ntitle; i++) {
        free(titles[i]);
    }
    str_free(&line);
    return ok;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

static bool is_buffered(const Out *o)
{
    return o->style == ST_BOX || o->style == ST_COLUMN || o->style == ST_TABLE ||
           o->style == ST_MARKDOWN;
}

static void free_result(Out *o)
{
    size_t i;

    for (i = 0u; i < (size_t)o->ncol; i++) {
        free(o->name[i]);
    }
    free((void *)o->name);
    o->name = NULL;
    /* Streaming styles count rows without buffering any, so the array can be
     * absent while nrow is not zero. */
    for (i = 0u; o->cell != NULL && i < o->nrow * (size_t)o->ncol; i++) {
        free(o->cell[i].text);
    }
    free((void *)o->cell);
    o->cell = NULL;
    o->cellcap = 0u;
    o->nrow = 0u;
    o->ncol = 0;
}

Out *out_new(FILE *stream)
{
    Out *o = (Out *)calloc(1u, sizeof(*o));

    if (o == NULL) {
        return NULL;
    }
    o->stream = stream;
    o->mode = -1;
    o->headers = true;
    o->border = true;
    o->multi_insert = OUT_DFLT_MULTI_INSERT;
    if (!out_set_mode(o, "box")) {
        out_free(o);
        return NULL;
    }
    return o;
}

void out_free(Out *out)
{
    if (out == NULL) {
        return;
    }
    free_result(out);
    str_free(&out->pending);
    free(out->colsep);
    free(out->rowsep);
    free(out->null_text);
    free(out->table_name);
    free((void *)out->align);
    free((void *)out->widths);
    free((void *)out);
}

void out_set_stream(Out *out, FILE *stream)
{
    out->stream = stream;
    out->failed = false;
}

static int mode_index(const char *name)
{
    size_t i;

    for (i = 0u; i < OUT_NMODE; i++) {
        if (strcmp(g_preset[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Reapply the current mode's preset, discarding option flags. */
static void apply_preset(Out *o)
{
    const Preset *p = &g_preset[o->mode];

    o->style = (Style)p->style;
    o->text = (Text)p->text;
    o->title = (Text)p->title;
    o->blob = (Blob)p->blob;
    o->esc = ES_AUTO;
    o->border = p->border != 0u;
    o->split = p->split != 0u;
    o->wordwrap = false;
    o->wrap = 0;
    o->charlimit = 0;
    o->linelimit = 0;
    o->titlelimit = 0;
    o->dflt_align = AL_LEFT;
    free((void *)o->align);
    o->align = NULL;
    o->nalign = 0u;
    free((void *)o->widths);
    o->widths = NULL;
    o->nwidths = 0u;
    if (p->csep != NULL) {
        replace_str(&o->colsep, p->csep);
    }
    if (p->rsep != NULL) {
        replace_str(&o->rowsep, p->rsep);
    }
    if (p->null_text != NULL) {
        replace_str(&o->null_text, p->null_text);
    }
    if (p->hdr != 0u) {
        o->headers = p->hdr == 2u;
    }
    if (is_buffered(o) && !o->compat) {
        /* Pretty mode keeps the generous defaults that make long text
         * readable; --compat must not have them, because sqlite3(1) in a
         * script has no limits at all. */
        o->charlimit = OUT_DFLT_CHAR_LIMIT;
        o->linelimit = OUT_DFLT_LINE_LIMIT;
        o->titlelimit = OUT_DFLT_TITLE_LIMIT;
    }
}

bool out_set_mode(Out *out, const char *name)
{
    int idx = mode_index(name);

    if (idx < 0) {
        return false;
    }
    out->mode = idx;
    if (out->colsep == NULL) {
        replace_str(&out->colsep, "|");
    }
    if (out->rowsep == NULL) {
        replace_str(&out->rowsep, "\n");
    }
    if (out->null_text == NULL) {
        replace_str(&out->null_text, "");
    }
    apply_preset(out);
    return true;
}

const char *out_mode_name(const Out *out)
{
    return g_preset[out->mode].name;
}

void out_set_pretty(Out *out)
{
    out->compat = false;
    (void)out_set_mode(out, "box");
}

void out_set_compat(Out *out)
{
    out->compat = true;
    out->colour = false;
    out->screen_auto = false;
    (void)out_set_mode(out, "list");
    out->headers = false;
}

bool out_is_compat(const Out *out)
{
    return out->compat;
}

void out_set_headers(Out *out, bool on)
{
    out->headers = on;
}

bool out_headers(const Out *out)
{
    return out->headers;
}

void out_set_colsep(Out *out, const char *sep)
{
    replace_str(&out->colsep, sep);
}

const char *out_colsep(const Out *out)
{
    return out->colsep != NULL ? out->colsep : "";
}

void out_set_rowsep(Out *out, const char *sep)
{
    replace_str(&out->rowsep, sep);
}

const char *out_rowsep(const Out *out)
{
    return out->rowsep != NULL ? out->rowsep : "";
}

void out_set_null_text(Out *out, const char *text)
{
    replace_str(&out->null_text, text);
}

const char *out_null_text(const Out *out)
{
    return out->null_text != NULL ? out->null_text : "";
}

void out_set_table_name(Out *out, const char *name)
{
    replace_str(&out->table_name, name);
}

void out_set_colour(Out *out, bool on)
{
    out->colour = on && !out->compat;
}

size_t out_widths(const Out *out, const short **widths)
{
    *widths = out->widths;
    return out->nwidths;
}

void out_set_screen_width(Out *out, unsigned cols)
{
    out->screen = cols;
    out->screen_auto = false;
}

void out_set_auto_screen_width(Out *out, bool on)
{
    out->screen_auto = on;
}

/* --------------------------------------------------------------------------
 * Result sets
 * ------------------------------------------------------------------------ */

bool out_begin(Out *out, int ncol, const char *const *names)
{
    int i;

    free_result(out);
    str_clear(&out->pending);
    out->failed = false;
    if (ncol <= 0) {
        return true;
    }
    if (ncol > OUT_MAX_COLUMNS) {
        ncol = OUT_MAX_COLUMNS;
    }
    out->name = (char **)calloc((size_t)ncol, sizeof(*out->name));
    if (out->name == NULL) {
        return false;
    }
    out->ncol = ncol;
    for (i = 0; i < ncol; i++) {
        out->name[i] = dup_str(names[i] != NULL ? names[i] : "");
        if (out->name[i] == NULL) {
            free_result(out);
            return false;
        }
    }
    /* List and html headers are deferred to the first row (see out_row):
     * sqlite3(1) prints nothing at all, not even the header, for a
     * zero-row result in those styles. */
    return true;
}

static bool buffer_row(Out *out, const OutValue *values)
{
    Str buf = {NULL, 0u, 0u};
    size_t base = out->nrow * (size_t)out->ncol;
    size_t need = base + (size_t)out->ncol;
    int i;
    bool ok = true;

    if (need > out->cellcap) {
        size_t grown = out->cellcap == 0u ? (size_t)out->ncol * 16u : out->cellcap * 2u;
        Cell *bigger;

        while (grown < need) {
            grown *= 2u;
        }
        bigger = (Cell *)realloc((void *)out->cell, grown * sizeof(*bigger));
        if (bigger == NULL) {
            return false;
        }
        out->cell = bigger;
        out->cellcap = grown;
    }
    for (i = 0; ok && i < out->ncol; i++) {
        Cell *c = &out->cell[base + (size_t)i];

        ok = render_value(out, &buf, &values[i], false) && truncate_charlimit(&buf, out->charlimit);
        c->type = values[i].type;
        c->text = ok ? dup_str(buf.p != NULL ? buf.p : "") : NULL;
        ok = ok && c->text != NULL;
    }
    str_free(&buf);
    if (ok) {
        out->nrow++;
    }
    return ok;
}

bool out_row(Out *out, const OutValue *values)
{
    if (out->ncol == 0) {
        return true;
    }
    if (is_buffered(out)) {
        return buffer_row(out, values);
    }
    switch (out->style) {
    case ST_OFF:
        return true;
    case ST_COUNT:
        out->nrow++;
        return true;
    case ST_LIST:
        /* Header is lazy: a zero-row result prints nothing at all, not
         * even the header (verified against sqlite3 3.53.3). */
        if (out->nrow == 0u && out->headers && !list_header(out)) {
            return false;
        }
        if (!list_row(out, values)) {
            return false;
        }
        out->nrow++;
        return true;
    case ST_LINE:
        if (!line_row(out, values)) {
            return false;
        }
        out->nrow++;
        return true;
    case ST_HTML:
        if (out->nrow == 0u && out->headers && !html_header(out)) {
            return false;
        }
        if (!html_row(out, values)) {
            return false;
        }
        out->nrow++;
        return true;
    case ST_INSERT:
        return insert_row(out, values);
    case ST_JOBJECT:
        return json_row(out, values, "{", "}") && w_str(out, out->rowsep);
    case ST_JSON:
    default:
        if (!w_str(out, out->nrow == 0u ? "[" : ",\n")) {
            return false;
        }
        out->nrow++;
        return json_row(out, values, "{", "}");
    }
}

bool out_end(Out *out)
{
    bool ok = true;

    if (is_buffered(out)) {
        ok = columnar_end(out);
    } else if (out->style == ST_INSERT && out->pending.n > 0u) {
        ok = w_str(out, out->pending.p) && w_str(out, ";\n");
        str_clear(&out->pending);
    } else if (out->style == ST_JSON && out->nrow > 0u) {
        ok = w_str(out, "]\n");
    } else if (out->style == ST_COUNT) {
        Str buf = {NULL, 0u, 0u};

        ok = str_num(&buf, (unsigned long)out->nrow) && w_str(out, buf.p) && w_str(out, "\n");
        str_free(&buf);
    }
    free_result(out);
    if (out->stream != NULL) {
        (void)fflush(out->stream);
    }
    return ok && !out->failed;
}

/* --------------------------------------------------------------------------
 * The .mode command
 *
 * Flags compose with any mode, because a mode is only a set of defaults for
 * the same underlying specification.
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *name;
    int value;
} Word;

static bool lookup_word(const Word *table, const char *s, int *out)
{
    size_t i;

    for (i = 0u; table[i].name != NULL; i++) {
        if (strcmp(table[i].name, s) == 0) {
            *out = table[i].value;
            return true;
        }
    }
    return false;
}

static const Word g_quote_word[] = {{"plain", TX_PLAIN},     {"sql", TX_SQL},   {"csv", TX_CSV},
                                    {"html", TX_HTML},       {"tcl", TX_TCL},   {"json", TX_JSON},
                                    {"relaxed", TX_RELAXED}, {"off", TX_PLAIN}, {NULL, 0}};

static const Word g_blob_word[] = {{"auto", BL_AUTO}, {"text", BL_TEXT}, {"sql", BL_SQL},
                                   {"hex", BL_HEX},   {"tcl", BL_TCL},   {"json", BL_JSON},
                                   {"size", BL_SIZE}, {NULL, 0}};

static const Word g_esc_word[] = {
    {"auto", ES_AUTO}, {"off", ES_OFF}, {"ascii", ES_ASCII}, {"symbol", ES_SYMBOL}, {NULL, 0}};

static const Word g_align_word[] = {{"left", AL_LEFT},     {"l", AL_LEFT},   {"center", AL_CENTER},
                                    {"centre", AL_CENTER}, {"c", AL_CENTER}, {"right", AL_RIGHT},
                                    {"r", AL_RIGHT},       {NULL, 0}};

static bool parse_bool(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "true") == 0 ||
        strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "off") == 0 || strcmp(s, "no") == 0 || strcmp(s, "false") == 0 ||
        strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);

    if (end == s || *end != '\0' || v < 0 || v > 100000L) {
        return false;
    }
    *out = (int)v;
    return true;
}

/* Comma- or space-separated list of numbers, for --widths. */
static bool parse_widths(Out *o, const char *spec)
{
    const char *p = spec;
    size_t n = 0u;
    short *list = NULL;

    while (*p != '\0') {
        char *end = NULL;
        long v;
        short *bigger;

        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 30000L) {
            free((void *)list);
            return false;
        }
        p = end;
        bigger = (short *)realloc((void *)list, (n + 1u) * sizeof(*bigger));
        if (bigger == NULL) {
            free((void *)list);
            return false;
        }
        list = bigger;
        list[n++] = (short)v;
    }
    free((void *)o->widths);
    o->widths = list;
    o->nwidths = n;
    return true;
}

static bool parse_aligns(Out *o, const char *spec)
{
    char *copy = dup_str(spec);
    char *save = copy;
    size_t n = 0u;
    Align *list = NULL;

    if (copy == NULL) {
        return false;
    }
    while (*save != '\0') {
        const char *tok = save;
        int value;
        Align *bigger;

        while (*save != '\0' && *save != ',' && *save != ' ') {
            save++;
        }
        if (*save != '\0') {
            *save++ = '\0';
        }
        if (*tok == '\0') {
            continue;
        }
        if (!lookup_word(g_align_word, tok, &value)) {
            free((void *)list);
            free(copy);
            return false;
        }
        bigger = (Align *)realloc((void *)list, (n + 1u) * sizeof(*bigger));
        if (bigger == NULL) {
            free((void *)list);
            free(copy);
            return false;
        }
        list = bigger;
        list[n++] = (Align)value;
    }
    free(copy);
    free((void *)o->align);
    o->align = list;
    o->nalign = n;
    if (n == 1u) {
        o->dflt_align = list[0];
    }
    return true;
}

static bool need_value(int argc, int i, const char *flag, FILE *err)
{
    if (i + 1 < argc) {
        return true;
    }
    if (err != NULL) {
        (void)fprintf(err, "Error: %s requires an argument\n", flag);
    }
    return false;
}

static void bad_value(FILE *err, const char *flag, const char *value)
{
    if (err != NULL) {
        (void)fprintf(err, "Error: unknown %s: \"%s\"\n", flag, value);
    }
}

bool out_command(Out *out, int argc, const char *const *argv, FILE *err)
{
    int i;

    if (argc <= 0) {
        out_describe(out, err != NULL ? err : stdout);
        return true;
    }
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        int word = 0;
        int num = 0;
        bool flag = false;

        if (a[0] != '-') {
            if (i > 0 && argv[i - 1][0] != '-') {
                /* ".mode insert TABLE": a second bare word names the table
                 * the INSERT statements are for. Other modes accept and
                 * ignore it, as sqlite3(1) does. */
                out_set_table_name(out, a);
                continue;
            }
            if (!out_set_mode(out, a)) {
                if (err != NULL) {
                    (void)fprintf(err, "Error: mode should be one of:");
                    for (word = 0; word < (int)OUT_NMODE; word++) {
                        (void)fprintf(err, " %s", g_preset[word].name);
                    }
                    (void)fprintf(err, "\n");
                }
                return false;
            }
            continue;
        }
        if (strcmp(a, "--list") == 0) {
            for (word = 0; word < (int)OUT_NMODE; word++) {
                (void)fprintf(err != NULL ? err : stdout, "%s\n", g_preset[word].name);
            }
        } else if (strcmp(a, "--reset") == 0) {
            apply_preset(out);
        } else if (strcmp(a, "--limits") == 0) {
            out->charlimit = OUT_DFLT_CHAR_LIMIT;
            out->linelimit = OUT_DFLT_LINE_LIMIT;
            out->titlelimit = OUT_DFLT_TITLE_LIMIT;
        } else if (strcmp(a, "--nolimits") == 0) {
            out->charlimit = 0;
            out->linelimit = 0;
            out->titlelimit = 0;
        } else if (strcmp(a, "--noquote") == 0) {
            out->text = TX_PLAIN;
        } else if (strcmp(a, "--ww") == 0) {
            out->wordwrap = true;
        } else if (strcmp(a, "--quote") == 0 || strcmp(a, "--title") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (!lookup_word(g_quote_word, v, &word)) {
                bad_value(err, "quoting", v);
                return false;
            }
            if (a[2] == 'q') {
                out->text = (Text)word;
            } else {
                out->title = (Text)word;
            }
            i++;
        } else if (strcmp(a, "--blob-quote") == 0 || strcmp(a, "--blob") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (!lookup_word(g_blob_word, v, &word)) {
                bad_value(err, "blob quoting", v);
                return false;
            }
            out->blob = (Blob)word;
            i++;
        } else if (strcmp(a, "--escape") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (!lookup_word(g_esc_word, v, &word)) {
                bad_value(err, "escape", v);
                return false;
            }
            out->esc = (Esc)word;
            i++;
        } else if (strcmp(a, "--align") == 0) {
            if (!need_value(argc, i, a, err) || !parse_aligns(out, v)) {
                bad_value(err, "alignment", v);
                return false;
            }
            i++;
        } else if (strcmp(a, "--widths") == 0 || strcmp(a, "--width") == 0) {
            if (!need_value(argc, i, a, err) || !parse_widths(out, v)) {
                bad_value(err, "width list", v);
                return false;
            }
            i++;
        } else if (strcmp(a, "--colsep") == 0 || strcmp(a, "--rowsep") == 0 ||
                   strcmp(a, "--null") == 0 || strcmp(a, "--tablename") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (a[2] == 'c') {
                out_set_colsep(out, v);
            } else if (a[2] == 'r') {
                out_set_rowsep(out, v);
            } else if (a[2] == 'n') {
                out_set_null_text(out, v);
            } else {
                out_set_table_name(out, v);
            }
            i++;
        } else if (strcmp(a, "--border") == 0 || strcmp(a, "--wordwrap") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (!parse_bool(v, &flag)) {
                bad_value(err, "boolean", v);
                return false;
            }
            if (a[2] == 'b') {
                out->border = flag;
            } else {
                out->wordwrap = flag;
            }
            i++;
        } else if (strcmp(a, "--sw") == 0 || strcmp(a, "--screenwidth") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (strcmp(v, "off") == 0) {
                out_set_screen_width(out, 0u);
            } else if (strcmp(v, "auto") == 0) {
                out_set_auto_screen_width(out, true);
            } else if (parse_int(v, &num)) {
                out_set_screen_width(out, (unsigned)num);
            } else {
                bad_value(err, "screen width", v);
                return false;
            }
            i++;
        } else if (strcmp(a, "--wrap") == 0 || strcmp(a, "--charlimit") == 0 ||
                   strcmp(a, "--linelimit") == 0 || strcmp(a, "--titlelimit") == 0 ||
                   strcmp(a, "--multiinsert") == 0) {
            if (!need_value(argc, i, a, err)) {
                return false;
            }
            if (!parse_int(v, &num)) {
                bad_value(err, "number", v);
                return false;
            }
            if (strcmp(a, "--wrap") == 0) {
                out->wrap = num;
            } else if (strcmp(a, "--charlimit") == 0) {
                out->charlimit = num;
            } else if (strcmp(a, "--linelimit") == 0) {
                out->linelimit = num;
            } else if (strcmp(a, "--titlelimit") == 0) {
                out->titlelimit = num;
            } else {
                out->multi_insert = (unsigned)num;
            }
            i++;
        } else {
            if (err != NULL) {
                (void)fprintf(err, "Error: unknown option \"%s\"\n", a);
            }
            return false;
        }
    }
    return true;
}

void out_describe(const Out *out, FILE *stream)
{
    (void)fprintf(stream, "current output mode: %s\n", out_mode_name(out));
    (void)fprintf(stream, "  headers %s, colsep \"%s\", null \"%s\"\n", out->headers ? "on" : "off",
                  out_colsep(out), out_null_text(out));
    if (out->wrap > 0 || out->charlimit > 0 || out->linelimit > 0 || out->titlelimit > 0) {
        (void)fprintf(stream, "  wrap %d, charlimit %d, linelimit %d, titlelimit %d\n", out->wrap,
                      out->charlimit, out->linelimit, out->titlelimit);
    }
}
