# Windows support: assessment and options

Status: implemented (option A). Decisions taken: platform layer; Windows 10 1809+; MinGW-w64 (cross from Nix locally, native MSYS2 on GitHub Actions); zip plus winget manifest; GitHub Actions CI and release. The Win32 side passes the unit tests under Wine; the `.edit` tests are POSIX-only (they use a `/bin/sh` editor stub), and the interactive console on real Windows remains a manual check. The assessment below is kept as written.

Requirement: redstone installs and works out of the box on Linux and Windows.

## Where the code depends on POSIX today

Read from `src/`. Line counts are of the whole file.

| File | Lines | POSIX use | Windows equivalent |
|---|---|---|---|
| `line.c` | 994 | `termios` raw mode, `poll` on stdin, `ioctl(TIOCGWINSZ)`, `SIGWINCH`, `sigaction`/`signal` to restore the terminal, `isatty`, `system()` to run `$EDITOR`, temp files, `ssize_t` | `SetConsoleMode` (raw input, `ENABLE_VIRTUAL_TERMINAL_INPUT`/`PROCESSING`), `WaitForSingleObject`, `GetConsoleScreenBufferInfo`, `SetConsoleCtrlHandler`, `_isatty`, `CreateProcess` |
| `out.c` | 2333 | `ioctl` for screen width, `isatty`, `getenv` locale | `GetConsoleScreenBufferInfo`, `_isatty`, UTF-8 code page |
| `shell.c` | 772 | `popen` for `.output \|cmd`, `getrusage` for `.timer`, `$HOME` | `_popen`, `GetProcessTimes`, `%USERPROFILE%` |
| `theme.c` | 801 | `isatty`, `$HOME`, `$XDG_CONFIG_HOME` | `_isatty`, `%APPDATA%\redstone` |
| `dot.c`, `import.c`, `main.c` | 1261 / 926 / 480 | `system()` for `.shell` and `.import \|cmd`, `isatty`, `fileno` | `system()` exists but runs `cmd.exe`, so quoting differs |
| Tests | | pty harness (`posix_openpt`, `fork`) for line and signal tests | None; needs a different harness |

The language itself is C99 and stays so. The portability gap is the terminal and process layer, about 7,500 lines of which roughly 40 call sites touch POSIX.

## Options

| Option | What it is | For | Against |
|---|---|---|---|
| A. Platform layer (recommended) | New `plat.h` with two implementations, `plat_posix.c` and `plat_win32.c`: raw mode, read-with-timeout, terminal size, resize notice, is-a-tty, config/state paths, run-command, process time. The rest of the code calls only `plat_*` | One seam, small, testable, no `#ifdef` scattered through 7,500 lines; matches the "few moving parts" rule; Linux behaviour does not change | Writing and testing the Win32 side; the existing pty tests do not apply to it |
| B. `#ifdef _WIN32` at each call site | Conditionals in place | Fastest to a first build | About 40 sites with duplicated reasoning; the line editor becomes hard to read and to verify |
| C. Use a library (PDCurses, libuv, linenoise-ng) | Delegate the terminal | Less code | Breaks the zero-dependency, C99, own-the-line-editor design; the completion menu and vi mode already rely on exact control of the terminal |
| D. WSL only | Document it | Zero work | Does not meet "works out of the box on Windows" |

## Windows specifics

- Terminal: Windows 10 1809 and later understand the same VT sequences the program already emits, once `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set on output and `ENABLE_VIRTUAL_TERMINAL_INPUT` on input. The banner, 24-bit colour and the completion menu then work unchanged. Older consoles (before 1809) would be unsupported; say so in the README.
- Encoding: set the console code pages to UTF-8 (65001) at start so `●`, `❯` and box drawing render, and restore them on exit.
- Resize: there is no `SIGWINCH`; the plat layer polls the screen buffer size when the editor wakes up, which is what the editor does already on each keypress.
- Paths: config in `%APPDATA%\redstone\`, state (history) in `%LOCALAPPDATA%\redstone\`, matching `~/.config` and `~/.local/state` on Linux. Per your earlier decision there is no fallback to the old `sqlsh` directory.
- SQLite: the Windows build uses the vendored amalgamation (`make SQLITE=vendored` already exists), so the `.exe` is self-contained: no DLL to install.
- Child processes: `.shell`, `.import |cmd` and `$EDITOR` go through `cmd.exe` on Windows, so quoting differs. The editor default falls back to `notepad` when neither `$VISUAL` nor `$EDITOR` is set.
- Line endings: `.import`, `.dump` and output stay byte-exact LF to keep `--compat` honest; files opened in text mode on Windows would add CR, so streams must be opened in binary mode.

## Build and install

| Target | Build | Install |
|---|---|---|
| Linux | existing `make` and the Nix flake | `make install PREFIX=...`, `nix build` |
| Windows | Cross-compile from the Nix shell with `pkgsCross.mingwW64` (static, vendored SQLite); a native MSVC or MinGW build is a later, optional step | A zip with `redstone.exe`, the man page as text, and a licence; optionally a Scoop manifest or winget entry once the repo is public |

## Verification limits, stated plainly

- This machine has no Windows host, and neither `wine` nor a MinGW compiler is on the PATH here. Inside the Nix shell I can add MinGW and Wine, so I can cross-compile, run the non-interactive parity suite and the unit tests under Wine, and check the console path under Wine's console.
- I cannot honestly claim the interactive console works on real Windows Terminal, `cmd.exe` and PowerShell until you have run it. These become manual checks in the roadmap.
- A CI job on a Windows runner would close this gap; that needs the repo's GitHub Actions, which I will not set up without being asked.

## Recommended plan (option A)

1. Add `plat.h` and move every POSIX call behind it with `plat_posix.c`, keeping behaviour identical; the existing 162 tests and 138 parity checks must stay green. This is a pure refactor and its own commit.
2. Add `plat_win32.c` and a MinGW cross-build target (`make windows`); parity and unit tests under Wine where they apply.
3. Replace the pty-only tests with a harness over `plat_*` so the line editor is testable on both.
4. Packaging: zip for Windows, README install sections for both platforms.
5. Manual checks on a real Windows machine.

## Decisions needed

1. Approve option A (platform layer) over the `#ifdef` approach?
2. Minimum Windows version: 10 build 1809 and later (recommended), or do you need older consoles?
3. Windows toolchain: cross-compile with MinGW from Nix (recommended) or do you build natively with MSVC?
4. Packaging: a zip of `redstone.exe` only, or also a Scoop/winget manifest?
5. Is a GitHub Actions Windows job in scope?
