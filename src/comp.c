/*
 * comp.c - candidate generation.
 *
 * Every source is enumerated from the linked library or read out of the
 * schema; there is no hardcoded list of keywords, functions or pragmas
 * anywhere in this program. That is deliberate. A list baked into the source
 * is wrong the day the library is upgraded, and wrong silently.
 */
#include "comp.h"

#include <stdlib.h>
#include <string.h>

struct CompList {
    Comp *item;
    size_t n;
    size_t cap;
    bool truncated;
    char prefix[SQL_NAME_MAX];
    size_t offset;
};

/* --- small helpers ------------------------------------------------------ */

static char *dup_str(const char *s)
{
    size_t n;
    char *copy;

    if (s == NULL) {
        s = "";
    }
    n = strlen(s) + 1u;
    copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static char lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)((c - 'A') + 'a');
    }
    return c;
}

static int cmp_fold(const char *a, const char *b)
{
    size_t i;

    for (i = 0u; a[i] != '\0' && b[i] != '\0'; i++) {
        char x = lower(a[i]);
        char y = lower(b[i]);

        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a[i] == b[i]) {
        return 0;
    }
    return a[i] == '\0' ? -1 : 1;
}

static bool has_prefix_fold(const char *s, const char *prefix)
{
    size_t i;

    for (i = 0u; prefix[i] != '\0'; i++) {
        if (lower(s[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

static bool all_lower(const char *s)
{
    size_t i;

    for (i = 0u; s[i] != '\0'; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            return false;
        }
    }
    return true;
}

/* --- list management ---------------------------------------------------- */

static bool list_grow(CompList *list)
{
    size_t cap = (list->cap == 0u) ? 64u : list->cap * 2u;
    Comp *item = realloc(list->item, cap * sizeof(*item));

    if (item == NULL) {
        return false;
    }
    list->item = item;
    list->cap = cap;
    return true;
}

/* Take ownership of three already-allocated strings, or free them all and
 * report failure, so a caller never has to unwind a half-built entry. */
static bool list_take(CompList *list, char *text, char *display, char *detail, CompKind kind)
{
    if (text == NULL || display == NULL || detail == NULL ||
        (list->n == list->cap && !list_grow(list))) {
        free(text);
        free(display);
        free(detail);
        list->truncated = true;
        return false;
    }
    list->item[list->n].text = text;
    list->item[list->n].display = display;
    list->item[list->n].detail = detail;
    list->item[list->n].kind = kind;
    list->n++;
    return true;
}

void comp_free(CompList *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0u; i < list->n; i++) {
        free(list->item[i].text);
        free(list->item[i].display);
        free(list->item[i].detail);
    }
    free(list->item);
    free(list);
}

size_t comp_count(const CompList *list)
{
    return list != NULL ? list->n : 0u;
}

const Comp *comp_at(const CompList *list, size_t i)
{
    if (list == NULL || i >= list->n) {
        return NULL;
    }
    return &list->item[i];
}

bool comp_truncated(const CompList *list)
{
    return list != NULL && list->truncated;
}

const char *comp_prefix(const CompList *list)
{
    return list != NULL ? list->prefix : "";
}

size_t comp_offset(const CompList *list)
{
    return list != NULL ? list->offset : 0u;
}

const char *comp_kind_label(CompKind kind)
{
    switch (kind) {
    case COMP_COLUMN:
        return "columns";
    case COMP_VALUE:
        return "values";
    case COMP_ALIAS:
        return "aliases";
    case COMP_TABLE:
        return "tables";
    case COMP_VIEW:
        return "views";
    case COMP_FUNCTION:
        return "functions";
    case COMP_PRAGMA:
        return "pragmas";
    case COMP_DOT_COMMAND:
        return "commands";
    case COMP_KEYWORD:
    default:
        return "keywords";
    }
}

/* --- quoting ------------------------------------------------------------ */

static bool ident_is_bare(const char *name)
{
    size_t i;

    if (name[0] == '\0' || (!(name[0] >= 'a' && name[0] <= 'z') &&
                            !(name[0] >= 'A' && name[0] <= 'Z') && name[0] != '_')) {
        return false;
    }
    for (i = 1u; name[i] != '\0'; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '$')) {
            return false;
        }
    }
    return true;
}

/* True when NAME would be read as a keyword rather than as an identifier, and
 * so has to be quoted even though it looks bare. This is what makes a column
 * literally named "order" usable. */
static bool ident_is_keyword(Db *db, const char *name)
{
    const DbList *kw = db_keywords(db);
    size_t i;

    for (i = 0u; i < db_list_count(kw); i++) {
        if (cmp_fold(db_list_name(kw, i), name) == 0) {
            return true;
        }
    }
    return false;
}

/* Double-quote NAME, doubling any quote inside it. Returns NULL on failure. */
static char *quote_ident(const char *name)
{
    size_t len = strlen(name);
    size_t extra = 0u;
    size_t i;
    size_t j = 0u;
    char *out;

    for (i = 0u; i < len; i++) {
        if (name[i] == '"') {
            extra++;
        }
    }
    out = malloc(len + extra + 3u);
    if (out == NULL) {
        return NULL;
    }
    out[j++] = '"';
    for (i = 0u; i < len; i++) {
        out[j++] = name[i];
        if (name[i] == '"') {
            out[j++] = '"';
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

static char *ident_text(Db *db, const char *name)
{
    if (ident_is_bare(name) && !ident_is_keyword(db, name)) {
        return dup_str(name);
    }
    return quote_ident(name);
}

/* Values go in as SQL literals. Anything that is not plainly a number is
 * quoted as a string, because guessing wrong towards "unquoted" produces a
 * syntax error while guessing wrong towards "quoted" still compares equal. */
static bool looks_numeric(const char *s)
{
    size_t i = 0u;
    bool digits = false;

    if (s[i] == '+' || s[i] == '-') {
        i++;
    }
    while (s[i] >= '0' && s[i] <= '9') {
        digits = true;
        i++;
    }
    if (s[i] == '.') {
        i++;
        while (s[i] >= '0' && s[i] <= '9') {
            digits = true;
            i++;
        }
    }
    return digits && s[i] == '\0';
}

static char *value_text(const char *value)
{
    size_t len = strlen(value);
    size_t extra = 0u;
    size_t i;
    size_t j = 0u;
    char *out;

    if (looks_numeric(value)) {
        return dup_str(value);
    }
    for (i = 0u; i < len; i++) {
        if (value[i] == '\'') {
            extra++;
        }
    }
    out = malloc(len + extra + 3u);
    if (out == NULL) {
        return NULL;
    }
    out[j++] = '\'';
    for (i = 0u; i < len; i++) {
        out[j++] = value[i];
        if (value[i] == '\'') {
            out[j++] = '\'';
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

/* --- adding candidates -------------------------------------------------- */

typedef struct {
    CompList *list;
    Db *db;
    const char *prefix;
} Gen;

static void add_ident(Gen *g, const char *name, const char *detail, CompKind kind)
{
    if (name == NULL || !has_prefix_fold(name, g->prefix)) {
        return;
    }
    (void)list_take(g->list, ident_text(g->db, name), dup_str(name),
                    dup_str(detail != NULL ? detail : ""), kind);
}

/* Keywords are the one source whose case we adapt: the library reports them
 * uppercase, but someone typing "sel" wants "select". An empty prefix keeps
 * the library's spelling. */
static void add_keyword(Gen *g, const char *name)
{
    char *text;

    if (name == NULL || !has_prefix_fold(name, g->prefix)) {
        return;
    }
    text = dup_str(name);
    if (text != NULL && g->prefix[0] != '\0' && all_lower(g->prefix)) {
        size_t i;

        for (i = 0u; text[i] != '\0'; i++) {
            text[i] = lower(text[i]);
        }
    }
    (void)list_take(g->list, text, dup_str(name), dup_str("keyword"), COMP_KEYWORD);
}

static void add_value(Gen *g, const char *value)
{
    if (value == NULL || !has_prefix_fold(value, g->prefix)) {
        return;
    }
    (void)list_take(g->list, value_text(value), dup_str(value), dup_str("value"), COMP_VALUE);
}

static void add_list(Gen *g, const DbList *src, CompKind kind)
{
    size_t i;

    for (i = 0u; i < db_list_count(src); i++) {
        add_ident(g, db_list_name(src, i), db_list_detail(src, i), kind);
    }
    if (db_list_truncated(src)) {
        g->list->truncated = true;
    }
}

/* --- sources ------------------------------------------------------------ */

static void add_keywords(Gen *g)
{
    const DbList *kw = db_keywords(g->db);
    size_t i;

    for (i = 0u; i < db_list_count(kw); i++) {
        add_keyword(g, db_list_name(kw, i));
    }
}

static void add_tables(Gen *g)
{
    const DbList *tables = db_tables(g->db);
    size_t i;

    for (i = 0u; i < db_list_count(tables); i++) {
        const char *type = db_list_detail(tables, i);
        CompKind kind = (strcmp(type, "view") == 0) ? COMP_VIEW : COMP_TABLE;

        add_ident(g, db_list_name(tables, i), type, kind);
    }
}

/* Resolve a qualifier against the in-scope tables: an alias first, then a
 * table name, so "SELECT c.<tab> FROM companies c" and
 * "SELECT companies.<tab> FROM companies" both work. Returns NULL when the
 * qualifier names nothing we know. */
static const char *resolve_qualifier(const SqlContext *ctx, const char *qualifier)
{
    size_t i;

    for (i = 0u; i < ctx->ntables; i++) {
        if (ctx->tables[i].alias[0] != '\0' && cmp_fold(ctx->tables[i].alias, qualifier) == 0) {
            return ctx->tables[i].name;
        }
    }
    for (i = 0u; i < ctx->ntables; i++) {
        if (cmp_fold(ctx->tables[i].name, qualifier) == 0) {
            return ctx->tables[i].name;
        }
    }
    return NULL;
}

/* Columns of the tables in scope, and nothing else. With no FROM clause yet
 * there is no scope, so there are no columns -- offering every column in the
 * database would be noise standing in for an answer. */
static void add_columns(Gen *g, const SqlContext *ctx)
{
    size_t i;

    if (ctx->qualifier[0] != '\0') {
        const char *table = resolve_qualifier(ctx, ctx->qualifier);

        if (table != NULL) {
            add_list(g, db_columns(g->db, table), COMP_COLUMN);
        }
        return;
    }
    for (i = 0u; i < ctx->ntables; i++) {
        add_list(g, db_columns(g->db, ctx->tables[i].name), COMP_COLUMN);
    }
    for (i = 0u; i < ctx->ntables; i++) {
        if (ctx->tables[i].alias[0] != '\0') {
            add_ident(g, ctx->tables[i].alias, ctx->tables[i].name, COMP_ALIAS);
        }
    }
}

/* Which in-scope table owns CTX->column. The first match wins; a name
 * ambiguous across a join is ambiguous to the user too. */
static const char *table_of_column(Gen *g, const SqlContext *ctx, const char *column)
{
    size_t i;

    for (i = 0u; i < ctx->ntables; i++) {
        const DbList *cols = db_columns(g->db, ctx->tables[i].name);
        size_t k;

        for (k = 0u; k < db_list_count(cols); k++) {
            if (cmp_fold(db_list_name(cols, k), column) == 0) {
                return ctx->tables[i].name;
            }
        }
    }
    return NULL;
}

static void add_values(Gen *g, const SqlContext *ctx)
{
    const char *table;
    const DbList *values;
    size_t i;

    if (ctx->column[0] == '\0') {
        return;
    }
    table = table_of_column(g, ctx, ctx->column);
    if (table == NULL) {
        return;
    }
    values = db_values(g->db, table, ctx->column);
    for (i = 0u; i < db_list_count(values); i++) {
        add_value(g, db_list_name(values, i));
    }
    if (db_list_truncated(values)) {
        g->list->truncated = true;
    }
}

static void add_dot_commands(Gen *g, const CompDotSource *dots)
{
    size_t i;

    if (dots == NULL || dots->command == NULL) {
        return;
    }
    for (i = 0u;; i++) {
        const char *name = NULL;
        const char *help = NULL;

        if (!dots->command(i, &name, &help)) {
            break;
        }
        if (name == NULL || !has_prefix_fold(name, g->prefix)) {
            continue;
        }
        (void)list_take(g->list, dup_str(name), dup_str(name), dup_str(help != NULL ? help : ""),
                        COMP_DOT_COMMAND);
    }
}

static void add_words(Gen *g, const char *const *words, size_t nwords)
{
    size_t i;

    for (i = 0u; i < nwords; i++) {
        if (words[i] != NULL && has_prefix_fold(words[i], g->prefix)) {
            (void)list_take(g->list, dup_str(words[i]), dup_str(words[i]), dup_str(""),
                            COMP_KEYWORD);
        }
    }
}

static void add_dot_arg(Gen *g, const SqlContext *ctx, const CompDotSource *dots)
{
    const char *const *words = NULL;
    size_t nwords = 0u;
    CompKind kind;

    if (dots == NULL || dots->arg_kind == NULL) {
        return;
    }
    kind = dots->arg_kind(ctx->dot_command, ctx->dot_argno, &words, &nwords);
    if (words != NULL) {
        add_words(g, words, nwords);
        return;
    }
    switch (kind) {
    case COMP_TABLE:
    case COMP_VIEW:
        add_tables(g);
        break;
    case COMP_PRAGMA:
        add_list(g, db_pragmas(g->db), COMP_PRAGMA);
        break;
    case COMP_FUNCTION:
        add_list(g, db_functions(g->db), COMP_FUNCTION);
        break;
    case COMP_COLUMN:
    case COMP_VALUE:
    case COMP_ALIAS:
    case COMP_DOT_COMMAND:
    case COMP_KEYWORD:
    default:
        break;
    }
}

/* --- ordering ----------------------------------------------------------- */

/* Most specific group first, keywords last. Within a group, alphabetical
 * ignoring case. The ordering is the menu's group order too, so the rank is
 * simply the enum. */
static int compare_comp(const void *a, const void *b)
{
    const Comp *x = (const Comp *)a;
    const Comp *y = (const Comp *)b;

    if (x->kind != y->kind) {
        return x->kind < y->kind ? -1 : 1;
    }
    return cmp_fold(x->display, y->display);
}

/* Drop adjacent duplicates: a column name shared by two joined tables is one
 * thing to type. */
static void dedupe(CompList *list)
{
    size_t out = 0u;
    size_t i;

    for (i = 0u; i < list->n; i++) {
        if (out > 0u && list->item[out - 1u].kind == list->item[i].kind &&
            strcmp(list->item[out - 1u].display, list->item[i].display) == 0) {
            free(list->item[i].text);
            free(list->item[i].display);
            free(list->item[i].detail);
            continue;
        }
        list->item[out] = list->item[i];
        out++;
    }
    list->n = out;
}

/* --- entry point -------------------------------------------------------- */

CompList *comp_generate(Db *db, const SqlContext *ctx, const CompDotSource *dots)
{
    CompList *list;
    Gen g;

    if (ctx == NULL) {
        return NULL;
    }
    list = calloc(1u, sizeof(*list));
    if (list == NULL) {
        return NULL;
    }
    memcpy(list->prefix, ctx->word, sizeof(list->prefix));
    list->prefix[SQL_NAME_MAX - 1u] = '\0';
    list->offset = ctx->word_off;

    g.list = list;
    g.db = db;
    g.prefix = list->prefix;

    switch (ctx->kind) {
    case CTX_DOT_COMMAND:
        add_dot_commands(&g, dots);
        break;
    case CTX_DOT_ARG:
        add_dot_arg(&g, ctx, dots);
        break;
    case CTX_STATEMENT_START:
        add_dot_commands(&g, dots);
        add_keywords(&g);
        break;
    case CTX_SELECT_LIST:
        add_columns(&g, ctx);
        add_list(&g, db_functions(db), COMP_FUNCTION);
        add_keywords(&g);
        break;
    case CTX_TABLE:
        add_tables(&g);
        break;
    case CTX_COLUMN:
        add_columns(&g, ctx);
        add_list(&g, db_functions(db), COMP_FUNCTION);
        break;
    case CTX_VALUE:
        add_values(&g, ctx);
        break;
    case CTX_FUNCTION:
        add_columns(&g, ctx);
        add_list(&g, db_functions(db), COMP_FUNCTION);
        break;
    case CTX_PRAGMA:
        add_list(&g, db_pragmas(db), COMP_PRAGMA);
        break;
    case CTX_KEYWORD:
        add_keywords(&g);
        break;
    case CTX_UNKNOWN:
    default:
        break;
    }

    if (list->n > 1u) {
        qsort(list->item, list->n, sizeof(*list->item), compare_comp);
        dedupe(list);
    }
    return list;
}
