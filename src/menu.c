#include "menu.h"

#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Room left for the prompt and for the note line. A menu that fills the screen
 * hides the statement being typed, which defeats the point. */
#define MENU_ROWS_MAX 12u
#define MENU_GAP 2u /* blank columns between cells */
#define MENU_CELL_MIN 8u
#define MENU_COLS_FALLBACK 80u

/* One drawn line: either a group header or a run of candidates laid out across
 * the grid. Candidates of a kind are contiguous because comp.c sorts by kind,
 * so a run is just a start and a count. */
typedef struct {
    bool header;
    CompKind kind;
    size_t first;
    size_t count;
} MenuLine;

struct Menu {
    CompList *list;
    size_t sel;

    unsigned cols;
    unsigned rows;

    MenuLine *line;
    size_t nlines;
    size_t lcap;
    size_t top; /* first visible line */
    unsigned cellw;
    unsigned ncols;
    bool laid_out;

    char *out;
    size_t olen;
    size_t ocap;
};

/* --------------------------------------------------------------------------
 * Width
 * ------------------------------------------------------------------------ */

/* Counts UTF-8 characters rather than bytes: continuation bytes occupy no
 * cell. Combining marks and double-width characters are not handled here —
 * that needs the tables Phase 5 brings — so a CJK column can be one cell
 * narrow. Names in a schema are overwhelmingly ASCII, and the failure mode is
 * cosmetic. */
size_t menu_display_width(const char *s)
{
    size_t w = 0u;
    size_t i;

    if (s == NULL) {
        return 0u;
    }
    for (i = 0u; s[i] != '\0'; i++) {
        if (((unsigned char)s[i] & 0xc0u) != 0x80u) {
            w++;
        }
    }
    return w;
}

/* Bytes of S that fit in MAX cells, never splitting a character. */
static size_t fit_bytes(const char *s, size_t max)
{
    size_t w = 0u;
    size_t i = 0u;

    while (s[i] != '\0') {
        if (((unsigned char)s[i] & 0xc0u) != 0x80u) {
            if (w == max) {
                break;
            }
            w++;
        }
        i++;
    }
    return i;
}

/* --------------------------------------------------------------------------
 * Output buffer
 * ------------------------------------------------------------------------ */

static bool emit_n(Menu *m, const char *s, size_t n)
{
    if (n == 0u) {
        return true;
    }
    if (m->olen + n + 1u > m->ocap) {
        size_t cap = m->ocap == 0u ? 512u : m->ocap;
        char *grown;

        while (cap < m->olen + n + 1u) {
            cap *= 2u;
        }
        grown = (char *)realloc(m->out, cap);
        if (grown == NULL) {
            return false;
        }
        m->out = grown;
        m->ocap = cap;
    }
    if (m->out == NULL) {
        return false; /* unreachable: ocap > 0 implies out != NULL */
    }
    memcpy(m->out + m->olen, s, n);
    m->olen += n;
    m->out[m->olen] = '\0';
    return true;
}

static bool emit(Menu *m, const char *s)
{
    return emit_n(m, s, strlen(s));
}

