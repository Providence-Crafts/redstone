# sqlsh

A drop-in replacement for `sqlite3(1)` with zsh-style completion, live syntax
highlighting, a colour theme system, and output worth looking at.

`sqlsh --compat` reproduces `sqlite3(1)` exactly — same dot commands, same
output modes, same command-line flags — verified byte-for-byte by a
differential test suite against the real binary. Outside `--compat`, sqlsh
adds a navigable completion menu (`<Tab>` for tables, columns, dot commands,
values), highlighting of the statement as you type, and box-drawn, coloured,
terminal-width-aware output by default.

See `man sqlsh` (`docs/sqlsh.1`) for the full reference: every flag, every
output mode, file locations, environment variables and exit status. This file
is the tour.

## Build

Everything happens inside the pinned Nix flake — no system dependency beyond
`libsqlite3` is assumed.

```sh
nix develop            # enter the dev shell once, then run make from inside it
make                    # debug build -> bin/sqlsh
make release            # optimised build
make SQLITE=vendored    # build against the sqlite amalgamation instead of the system library
make test               # unit + pty tests under ASan+UBSan
make parity             # differential output test vs the real sqlite3(1)
make gate               # format, both builds, tests, parity, cppcheck, clang-tidy -> PASS
make install PREFIX=~/.local   # installs bin/sqlsh and the man page only, never as/symlinked to sqlite3
```

`make help` lists every target, including `valgrind`, `fuzz`, `tidy`,
`cppcheck`, `compdb` and `watch`.

Without Nix, `nix build` (using the flake's `packages.default`) produces the
same release binary.

## Usage

```sh
sqlsh mydb.db                              # interactive, completion + highlighting + box output
sqlsh --compat mydb.db "SELECT * FROM t;"  # scripting, byte-identical to sqlite3(1)
alias sqlite3=sqlsh                        # meant to survive a normal day's work
```

With no database argument, sqlsh opens an in-memory database, exactly as
`sqlite3(1)` does. Any SQL given after the database name on the command line
runs as a batch and the shell exits; with none, it opens the interactive
prompt, or reads statements from standard input when that is not a terminal.

## Keybindings

Two keymaps, switched at runtime with `.editor emacs` or `.editor vi` (no
restart needed); emacs is the default.

**Emacs keymap** (always available)

| Key | Action |
|---|---|
| `←` `→` / `Ctrl-B` `Ctrl-F` | Move the cursor one character |
| `Ctrl-A` / `Ctrl-E` | Jump to start / end of line |
| `Ctrl-W` | Delete the word before the cursor |
| `Ctrl-U` | Delete to the start of the line |
| `Ctrl-K` | Delete to the end of the line |
| `Ctrl-T` | Transpose the two characters before the cursor |
| `↑` `↓` | Walk history |
| `Ctrl-L` | Clear the screen and repaint |
| `Ctrl-C` | Abandon the current line |
| `Ctrl-D` | End of file on an empty line, delete-forward otherwise |
| `Tab` | Open (or narrow) the completion menu; accept a unique match with no menu |

**Vi keymap** (`.editor vi`), normal mode

| Key | Action |
|---|---|
| `h` `j` `k` `l` | Move / history |
| `0` `$` `^` | Start, end, first non-blank of the line |
| `w` `b` `e` | Word motions |
| `i` `a` `I` `A` | Enter insert mode (before/after cursor, start/end of line) |
| `x` | Delete character under the cursor |
| `d` + motion (`dw`, `dd`, ...) | Delete |
| `c` + motion (`cw`, `cc`, ...) | Change |
| `r` | Replace one character |
| `u` | Undo |
| a leading count | Repeats the motion or command |

**Completion menu**, once open (either keymap)

| Key | Action |
|---|---|
| `Tab` / `Shift-Tab` | Next / previous candidate |
| `←` `→` `↑` `↓`, `Ctrl-N` `Ctrl-P` | Move the selection |
| Typing | Narrows the candidates in place |
| `Enter` | Accept the selection |
| `Esc` / `Ctrl-G` | Dismiss, restoring the buffer exactly |

## Output modes and theming

The default, interactive or not, is `box` with headers and colour, sized to
the terminal (`ioctl(TIOCGWINSZ)`, re-read before every result, so resizing
the window takes effect on the next query). `--compat` switches to `list`,
headers off, no colour — upstream's default. Every classic mode
(`ascii box column csv html insert json line list markdown quote table tabs`)
is byte-parity-tested against `sqlite3(1)`; the rest
(`c count jatom jobject off psql qbox split tcl`) are best-effort. See
`.help mode` for the option flags each one accepts.

Colours come from `theme.c`'s theme file
(`$XDG_CONFIG_HOME/sqlsh/theme`, default `~/.config/sqlsh/theme`), an INI file
with `[menu]`, `[syntax]` and `[output]` sections. `.theme` with no argument
prints the active palette in that same loadable format; `.theme dark|light|
basic|no-color` or a file path loads another one. `NO_COLOR` and `TERM=dumb`
turn colour and highlighting off regardless.

## Parity

`sqlsh --compat` targets byte-identical behaviour with the linked
`sqlite3(1)` for every flag, dot command and classic output mode; the gate's
differential suite (`make parity`) is what backs that claim rather than
asserting it. Where sqlsh knowingly differs — an extension this build does not
vendor, a mode the shipped binary itself rejects, and similar — the gap is
refused loudly (an error naming what's missing) rather than approximated
silently. `docs/notes/` and `PROJECT.md`'s "Known gaps" tables carry the
complete, current list; the closest single-command summary is `.help` inside
the shell, which marks each of the 11 unsupported commands as such.

## Development

`PROJECT.md` is the full roadmap: architecture, phase-by-phase history, design
decisions and their rationale, the deferred-work log, and every outstanding
manual check. `docs/ARCHITECTURE.md` covers the module layering in more
depth; `docs/development-workflow.md` covers the day-to-day process this
project follows.
