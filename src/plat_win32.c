#ifdef _WIN32

#include "plat.h"

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

/* Windows 10 1809 and later: the console understands the same VT sequences the
 * program already emits once VT processing is on, so the editor, the menu and
 * the banner need no Windows-specific drawing. */

static HANDLE handle_of(FILE *stream)
{
    int fd = stream != NULL ? _fileno(stream) : -1;

    return fd >= 0 ? (HANDLE)_get_osfhandle(fd) : INVALID_HANDLE_VALUE;
}

static bool is_console(HANDLE h)
{
    DWORD mode;

    return h != INVALID_HANDLE_VALUE && h != NULL && GetConsoleMode(h, &mode) != 0;
}

/* ------------------------------------------------------------------------
 * Start-up
 * ---------------------------------------------------------------------- */

static UINT g_old_in_cp;
static UINT g_old_out_cp;

static void restore_code_pages(void)
{
    if (g_old_in_cp != 0u) {
        (void)SetConsoleCP(g_old_in_cp);
    }
    if (g_old_out_cp != 0u) {
        (void)SetConsoleOutputCP(g_old_out_cp);
    }
}

static void enable_vt_output(FILE *stream)
{
    HANDLE h = handle_of(stream);
    DWORD mode;

    if (is_console(h) && GetConsoleMode(h, &mode) != 0) {
        (void)SetConsoleMode(h,
                             mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

/* The CRT fills main()'s argv in the ANSI code page, so a non-ASCII database
 * path or SQL argument arrives already mangled. Rebuild it from the wide
 * command line as UTF-8. One contiguous block holds the pointer array and the
 * strings, so the whole thing frees at exit as a unit (and leaks nothing if a
 * step fails: the original argv stays in use). */
static char *g_utf8_argv_block;

static void free_utf8_argv(void)
{
    free(g_utf8_argv_block);
    g_utf8_argv_block = NULL;
}

static void adopt_utf8_argv(int *argc, char ***argv)
{
    LPWSTR *wide;
    int n = 0;
    int i;
    size_t bytes = 0u;
    size_t offset;
    char **out;
    char *strings;

    wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wide == NULL || n <= 0) {
        return;
    }
    /* Sum the UTF-8 sizes first so the block can be one allocation. */
    for (i = 0; i < n; i++) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);

        if (need <= 0) {
            LocalFree(wide);
            return;
        }
        bytes += (size_t)need;
    }
    offset = sizeof(char *) * ((size_t)n + 1u);
    out = (char **)malloc(offset + bytes);
    if (out == NULL) {
        LocalFree(wide);
        return;
    }
    strings = (char *)out + offset;
    for (i = 0; i < n; i++) {
        int wrote = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, strings, (int)bytes, NULL, NULL);

        out[i] = strings;
        strings += wrote;
        bytes -= (size_t)wrote;
    }
    out[n] = NULL;
    LocalFree(wide);

    g_utf8_argv_block = (char *)out;
    (void)atexit(free_utf8_argv);
    *argc = n;
    *argv = out;
}

void plat_init(int *argc, char ***argv)
{
    g_old_in_cp = GetConsoleCP();
    g_old_out_cp = GetConsoleOutputCP();
    (void)SetConsoleCP(CP_UTF8);
    (void)SetConsoleOutputCP(CP_UTF8);
    (void)atexit(restore_code_pages);
    enable_vt_output(stdout);
    enable_vt_output(stderr);
    /* Text mode would turn every "\n" into "\r\n" in files and pipes, and
     * --compat promises sqlite3's bytes. */
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    adopt_utf8_argv(argc, argv);
}

/* ------------------------------------------------------------------------
 * Terminal
 * ---------------------------------------------------------------------- */

bool plat_isatty(FILE *stream)
{
    return is_console(handle_of(stream));
}

