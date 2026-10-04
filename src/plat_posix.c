#ifndef _WIN32

#include "plat.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* NOLINTNEXTLINE(readability-non-const-parameter): plat_win32.c writes through both */
void plat_init(int *argc, char ***argv)
{
    (void)argc;
    (void)argv; /* POSIX argv is already the bytes the shell passed. */
}

bool plat_isatty(FILE *stream)
{
    int fd = stream != NULL ? fileno(stream) : -1;

    return fd >= 0 && isatty(fd) == 1;
}

bool plat_term_size(FILE *stream, unsigned *cols, unsigned *rows)
{
    struct winsize ws;
    int fd = stream != NULL ? fileno(stream) : -1;

    if (fd < 0 || ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        return false;
    }
    *cols = ws.ws_col;
    if (rows != NULL) {
        *rows = ws.ws_row;
    }
    return true;
}

bool plat_utf8_default(void)
{
    return false;
}

bool plat_truecolor_default(void)
{
    return false;
}

/* ------------------------------------------------------------------------
 * Raw mode
 *
 * The one live terminal lives here, as statics, so the atexit hook and the
 * signal handlers can restore it. Only the two things a handler touches are
 * kept as sig_atomic_t/plain scalars: a handler must not chase pointers.
 * ---------------------------------------------------------------------- */

static volatile sig_atomic_t g_raw_fd = -1;
static struct termios g_saved;
static volatile sig_atomic_t g_winch = 0;
static bool g_hooked = false;

/* Async-signal-safe: tcsetattr and _exit are both on the POSIX list. The
 * terminal is restored first, then the default disposition re-raises the signal
 * so the process dies with the right status. */
static void restore_on_signal(int sig)
{
    if (g_raw_fd >= 0) {
        (void)tcsetattr((int)g_raw_fd, TCSADRAIN, &g_saved);
        g_raw_fd = -1;
    }
    (void)signal(sig, SIG_DFL);
    (void)raise(sig);
}

static void note_winch(int sig)
{
    (void)sig;
    g_winch = 1;
}

/* SIGINT is deliberately absent: raw mode disables ISIG, so Ctrl-C reaches the
 * editor as a byte. These are the signals that would otherwise kill the process
 * with the terminal still in raw mode. */
static void install_hooks(void)
{
    static const int fatal[] = {SIGTERM, SIGQUIT, SIGHUP, SIGSEGV, SIGABRT};
    size_t i;

    if (g_hooked) {
        return;
    }
    g_hooked = true;
    (void)atexit(plat_raw_leave);
    for (i = 0u; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        (void)signal(fatal[i], restore_on_signal);
    }
    (void)signal(SIGWINCH, note_winch);
}

bool plat_raw_enter(FILE *in)
{
    struct termios raw;
    int fd = in != NULL ? fileno(in) : -1;

    if (g_raw_fd >= 0) {
        return true;
    }
    if (fd < 0 || tcgetattr(fd, &g_saved) != 0) {
        return false;
    }
    raw = g_saved;
    /* No canonical input, no echo, no signal or flow-control interception: the
     * editor sees every byte, including Ctrl-C and Ctrl-S, and decides. */
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= (tcflag_t)CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    /* TCSADRAIN, not TCSAFLUSH: the latter discards whatever the user has
     * already typed, which loses pasted input and type-ahead. */
    if (tcsetattr(fd, TCSADRAIN, &raw) != 0) {
        return false;
    }
    install_hooks();
    g_raw_fd = fd;
    return true;
}

void plat_raw_leave(void)
{
    if (g_raw_fd < 0) {
        return;
    }
    (void)tcsetattr((int)g_raw_fd, TCSADRAIN, &g_saved);
    g_raw_fd = -1;
}

bool plat_resized(void)
{
    if (g_winch == 0) {
        return false;
    }
    g_winch = 0;
    return true;
}

int plat_read_byte(FILE *in, unsigned char *byte)
{
    int fd = fileno(in);

    for (;;) {
        ssize_t n = read(fd, byte, 1u);

        if (n >= 0) {
            return (int)n;
        }
        if (errno != EINTR) {
            return -1; /* a resize or a caught signal is not an error */
        }
    }
}

/* EINTR is treated as "keep waiting" so a window resize does not turn an arrow
 * key into an Esc. */
