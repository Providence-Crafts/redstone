/*
 * dot.h - the dot-command table.
 *
 * One table, one dispatch. Every command sqlite3(1) documents appears in it:
 * the 54 sqlsh implements and the 11 it refuses because they are backed by
 * extension sources this project deliberately does not vendor. A refusal is
 * an entry like any other, so `.help` can list it and completion can offer
 * it — the gap is documented rather than discovered.
 */
#ifndef SQLSH_DOT_H
#define SQLSH_DOT_H

#include "comp.h"
#include "shell.h"

#include <stdbool.h>

/* Implementations live in dot.c, schema.c and import.c. ARGV[0] is the
 * command name without its dot; ARGC counts it. */
typedef bool (*DotFn)(Shell *sh, int argc, char **argv);

/* Run the dot command in LINE, which must start with '.' after leading
 * whitespace. Returns false if it failed, which `.bail` and non-interactive
 * mode turn into an exit. */
bool dot_run(Shell *sh, const char *line);

/* The command table as completion needs it. */
const CompDotSource *dot_comp_source(void);

/* Split LINE into arguments in place, honouring single and double quotes and
 * backslash escapes, the way upstream's parser does. Returns the count.
 * Exposed because `.read`, the init files and the tests all need it. */
int dot_split(char *line, char **argv, int max);

/* Interpret an on/off/yes/no/true/false/0/1 argument. Unrecognised text is
 * reported to the shell's error stream and treated as false, which is what
 * sqlite3(1) does. */
bool dot_boolean(Shell *sh, const char *text);

#endif /* SQLSH_DOT_H */