static bool buffer_size(HANDLE h, unsigned *cols, unsigned *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    if (!is_console(h) || GetConsoleScreenBufferInfo(h, &info) == 0) {
        return false;
    }
    *cols = (unsigned)(info.srWindow.Right - info.srWindow.Left + 1);
    *rows = (unsigned)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return *cols > 0u;
}

/* An input stream has no size of its own: ask the output side of the same
 * console. */
bool plat_term_size(FILE *stream, unsigned *cols, unsigned *rows)
{
    unsigned c;
    unsigned r;

    if (!buffer_size(handle_of(stream), &c, &r) && !buffer_size(handle_of(stdout), &c, &r) &&
        !buffer_size(handle_of(stderr), &c, &r)) {
        return false;
    }
    *cols = c;
    if (rows != NULL) {
        *rows = r;
    }
    return true;
}

bool plat_utf8_default(void)
{
    return true; /* plat_init switched the console to UTF-8 */
}

bool plat_truecolor_default(void)
{
    return true; /* conhost and Windows Terminal both draw 24-bit colour */
}

static HANDLE g_in = INVALID_HANDLE_VALUE;
static DWORD g_saved_mode;
static bool g_raw = false;
static bool g_hooked = false;
static unsigned g_cols;
static unsigned g_rows;
static unsigned char g_queue[8];
static size_t g_qlen;
static size_t g_qpos;

static BOOL WINAPI on_ctrl(DWORD event)
{
    if (g_raw) {
        (void)SetConsoleMode(g_in, g_saved_mode);
    }
    (void)event;
    return FALSE; /* default handling goes on to end the process */
}

bool plat_raw_enter(FILE *in)
{
    HANDLE h = handle_of(in);

    if (g_raw) {
        return true;
    }
    if (!is_console(h) || GetConsoleMode(h, &g_saved_mode) == 0) {
        return false;
    }
    /* No line input, echo or processed input: Ctrl-C arrives as a byte, and the
     * keys arrive as the same VT sequences a POSIX terminal sends. */
    if (SetConsoleMode(h, ENABLE_VIRTUAL_TERMINAL_INPUT) == 0) {
        return false;
    }
    if (!g_hooked) {
        g_hooked = true;
        (void)atexit(plat_raw_leave);
        (void)SetConsoleCtrlHandler(on_ctrl, TRUE);
    }
    g_in = h;
    g_raw = true;
    g_qlen = 0u;
    g_qpos = 0u;
    (void)plat_term_size(in, &g_cols, &g_rows);
    return true;
}

void plat_raw_leave(void)
{
    if (!g_raw) {
        return;
    }
    (void)SetConsoleMode(g_in, g_saved_mode);
    g_raw = false;
}

/* No SIGWINCH here: compare the size each time the editor asks. */
bool plat_resized(void)
{
    unsigned c;
    unsigned r;

    if (!plat_term_size(stdin, &c, &r) || (c == g_cols && r == g_rows)) {
        return false;
    }
    g_cols = c;
    g_rows = r;
    return true;
}

static void queue_utf8(unsigned long cp)
{
    g_qlen = 0u;
    g_qpos = 0u;
    if (cp < 0x80ul) {
        g_queue[g_qlen++] = (unsigned char)cp;
    } else if (cp < 0x800ul) {
        g_queue[g_qlen++] = (unsigned char)(0xC0ul | (cp >> 6));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | (cp & 0x3Ful));
    } else if (cp < 0x10000ul) {
        g_queue[g_qlen++] = (unsigned char)(0xE0ul | (cp >> 12));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | ((cp >> 6) & 0x3Ful));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | (cp & 0x3Ful));
    } else {
        g_queue[g_qlen++] = (unsigned char)(0xF0ul | (cp >> 18));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | ((cp >> 12) & 0x3Ful));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | ((cp >> 6) & 0x3Ful));
        g_queue[g_qlen++] = (unsigned char)(0x80ul | (cp & 0x3Ful));
    }
}

static bool read_wide(HANDLE h, WCHAR *out)
{
    DWORD n = 0;

    return ReadConsoleW(h, out, 1, &n, NULL) != 0 && n == 1;
}