bool plat_input_ready(FILE *in, int ms)
{
    struct pollfd pfd;
    int ready;

    pfd.fd = fileno(in);
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        ready = poll(&pfd, (nfds_t)1, ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

/* ------------------------------------------------------------------------
 * Files and paths
 * ---------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *copy = (char *)malloc(n);

    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static const char *env_nonempty(const char *name)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : NULL;
}

static char *join(const char *dir, const char *rest)
{
    size_t need = strlen(dir) + strlen(rest) + 2u;
    char *out = (char *)malloc(need);

    if (out != NULL) {
        (void)snprintf(out, need, "%s/%s", dir, rest);
    }
    return out;
}

char *plat_home_dir(void)
{
    const char *home = env_nonempty("HOME");

    return home != NULL ? dup_str(home) : NULL;
}

char *plat_config_dir(void)
{
    const char *xdg = env_nonempty("XDG_CONFIG_HOME");
    const char *home = env_nonempty("HOME");

    if (xdg != NULL) {
        return dup_str(xdg);
    }
    return home != NULL ? join(home, ".config") : NULL;
}

char *plat_state_dir(void)
{
    const char *xdg = env_nonempty("XDG_STATE_HOME");
    const char *home = env_nonempty("HOME");

    if (xdg != NULL) {
        return dup_str(xdg);
    }
    return home != NULL ? join(home, ".local/state") : NULL;
}

const char *plat_temp_dir(void)
{
    const char *dir = env_nonempty("TMPDIR");

    return dir != NULL ? dir : "/tmp";
}

bool plat_mkdir(const char *path)
{
    return mkdir(path, 0700) == 0;
}

bool plat_mkdir_p(const char *path)
{
    size_t n = strlen(path);
    char *copy = (char *)malloc(n + 1u);
    struct stat st;
    size_t i;
    bool ok;

    if (copy == NULL) {
        return false;
    }
    memcpy(copy, path, n + 1u);
    for (i = 1u; i < n; i++) {
        if (copy[i] == '/') {
            copy[i] = '\0';
            (void)mkdir(copy, 0700);
            copy[i] = '/';
        }
    }
    (void)mkdir(copy, 0700);
    ok = stat(copy, &st) == 0 && S_ISDIR(st.st_mode);
    free(copy);
    return ok;
}

bool plat_chdir(const char *path)
{
    return chdir(path) == 0;
}

bool plat_temp_file(char *path, size_t size, const char *prefix)
{
    int fd;

    if ((size_t)snprintf(path, size, "%s/%s-XXXXXX", plat_temp_dir(), prefix) >= size) {
        return false;
    }
    fd = mkstemp(path);
    if (fd < 0) {
        return false;
    }
    (void)close(fd);
    return true;
}

unsigned long plat_pid(void)
{
    return (unsigned long)getpid();
}

/* ------------------------------------------------------------------------
 * Processes and clocks
 * ---------------------------------------------------------------------- */

FILE *plat_popen_write(const char *cmd)
{
    /* ".output |CMD" is a documented sqlite3(1) feature; shell_unsafe rejects
     * it under -safe. */
    /* NOLINTNEXTLINE(cert-env33-c) */
    return popen(cmd, "w");
}

int plat_pclose(FILE *stream)
{
    return pclose(stream);
}

const char *plat_default_editor(void)
{
    const char *cmd = env_nonempty("VISUAL");

    if (cmd == NULL) {
        cmd = env_nonempty("EDITOR");
    }
    return cmd != NULL ? cmd : "vi";
}

int plat_open_file(const char *path, char *cmd, size_t size)
{
    int n = snprintf(cmd, size, "xdg-open '%s'", path);

    if (n <= 0 || (size_t)n >= size) {
        return -1;
    }
    /* .excel and .www exist to hand the file to the desktop's opener; -safe
     * refuses both before a temporary file is even named. The path is one we
     * built ourselves under the temp directory, not user text. */
    /* NOLINTNEXTLINE(cert-env33-c,clang-analyzer-optin.taint.GenericTaint) */
    return system(cmd);
}

static double seconds(const struct timeval *tv)
{
    return (double)tv->tv_sec + ((double)tv->tv_usec / 1.0e6);
}

void plat_clock(PlatClock *c)
{
    struct timeval now;
    struct rusage ru;

    (void)gettimeofday(&now, NULL);
    c->wall = seconds(&now);
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        c->user = seconds(&ru.ru_utime);
        c->sys = seconds(&ru.ru_stime);
    } else {
        c->user = 0.0;
        c->sys = 0.0;
    }
}

int plat_strcasecmp(const char *lhs, const char *rhs)
{
    return strcasecmp(lhs, rhs);
}

int plat_strncasecmp(const char *lhs, const char *rhs, size_t count)
{
    return strncasecmp(lhs, rhs, count);
}

unsigned plat_nprocs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? (unsigned)n : 1u;
}

bool plat_monotonic_ms(long long *ms)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return false;
    }
    *ms = ((long long)now.tv_sec * 1000LL) + ((long long)now.tv_nsec / 1000000LL);
    return true;
}

int plat_setenv(const char *name, const char *value)
{
    return setenv(name, value, 1);
}

int plat_unsetenv(const char *name)
{
    return unsetenv(name);
}

#else
typedef int plat_posix_not_built;
#endif
