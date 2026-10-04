/*
 * comp.h - context in, candidate list out.
 *
 * This is where a SqlContext meets a database. Scoping is the whole point:
 * `SELECT * FROM companies WHERE <tab>` must offer the columns of companies
 * and nothing else. A list that includes every column in the database is not
 * a slightly worse answer, it is a useless one.
 *
 * Nothing here knows about terminals. Phase 4's menu renders what this
 * produces; the split means candidate generation is testable against a fixture
 * database with no pty in sight.
 */
#ifndef REDSTONE_COMP_H
#define REDSTONE_COMP_H

#include "db.h"
#include "sqlctx.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COMP_COLUMN = 0,
    COMP_VALUE,
    COMP_ALIAS,
    COMP_TABLE,
    COMP_VIEW,
    COMP_FUNCTION,
    COMP_PRAGMA,
    COMP_DOT_COMMAND,
    COMP_KEYWORD
} CompKind;

typedef struct {
    /* What replaces the prefix in the buffer: quoted when the identifier needs
     * it, so inserting a column named "order" or "total amount" produces
     * something that parses. */
    char *text;
    /* What the menu shows: never quoted, so the user reads the name itself. */
    char *display;
    /* A short description beside it: declared type, "table", "view",
     * "function", "pragma". Never NULL. */
    char *detail;
    CompKind kind;
} Comp;

typedef struct CompList CompList;

/* Phase 6's dot.c owns the dot-command table. comp.c asks for it through this
 * interface rather than keeping a second copy that could quietly drift out of
 * step with the real dispatch table. NULL means no dot-command candidates,
 * which is what the earlier phases pass. */
typedef struct {
    /* Enumerate commands, without the leading dot. False past the end. */
    bool (*command)(size_t i, const char **name, const char **help);
    /* What argument ARGNO (1-based) of command NAME completes to. Returning
     * COMP_KEYWORD with *words set offers that fixed word list instead of the
     * library's keywords; *nwords is then its length. */
    CompKind (*arg_kind)(const char *name, size_t argno, const char *const **words, size_t *nwords);
} CompDotSource;

/* Candidates for CTX. DB may be NULL, in which case only the context-free
 * sources appear. DOTS may be NULL. Returns NULL only on allocation failure;
 * "nothing to offer" is an empty list. */
CompList *comp_generate(Db *db, const SqlContext *ctx, const CompDotSource *dots);

void comp_free(CompList *list);

size_t comp_count(const CompList *list);
const Comp *comp_at(const CompList *list, size_t i);

/* True when a cap or a time limit cut the list short. The menu says so
 * explicitly rather than presenting a partial list as complete. */
bool comp_truncated(const CompList *list);

/* The prefix these candidates replace and where it starts in the buffer. */
const char *comp_prefix(const CompList *list);
size_t comp_offset(const CompList *list);

/* Group heading for the menu. */
const char *comp_kind_label(CompKind kind);

#endif /* REDSTONE_COMP_H */