static bool emit_pad(Menu *m, size_t n)
{
    static const char spaces[] = "                ";

    while (n > 0u) {
        size_t chunk = n > sizeof(spaces) - 1u ? sizeof(spaces) - 1u : n;

        if (!emit_n(m, spaces, chunk)) {
            return false;
        }
        n -= chunk;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

Menu *menu_new(void)
{
    Menu *m = (Menu *)calloc(1u, sizeof(*m));

    if (m != NULL) {
        m->cols = MENU_COLS_FALLBACK;
        m->rows = MENU_ROWS_MAX;
    }
    return m;
}

void menu_free(Menu *m)
{
    if (m == NULL) {
        return;
    }
    comp_free(m->list);
    free(m->line);
    free(m->out);
    free(m);
}

void menu_close(Menu *m)
{
    if (m == NULL) {
        return;
    }
    comp_free(m->list);
    m->list = NULL;
    m->sel = 0u;
    m->top = 0u;
    m->nlines = 0u;
    m->laid_out = false;
}

void menu_open(Menu *m, CompList *list)
{
    if (m == NULL) {
        comp_free(list);
        return;
    }
    menu_close(m);
    if (list == NULL || comp_count(list) == 0u) {
        comp_free(list);
        return;
    }
    m->list = list;
}

bool menu_active(const Menu *m)
{
    return m != NULL && m->list != NULL;
}

const CompList *menu_list(const Menu *m)
{
    return m != NULL ? m->list : NULL;
}

const Comp *menu_selected(const Menu *m)
{
    if (!menu_active(m)) {
        return NULL;
    }
    return comp_at(m->list, m->sel);
}

size_t menu_selected_index(const Menu *m)
{
    return menu_active(m) ? m->sel : 0u;
}

void menu_set_size(Menu *m, unsigned cols, unsigned rows)
{
    if (m == NULL) {
        return;
    }
    m->cols = cols > 0u ? cols : MENU_COLS_FALLBACK;
    m->rows = rows > 0u ? rows : 1u;
    if (m->rows > MENU_ROWS_MAX) {
        m->rows = MENU_ROWS_MAX;
    }
    m->laid_out = false;
}

/* --------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------ */

static size_t item_width(const Comp *c)
{
    size_t w = menu_display_width(c->display);

    if (c->detail != NULL && c->detail[0] != '\0') {
        w += MENU_GAP + menu_display_width(c->detail);
    }
    return w;
}

static bool push_line(Menu *m, bool header, CompKind kind, size_t first, size_t count)
{
    if (m->nlines == m->lcap) {
        size_t cap = m->lcap == 0u ? 16u : m->lcap * 2u;
        MenuLine *grown = (MenuLine *)realloc(m->line, cap * sizeof(*grown));

        if (grown == NULL) {
            return false;
        }
        m->line = grown;
        m->lcap = cap;
    }
    m->line[m->nlines].header = header;
    m->line[m->nlines].kind = kind;
    m->line[m->nlines].first = first;
    m->line[m->nlines].count = count;
    m->nlines++;
    return true;
}

/* Group headers are only worth their row when there is more than one group:
 * with a single kind the label repeats what the candidates already say. */
static bool multiple_kinds(const CompList *list)
{
    size_t i;

    for (i = 1u; i < comp_count(list); i++) {
        if (comp_at(list, i)->kind != comp_at(list, i - 1u)->kind) {
            return true;
        }
    }
    return false;
}

static void layout(Menu *m)
{
    size_t n = comp_count(m->list);
    size_t widest = 0u;
    size_t i = 0u;
    bool headers;

    m->nlines = 0u;
    for (i = 0u; i < n; i++) {
        size_t w = item_width(comp_at(m->list, i));

        if (w > widest) {
            widest = w;
        }
    }
    if (widest + MENU_GAP > m->cols) {
        widest = m->cols > MENU_GAP ? m->cols - MENU_GAP : MENU_CELL_MIN;
    }
    if (widest < MENU_CELL_MIN) {
        widest = MENU_CELL_MIN;
    }
    m->cellw = (unsigned)widest;
    m->ncols = (unsigned)(m->cols / (widest + MENU_GAP));
    if (m->ncols == 0u) {
        m->ncols = 1u;
    }

    headers = multiple_kinds(m->list);
    i = 0u;
    while (i < n) {
        CompKind kind = comp_at(m->list, i)->kind;
        size_t end = i;

        while (end < n && comp_at(m->list, end)->kind == kind) {
            end++;
        }
        if (headers && !push_line(m, true, kind, i, 0u)) {
            return;
        }
        while (i < end) {
            size_t count = end - i < m->ncols ? end - i : m->ncols;

            if (!push_line(m, false, kind, i, count)) {
                return;
            }
            i += count;
        }
    }
    m->laid_out = true;
}

static void ensure_layout(Menu *m)
{
    if (!m->laid_out) {
        layout(m);
    }
}

/* Index of the line holding candidate SEL, or 0. */
static size_t line_of(const Menu *m, size_t sel)
{
    size_t i;

    for (i = 0u; i < m->nlines; i++) {
        if (!m->line[i].header && sel >= m->line[i].first &&
            sel < m->line[i].first + m->line[i].count) {
            return i;
        }
    }
    return 0u;
}

/* How many lines fit, leaving one for the note when there is something to say
 * about scrolling or truncation. */
static unsigned visible_lines(const Menu *m)
{
    unsigned budget = m->rows;

    if (m->nlines > (size_t)budget || comp_truncated(m->list)) {
        budget = budget > 1u ? budget - 1u : 1u;
    }
    return budget;
}

static void scroll_to(Menu *m, size_t line)
{
    unsigned visible = visible_lines(m);

    if (line < m->top) {
        m->top = line;
    }
    if (line >= m->top + visible) {
        m->top = line - visible + 1u;
    }
    /* A header is useless once scrolled past, but including the one directly
     * above the first visible row keeps the grouping readable. */
    if (m->top > 0u && m->line[m->top - 1u].header) {
        m->top--;
    }
}

/* --------------------------------------------------------------------------
 * Movement
 * ------------------------------------------------------------------------ */

/* Move one grid row while staying in the same column, skipping headers. The
 * target row may be shorter, in which case the last cell is chosen. */
static void move_row(Menu *m, int delta)
{
    size_t cur = line_of(m, m->sel);
    size_t col = m->sel - m->line[cur].first;
    size_t i = cur;

    for (;;) {
        if (delta < 0) {
            if (i == 0u) {
                return;
            }
            i--;
        } else {
            if (i + 1u >= m->nlines) {
                return;
            }
            i++;
        }
        if (!m->line[i].header) {
            break;
        }
    }
    if (col >= m->line[i].count) {
        col = m->line[i].count - 1u;
    }
    m->sel = m->line[i].first + col;
}

void menu_move(Menu *m, MenuMove move)
{
    size_t n;

    if (!menu_active(m)) {
        return;
    }
    ensure_layout(m);
    n = comp_count(m->list);
    switch (move) {
    case MENU_NEXT:
        m->sel = (m->sel + 1u) % n;
        break;
    case MENU_PREV:
        m->sel = m->sel == 0u ? n - 1u : m->sel - 1u;
        break;
    case MENU_UP:
        move_row(m, -1);
        break;
    case MENU_DOWN:
    default:
        move_row(m, 1);
        break;
    }
    scroll_to(m, line_of(m, m->sel));
}

/* --------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------ */

static ThemeStyle style_of(CompKind kind)
{
    switch (kind) {
    case COMP_TABLE:
        return THEME_TABLE;
    case COMP_VIEW:
        return THEME_VIEW;
    case COMP_COLUMN:
    case COMP_ALIAS:
        return THEME_COLUMN;
    case COMP_VALUE:
        return THEME_VALUE;
    case COMP_FUNCTION:
        return THEME_FUNCTION;
    case COMP_PRAGMA:
        return THEME_PRAGMA;
    case COMP_DOT_COMMAND:
        return THEME_DOT;
    case COMP_KEYWORD:
    default:
        return THEME_KEYWORD;
    }
}

/* Emit one candidate padded to the cell width. The already-typed prefix is
 * emphasised so the eye can see what it is choosing between rather than
 * re-reading the part it just typed. */
/* The candidate name, with the already-typed prefix emphasised so the eye can
 * see what it is choosing between rather than re-reading what it just typed.
 * The highlighted row is left alone: reverse video already carries it, and
 * layering colour on top of it is what makes a menu look busy. */
static bool emit_name(Menu *m, const Comp *c, bool selected, size_t cut)
{
    size_t plen = menu_display_width(comp_prefix(m->list));
    size_t pbytes;

    if (selected) {
        return emit_n(m, c->display, cut);
    }
    if (plen == 0u || plen > menu_display_width(c->display)) {
        return emit(m, theme_sgr(style_of(c->kind))) && emit_n(m, c->display, cut);
    }
    pbytes = fit_bytes(c->display, plen);
    if (pbytes > cut) {
        pbytes = cut;
    }
    return emit(m, theme_sgr(THEME_MATCH)) && emit_n(m, c->display, pbytes) &&
           emit(m, theme_sgr(THEME_RESET)) && emit(m, theme_sgr(style_of(c->kind))) &&
           emit_n(m, c->display + pbytes, cut - pbytes);
}

/* The description, when there is room for any of it. Returns the cells used,
 * zero when the detail was dropped. */
static size_t emit_detail(Menu *m, const Comp *c, bool selected, size_t room, bool *ok)
{
    size_t cut;

    if (c->detail == NULL || c->detail[0] == '\0' || room <= MENU_GAP) {
        return 0u;
    }
    cut = fit_bytes(c->detail, room - MENU_GAP);
    if (cut == 0u) {
        return 0u;
    }
    *ok = emit_pad(m, MENU_GAP) && (selected || emit(m, theme_sgr(THEME_DETAIL))) &&
          emit_n(m, c->detail, cut);
    return MENU_GAP + menu_display_width(c->detail);
}

/* One cell of the grid, padded to the full width. The selected cell is padded
 * before the reset so its highlight is a solid block rather than a ragged one. */
static bool emit_item(Menu *m, size_t idx, bool last_in_row)
{
    const Comp *c = comp_at(m->list, idx);
    bool selected = idx == m->sel;
    size_t avail = m->cellw;
    size_t dispw = menu_display_width(c->display);
    size_t used = dispw < avail ? dispw : avail;
    bool ok = true;

    if (selected && !emit(m, theme_sgr(THEME_SELECTED))) {
        return false;
    }
    if (!emit_name(m, c, selected, fit_bytes(c->display, avail))) {
        return false;
    }
    used += emit_detail(m, c, selected, avail - used, &ok);
    if (!ok) {
        return false;
    }
    if (used > avail) {
        used = avail;
    }
    if (!emit_pad(m, avail - used) || !emit(m, theme_sgr(THEME_RESET))) {
        return false;
    }
    return last_in_row || emit_pad(m, MENU_GAP);
}

static bool emit_header(Menu *m, CompKind kind)
{
    return emit(m, theme_sgr(THEME_GROUP)) && emit(m, comp_kind_label(kind)) &&
           emit(m, theme_sgr(THEME_RESET));
}

static bool emit_note(Menu *m, size_t hidden)
{
    char buf[96];
    const char *trunc = comp_truncated(m->list) ? " (list truncated)" : "";

    if (hidden > 0u) {
        (void)snprintf(buf, sizeof(buf), "%lu more%s", (unsigned long)hidden, trunc);
    } else {
        (void)snprintf(buf, sizeof(buf), "%lu candidates%s", (unsigned long)comp_count(m->list),
                       trunc);
    }
    return emit(m, "\r\n") && emit(m, theme_sgr(THEME_NOTE)) && emit(m, buf) &&
           emit(m, theme_sgr(THEME_RESET)) && emit(m, "\x1b[0K");
}

/* One drawn line, already positioned by the caller's "\r\n". */
static bool emit_line(Menu *m, const MenuLine *ml)
{
    size_t j;

    if (ml->header) {
        return emit_header(m, ml->kind);
    }
    for (j = 0u; j < ml->count; j++) {
        if (!emit_item(m, ml->first + j, j + 1u == ml->count)) {
            return false;
        }
    }
    return true;
}

const char *menu_render(Menu *m, unsigned *rows)
{
    unsigned drawn = 0u;
    unsigned visible;
    size_t i;

    if (rows != NULL) {
        *rows = 0u;
    }
    if (!menu_active(m)) {
        return "";
    }
    ensure_layout(m);
    m->olen = 0u;
    if (m->out != NULL) {
        m->out[0] = '\0';
    }
    scroll_to(m, line_of(m, m->sel));
    visible = visible_lines(m);

    for (i = m->top; i < m->nlines && drawn < visible; i++, drawn++) {
        if (!emit(m, "\r\n") || !emit_line(m, &m->line[i]) || !emit(m, "\x1b[0K")) {
            return "";
        }
    }
    if (m->nlines > (size_t)visible || comp_truncated(m->list)) {
        if (!emit_note(m, i < m->nlines ? m->nlines - i : 0u)) {
            return "";
        }
        drawn++;
    }
    if (rows != NULL) {
        *rows = drawn;
    }
    return m->out != NULL ? m->out : "";
}