/* The console hands out UTF-16; the editor wants UTF-8 bytes, as on POSIX. */
int plat_read_byte(FILE *in, unsigned char *byte)
{
    HANDLE h = handle_of(in);
    WCHAR w;
    unsigned long cp;

    if (g_qpos < g_qlen) {
        *byte = g_queue[g_qpos++];
        return 1;
    }
    if (!is_console(h)) {
        int n = _read(_fileno(in), byte, 1u);

        return n < 0 ? -1 : n;
    }
    if (!read_wide(h, &w)) {
        return -1;
    }
    cp = w;
    if (w >= 0xD800 && w <= 0xDBFF) {
        WCHAR low;

        if (read_wide(h, &low) && low >= 0xDC00 && low <= 0xDFFF) {
            cp = 0x10000ul + (((unsigned long)(w - 0xD800) << 10) | (unsigned long)(low - 0xDC00));
        }
    }
    queue_utf8(cp);
    *byte = g_queue[g_qpos++];
    return 1;
}

/* The console input handle is also signalled by key releases and focus events,
 * so a wake-up is only believed when a key press is actually queued. */
bool plat_input_ready(FILE *in, int ms)
{
    HANDLE h = handle_of(in);
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)ms;

    if (g_qpos < g_qlen) {
        return true;
    }
    for (;;) {
        INPUT_RECORD rec[32];
        DWORD n = 0;
        DWORD i;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline || WaitForSingleObject(h, (DWORD)(deadline - now)) != WAIT_OBJECT_0) {
            return false;
        }
        if (PeekConsoleInputW(h, rec, 32, &n) == 0) {
            return false;
        }
        for (i = 0; i < n; i++) {
            if (rec[i].EventType == KEY_EVENT && rec[i].Event.KeyEvent.bKeyDown &&
                rec[i].Event.KeyEvent.uChar.UnicodeChar != 0) {
                return true;
            }
        }
        (void)FlushConsoleInputBuffer(h);
    }
}

/* ------------------------------------------------------------------------
 * Files and paths
 * ---------------------------------------------------------------------- */

static char *dup_slashes(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *copy = (char *)malloc(n);
    size_t i;

    if (copy == NULL) {
        return NULL;
    }
    for (i = 0u; i < n; i++) {
        copy[i] = s[i] == '\\' ? '/' : s[i];
    }
    return copy;
}

static const char *env_nonempty(const char *name)
{
    const char *v = getenv(name);

    return (v != NULL && v[0] != '\0') ? v : NULL;
}

static const char *first_env(const char *a, const char *b)
{
    const char *v = env_nonempty(a);

    return v != NULL ? v : env_nonempty(b);
}

/* HOME if a Unix-flavoured shell set it, else the profile directory. */
char *plat_home_dir(void)
{
    const char *home = first_env("HOME", "USERPROFILE");

    return home != NULL ? dup_slashes(home) : NULL;
}

/* %APPDATA% unless XDG_CONFIG_HOME says otherwise. */
char *plat_config_dir(void)
{
    const char *dir = first_env("XDG_CONFIG_HOME", "APPDATA");

    return dir != NULL ? dup_slashes(dir) : NULL;
}

char *plat_state_dir(void)
{
    const char *dir = first_env("XDG_STATE_HOME", "LOCALAPPDATA");

    return dir != NULL ? dup_slashes(dir) : NULL;
}

const char *plat_temp_dir(void)
{
    static char dir[1024];
    const char *env = env_nonempty("TMPDIR");
    size_t n;
    size_t i;

    if (env == NULL) {
        env = first_env("TEMP", "TMP");
    }
    if (env == NULL || strlen(env) >= sizeof(dir)) {
        return ".";
    }
    n = strlen(env);
    for (i = 0u; i <= n; i++) {
        dir[i] = env[i] == '\\' ? '/' : env[i];
    }
    while (n > 1u && dir[n - 1u] == '/') {
        dir[--n] = '\0';
    }
    return dir;
}

