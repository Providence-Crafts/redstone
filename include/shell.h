/*
 * shell.h - the session.
 *
 * Everything a dot command needs that outlives one line: the connection, the
 * editor, where output currently goes, and the switches (`echo`, `bail`,
 * `timer`, `stats`, `changes`, safe mode). main.c parses argv and calls
 * shell_run; dot.c is handed a Shell and needs no globals of its own.
 *
 * The output redirect is the reason this type exists. `.output FILE` has to
 * survive until `.output` with no argument, and `.once` has to survive
 * exactly one statement, so the stream cannot keep being passed down from
 * the REPL as a parameter.
 */
#ifndef SQLSH_SHELL_H
#define SQLSH_SHELL_H

#include "db.h"
#include "line.h"

#include <stdbool.h>
#include <stdio.h>

typedef struct Shell Shell;

/* IN is the script or terminal to read from; OUT and ERR are the base
 * streams, to which output returns whenever a redirect ends. */
Shell *shell_new(FILE *in, FILE *out, FILE *err);
void shell_free(Shell *sh);

/* main.c builds the connection and the editor and hands them over; the Shell
 * owns them from then on and shell_free closes both. */
void shell_adopt(Shell *sh, Db *db, Line *ln);

Db *shell_db(Shell *sh);
Line *shell_line(Shell *sh);

/* `.connection`: sqlite3(1) keeps a small fixed set of connections and one
 * current. The formatter moves with the switch, because how results look is
 * a property of the session rather than of a connection. */
#define SHELL_MAX_CONN 5
size_t shell_conn_count(const Shell *sh);
size_t shell_conn_current(const Shell *sh);
const char *shell_conn_path(Shell *sh, size_t i);
bool shell_conn_switch(Shell *sh, size_t i);
bool shell_conn_close(Shell *sh, size_t i);

/* Where results go now: the redirect when one is active, the base stream
 * otherwise. Never NULL. */
FILE *shell_out(Shell *sh);
FILE *shell_err(Shell *sh);

/* The base output stream, ignoring any redirect. `.output` needs it to know
 * what it is returning to, and `.excel` to report where it wrote. */
FILE *shell_base_out(Shell *sh);

/* Redirect results. TARGET is a file name, "|command" for a pipe, or NULL to
 * return to the base stream. ONCE ends the redirect after the next statement,
 * which is what `.once` and `.excel` are. Returns false after reporting. */
bool shell_redirect(Shell *sh, const char *target, bool once);

/* End a `.once` redirect. Called by the statement executor, not by callers. */
void shell_redirect_done(Shell *sh);

/* The name of the current redirect target, or "stdout". */
const char *shell_output_name(Shell *sh);

/* --- switches ----------------------------------------------------------- */

typedef enum {
    SHELL_ECHO = 0, /* print each statement before running it */
    SHELL_BAIL,     /* stop at the first error */
    SHELL_TIMER,    /* report elapsed time per statement */
    SHELL_STATS,    /* report memory statistics per statement */
    SHELL_CHANGES,  /* report sqlite3_changes after each statement */
    SHELL_EQP,      /* print the query plan before each statement */
    SHELL_SAFE,     /* -safe: refuse anything that touches the filesystem */
    SHELL_CRLF,     /* write CRLF line endings */
    SHELL_INTERACTIVE,
    SHELL_FLAG_COUNT
} ShellFlag;

bool shell_flag(const Shell *sh, ShellFlag flag);
void shell_set_flag(Shell *sh, ShellFlag flag, bool on);

/* `.explain auto|on|off`. AUTO turns explain formatting on only for
 * statements that begin with EXPLAIN. */
typedef enum { SHELL_EXPLAIN_OFF = 0, SHELL_EXPLAIN_ON, SHELL_EXPLAIN_AUTO } ShellExplain;
void shell_set_explain(Shell *sh, ShellExplain mode);
ShellExplain shell_explain(const Shell *sh);

/* `.prompt MAIN CONTINUE`. */
void shell_set_prompt(Shell *sh, const char *main_prompt, const char *continuation);
const char *shell_prompt(const Shell *sh, bool continuation);

/* `.log`. TARGET is a file name, "stdout", "stderr", or NULL for off. */
bool shell_set_log(Shell *sh, const char *target);
const char *shell_log_name(const Shell *sh);

/* Safe mode refuses a command rather than silently doing less. Returns true
 * and reports when NAME may not run; callers bail out on true. */
bool shell_unsafe(Shell *sh, const char *name);

/* The `-nonce` argument: the one string `.nonce` will accept to leave safe
 * mode. Unset, no nonce matches, so safe mode is final. */
void shell_set_nonce(Shell *sh, const char *nonce);
bool shell_nonce_matches(const Shell *sh, const char *text);

/* --- running ------------------------------------------------------------ */

/* Execute SQL through the current redirect, honouring echo, timer, stats,
 * changes and bail. Returns false if it failed. */
bool shell_exec(Shell *sh, const char *sql);

/* Feed one input line: a dot command at the start of a statement, otherwise
 * a line of SQL that is accumulated until complete. Used by the REPL, by
 * `.read`, and by the init files, so all three behave identically. */
bool shell_feed(Shell *sh, const char *text);

/* True once a dot command has asked to leave. */
bool shell_quitting(const Shell *sh);
void shell_quit(Shell *sh, int status);
int shell_status(const Shell *sh);

/* Read FILE line by line through shell_feed. `.read` and the init files. */
bool shell_source(Shell *sh, const char *path, bool complain);

/* The init files, in order: ~/.sqliterc then $XDG_CONFIG_HOME/sqlsh/sqlshrc,
 * so ours wins on conflict. Missing files are not an error. */
void shell_load_init(Shell *sh, const char *explicit_path);

/* The REPL. Returns the process exit status. */
int shell_run(Shell *sh);

#endif /* SQLSH_SHELL_H */
