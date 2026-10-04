# Testing guide

A walkthrough for exercising `redstone` by hand, beyond what `make gate` already
proves automatically. Use it after a build, before a release, or whenever a
change touches the terminal layer (completion, highlighting, output width) —
the one class of behaviour the automatic suite cannot fully see, because it
depends on a real controlling terminal.

Each section names what to run and what "correct" looks like. Where the
project's own roadmap (`PROJECT.md`) already tracks a check as open, this
guide points at it rather than duplicating the tracking.

## Setup

```sh
nix develop
make            # debug build
make fixtures   # tests/test.db, if not already generated
make gate       # confirms the automatic baseline before you start by hand
```

Everything below assumes `./bin/redstone` is the freshly built binary and
`tests/test.db` is the fixture database, unless a step says otherwise.

## 1. Smoke test

```sh
./bin/redstone tests/test.db "SELECT * FROM employees LIMIT 3;"
./bin/redstone --compat tests/test.db "SELECT * FROM employees LIMIT 3;"
```

- Default: box-drawn, coloured, headers on.
- `--compat`: `|`-separated, no colour, headers off — must match
  `sqlite3 tests/test.db "SELECT * FROM employees LIMIT 3;"` byte for byte.
  (This exact property is what `make parity` asserts continuously; a manual
  spot-check here is just a sanity check before diving into what parity
  *doesn't* cover.)

## 2. Completion menu

Open `./bin/redstone tests/test.db` interactively for all of this section.

**The four headline scenarios**

| Type | Expect |
|---|---|
| `<Tab>` (empty line) | Menu of statement-starting keywords and dot commands |
| `.tables <Tab>` | Table names |
| `SELECT <Tab> FROM <Tab>` | Columns, then table names |
| `SELECT * FROM employees WHERE <Tab>` | Columns of `employees` only — not of every table in the schema |

**Edge cases**

- A unique candidate (e.g. `SELECT * FROM emplo<Tab>` if only one table
  matches) inserts directly with **no menu drawn**.
- Typing while the menu is open **narrows** the candidate list in place
  rather than dismissing it.
- `Esc` or `Ctrl-G` dismisses the menu and restores the buffer and screen
  exactly as they were before `Tab` was pressed — no leftover fragment.
- Arrow keys (`←` `→` `↑` `↓`) and `Ctrl-N`/`Ctrl-P` move the selection;
  `Enter` accepts it.
- Resize the terminal window while a menu is open (`SIGWINCH`): the menu
  re-lays-out rather than leaving stale rows on screen.
- Scroll test: a context with many candidates (e.g. `<Tab>` on an empty line,
  which offers the full keyword/pragma/function set) should scroll rather
  than overflow the screen.
- Value completion: `SELECT * FROM employees WHERE department = <Tab>` should
  offer actual column values, capped and quoted where needed; on a large
  table this must return promptly (bounded internally at ~150 ms) rather than
  stalling the prompt.
- **Not automatable, still worth doing by hand** (`PROJECT.md` Phase 4 manual
  check): does the menu *feel* like zsh's? No flicker, correct placement near
  the bottom of the screen, readable columns — including with the window
  scrolled so the prompt sits on the last row, and in a narrow window.

## 3. Syntax highlighting

While typing, in the interactive shell:

- Keywords, functions, strings, numbers, comments, `:name`/`@name`/`$name`
  parameters, and quoted identifiers (`"x"`, `` `x` ``, `[x]`) each get a
  distinct colour.
- A known table, view, or in-scope column is coloured as such; an **unknown**
  name (typo) stays plain — this is the point of the feature, and is worth
  deliberately checking: type `SELECT nosuchcolumn FROM employees;` and
  confirm `nosuchcolumn` does *not* get the "known column" colour.
- An unbalanced quote or paren shows the error colour — this is also the
  visual cue for why the prompt is asking for a continuation line.
- `NO_COLOR=1 ./bin/redstone tests/test.db` disables highlighting as well as
  output colour (both, not just one).
- A malformed theme file falls back to the built-in palette rather than
  refusing to start — plant a broken `~/.config/redstone/theme` (e.g. an unknown
  section, or `color = not-a-color`) and confirm the shell still starts, with
  a diagnostic rather than a crash.
- **Not automatable** (`PROJECT.md` Phase 7 manual check): are colours
  actually legible on your dark *and* light terminal profiles? Try
  `.theme dark`, `.theme light`, and `.theme basic` (the 16-colour-safe
  palette) under each.

## 4. Themes

```
.theme                # dumps the active palette in loadable format
.theme dark|light|basic|no-color
.theme /path/to/file
.theme reload
.theme on|off
```

- `.theme` with no argument round-trips: piping its output to a file and
  loading that file back should reproduce the same palette.
- Loading a nonexistent file or an unknown built-in name should fail with a
  message, not silently keep the old palette or crash.

## 5. Line editor and keybindings

Covered in the guide's own README section for the full key list; the parts
worth *doing*, not just reading:

- `.editor vi` then `.editor emacs`: switches the keymap without restarting,
  and the prompt shows the vi mode indicator only in vi mode.
- History: run a few statements, restart the shell, press `↑` — history
  should have persisted (`$XDG_STATE_HOME/redstone/history`, default
  `~/.local/state/redstone/history`).
- `Ctrl-C` on a partially-typed line abandons it without exiting the shell;
  `Ctrl-D` on an *empty* line exits, on a non-empty line deletes forward.
- Kill the shell (`kill -TERM <pid>`) mid-line and confirm the terminal is
  left in normal (non-raw) mode afterward — `reset` should not be necessary.
  (Automated as `test_sigterm_restores_termios`, but worth confirming by hand
  once per release, since a real terminal emulator can behave subtly
  differently from the pty harness.)
- Type a line longer than the terminal width, then edit at the far end from
  the cursor (e.g. `Ctrl-A` to jump to the start). The buffer should end up
  correct, and the on-screen redraw should not leave garbage from a previous,
  differently-wrapped draft. (Automated as `test_pty_long_line_redraw`
  against a 20-column pty; a real terminal is still worth a manual spot
  check, since actual wrapping behaviour varies slightly by terminal
  emulator.)
- Non-interactive: `echo "SELECT 1;" | ./bin/redstone tests/test.db` should
  write no escape sequences at all — pipe it through `cat -v` and confirm
  there's nothing but the plain result.

## 6. Output modes and screen width

```sh
for m in ascii box column csv html insert json line list markdown quote table tabs; do
  diff <(./bin/redstone --compat -$m tests/test.db "SELECT * FROM employees;") \
       <(sqlite3 -$m tests/test.db "SELECT * FROM employees;")
done
```

(This is exactly what `make parity` already automates across more fixtures —
useful here mainly as a quick manual spot-check against a query of your own
choosing that isn't in the fixture set.)

**Edge cases specific to this project, not just parity:**

- `-screenwidth 1` (or `.mode --screenwidth 1`) must be **rejected**:
  `redstone: minimum --screenwidth is 2`, exit code 2. `-screenwidth 2` is the
  smallest accepted value.
- `.mode box --widths 10,0` (pin one column, leave another free) on a wide
  row, then shrink `--screenwidth`: the pinned column must stay exactly its
  width while the unpinned one gives up space instead.
- Live resize: run a wide query, narrow the actual terminal window, run the
  same query again **without restarting** — the shrink should track the new
  width. `--compat` and an explicit `-screenwidth N` both disable this
  auto-detection deliberately; confirm the width stays fixed under either.
  (`PROJECT.md` Phase 5 manual check — not automatable, since
  `ioctl(TIOCGWINSZ)` needs a real controlling terminal.)
- A row containing CJK characters and a row containing combining characters
  (e.g. `é` as `e` + combining acute) should still align columns correctly in
  `box` and `column` mode.
- `LC_ALL=C ./bin/redstone tests/test.db` should fall back to ASCII borders
  (`+`/`-`/`|`) instead of Unicode box-drawing characters, and stay aligned.
- A blob containing an embedded NUL byte, printed under
  `.mode box --blob-quote text`: expect **truncation at the NUL** (a C-string
  limitation shared with upstream `sqlite3(1)` — not a bug to chase). Use
  `--blob-quote hex` or `--blob-quote sql` to see the whole blob instead.
- `www` mode: confirm `.mode www` is rejected (`unknown mode`) — the shipped
  3.53.3 `sqlite3(1)` binary rejects it too, even though trunk `shell.c`
  still lists the preset; this is deliberate, not a gap.

## 7. Dot commands and CLI parity

- **The 11 refused commands** (`.archive`/`.ar`, `.check`, `.expert`,
  `.imposter`, `.intck`, `.recover`, `.scanstats`, `.selftest`, `.session`,
  `.sha3sum`, `.testcase`): each should fail with a message naming the
  extension source it needs, not "unknown command" and not silent success.
- `-noinit` suppresses **both** `~/.sqliterc` and the theme file. Put
  `.mode box` in `~/.sqliterc` (temporarily — remember to remove it after):
  `./bin/redstone tests/test.db "SELECT 1;"` should come out in `box` mode,
  `./bin/redstone -noinit tests/test.db "SELECT 1;"` should come out in `list`
  mode instead. (`PROJECT.md` Phase 6 manual check — not automated because
  the suite will not plant files in a real `$HOME`.)
- `.import` a CSV with embedded commas, quotes, and newlines inside quoted
  fields; round-trip it with `.dump` and reload — data should survive intact.
  Known deviations from upstream, worth confirming rather than "fixing":
  no UTF-8 BOM stripping on the first field; duplicate-column renaming is an
  approximation (`_N` suffix, not upstream's `zAutoColumn`); no `-esc`/
  `-qesc` backslash-escape option.
- `-safe -nonce mypass`: confirm `.shell` and similar are refused under
  `-safe`, and that a line prefixed with the matching nonce is allowed
  through for that one line only.
- **The big one, ongoing rather than a single session**
  (`PROJECT.md` Phase 6 manual check): `alias sqlite3=redstone` and use it for a
  real day's work. Anything that behaves differently from actual
  `sqlite3(1)` is a parity bug worth filing, not something to route around.

## 8. Regression checklist after a change

Match the area you touched against the table below, then run at minimum
`make gate` plus the matching manual section above.

| You changed | Also check |
|---|---|
| `out.c`, `width.c` | §6 (output modes, screen width, CJK/combining alignment) |
| `menu.c`, `comp.c`, `sqlctx.c` | §2 (completion menu) |
| `hl.c`, `theme.c` | §3, §4 (highlighting, themes) |
| `edit.c`, `line.c` | §5 (line editor, keybindings, pty behaviour) |
| `dot.c`, `schema.c`, `import.c`, `shell.c`, `main.c` | §7, plus `make parity` |

## Reference: where the rest lives

- **`PROJECT.md`** — "Known gaps" tables (per phase) and "Manual checks
  outstanding" are the canonical, living list this guide draws from; check
  there for anything newer than this guide.
- **`docs/redstone.1`** (`man redstone`) — the full flag/mode/file reference.
- **`docs/notes/`** — phase-specific parity investigation notes (e.g.
  `phase5-parity-findings.md`), for *why* a specific difference from upstream
  exists.