bool plat_mkdir(const char *path)
{
    return _mkdir(path) == 0;
}

static bool is_dir(const char *path)
{
    DWORD attr = GetFileAttributesA(path);

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0u;
}

/* Either separator counts; a drive prefix ("C:") is never created. */
bool plat_mkdir_p(const char *path)
{
    size_t n = strlen(path);
    char *copy = (char *)malloc(n + 1u);
    size_t i;
    bool ok;

    if (copy == NULL) {
        return false;
    }
    memcpy(copy, path, n + 1u);
    for (i = 1u; i < n; i++) {
        if ((copy[i] == '/' || copy[i] == '\\') && copy[i - 1u] != ':') {
            char sep = copy[i];

            copy[i] = '\0';
            (void)_mkdir(copy);
            copy[i] = sep;
        }
    }
    (void)_mkdir(copy);
    ok = is_dir(copy);
    free(copy);
    return ok;
}

bool plat_chdir(const char *path)
{
    return _chdir(path) == 0;
}

bool plat_temp_file(char *path, size_t size, const char *prefix)
{
    unsigned attempt;

    for (attempt = 0u; attempt < 100u; attempt++) {
        int fd;

        if ((size_t)snprintf(path, size, "%s/%s-%lu-%u.tmp", plat_temp_dir(), prefix, plat_pid(),
                             attempt) >= size) {
            return false;
        }
        fd = _open(path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            (void)_close(fd);
            return true;
        }
    }
    return false;
}

unsigned long plat_pid(void)
{
    return (unsigned long)GetCurrentProcessId();
}

/* ------------------------------------------------------------------------
 * Processes and clocks
 * ---------------------------------------------------------------------- */

FILE *plat_popen_write(const char *cmd)
{
    /* NOLINTNEXTLINE(cert-env33-c) */
    return _popen(cmd, "wb");
}

int plat_pclose(FILE *stream)
{
    return _pclose(stream);
}

const char *plat_default_editor(void)
{
    const char *cmd = first_env("VISUAL", "EDITOR");

    return cmd != NULL ? cmd : "notepad";
}

int plat_open_file(const char *path, char *cmd, size_t size)
{
    int n = snprintf(cmd, size, "start \"\" \"%s\"", path);

    if (n <= 0 || (size_t)n >= size) {
        return -1;
    }
    /* NOLINTNEXTLINE(cert-env33-c) */
    return system(cmd);
}

static double filetime_seconds(FILETIME ft)
{
    unsigned long long ticks =
        ((unsigned long long)ft.dwHighDateTime << 32) | (unsigned long long)ft.dwLowDateTime;

    return (double)ticks / 1.0e7; /* 100 ns ticks */
}

void plat_clock(PlatClock *c)
{
    FILETIME now;
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;

    GetSystemTimeAsFileTime(&now);
    c->wall = filetime_seconds(now);
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0) {
        c->user = filetime_seconds(user);
        c->sys = filetime_seconds(kernel);
    } else {
        c->user = 0.0;
        c->sys = 0.0;
    }
}

int plat_strcasecmp(const char *lhs, const char *rhs)
{
    return _stricmp(lhs, rhs);
}

int plat_strncasecmp(const char *lhs, const char *rhs, size_t count)
{
    return _strnicmp(lhs, rhs, count);
}

unsigned plat_nprocs(void)
{
    SYSTEM_INFO info;

    GetSystemInfo(&info);
    return info.dwNumberOfProcessors > 0u ? (unsigned)info.dwNumberOfProcessors : 1u;
}

bool plat_monotonic_ms(long long *ms)
{
    *ms = (long long)GetTickCount64();
    return true;
}

int plat_setenv(const char *name, const char *value)
{
    return _putenv_s(name, value);
}

int plat_unsetenv(const char *name)
{
    return _putenv_s(name, ""); /* an empty value removes the variable */
}

#else
typedef int plat_win32_not_built;
#endif
