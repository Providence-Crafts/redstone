#ifndef REDSTONE_PLAT_H
#define REDSTONE_PLAT_H

/* The platform seam. Everything that differs between POSIX and Windows lives
 * behind these functions: the terminal, paths, child processes, clocks. The
 * rest of the program is plain C99 and includes no system header beyond this
 * one. plat_posix.c and plat_win32.c each guard themselves with _WIN32, so the
 * build compiles both files everywhere and exactly one has a body.
 *
 * Paths use '/' on every platform; Windows accepts it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Once, first thing in main, with main's own ARGC/ARGV. POSIX: nothing.
 * Windows: UTF-8 code pages, VT processing on stdout/stderr, binary stdio so
 * output stays byte-exact, and *ARGV is replaced with a UTF-8 copy rebuilt from
 * the wide command line (the C runtime hands main() argv in the ANSI code page,
 * which loses non-ASCII paths and SQL). The replacement lives until exit. */
void plat_init(int *argc, char ***argv);

/* ------------------------------------------------------------------------
 * Terminal
 * ---------------------------------------------------------------------- */

bool plat_isatty(FILE *stream);

/* Size of the terminal attached to F. False when F is not a terminal or the
 * size is unknown. ROWS may be NULL. */
bool plat_term_size(FILE *stream, unsigned *cols, unsigned *rows);

/* Defaults for the capabilities POSIX learns from the environment (LANG,
 * COLORTERM) and Windows 10 console hosts simply have. */
bool plat_utf8_default(void);
bool plat_truecolor_default(void);

/* Raw mode on the terminal behind IN: no echo, no line buffering, no signal or
 * flow-control keys; every key arrives as bytes (UTF-8, VT sequences). One
 * terminal at a time. The terminal is restored on plat_raw_leave, at exit, and
 * on fatal signals. */
bool plat_raw_enter(FILE *in);
void plat_raw_leave(void);

/* True once after each terminal resize. */
bool plat_resized(void);

/* One byte from IN. 1 on success, 0 at EOF, -1 on error. Interrupted reads are
 * retried. */
int plat_read_byte(FILE *in, unsigned char *byte);

/* True when a byte arrives from IN within MS milliseconds. */
bool plat_input_ready(FILE *in, int ms);

/* ------------------------------------------------------------------------
 * Files and paths. Returned strings are malloc'd; NULL when unknown.
 * ---------------------------------------------------------------------- */

char *plat_home_dir(void);   /* $HOME, %USERPROFILE% */
char *plat_config_dir(void); /* $XDG_CONFIG_HOME or ~/.config, %APPDATA% */
char *plat_state_dir(void);  /* $XDG_STATE_HOME or ~/.local/state, %LOCALAPPDATA% */
const char *plat_temp_dir(void);

bool plat_mkdir(const char *path); /* one level; false if it cannot be made */
/* PATH and every missing parent; true when PATH is a directory afterwards. */
bool plat_mkdir_p(const char *path);
bool plat_chdir(const char *path);

/* Creates an empty, uniquely named file under the temp dir and writes its path
 * into PATH. */
bool plat_temp_file(char *path, size_t size, const char *prefix);

unsigned long plat_pid(void);

/* ------------------------------------------------------------------------
 * Processes
 * ---------------------------------------------------------------------- */

FILE *plat_popen_write(const char *cmd);
int plat_pclose(FILE *stream);

/* $VISUAL, $EDITOR, then the platform's own: vi, notepad. */
const char *plat_default_editor(void);

/* Hands PATH to the desktop's opener (xdg-open, start). Returns the command
 * processor's status; CMD receives the command line for error messages. */
int plat_open_file(const char *path, char *cmd, size_t size);

typedef struct {
    double wall;
    double user;
    double sys;
} PlatClock;

void plat_clock(PlatClock *c);

/* ASCII case-insensitive compare: POSIX strcasecmp, Windows _stricmp. */
int plat_strcasecmp(const char *lhs, const char *rhs);
int plat_strncasecmp(const char *lhs, const char *rhs, size_t count);

/* Online CPUs, at least 1. */
unsigned plat_nprocs(void);

/* Milliseconds on a monotonic clock, for measuring intervals. False when the
 * platform has none. */
bool plat_monotonic_ms(long long *ms);

/* ------------------------------------------------------------------------
 * Environment writes, for tests: C99 has getenv but no setenv.
 * ---------------------------------------------------------------------- */

int plat_setenv(const char *name, const char *value);
int plat_unsetenv(const char *name);

#endif
