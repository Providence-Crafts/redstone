---
title: "redstone — a drop-in SQLite shell with zsh-style completion"
id: "redstone"
status: waiting                # initiated | defined | in-research | in-progress | waiting | completed
priority: medium              # critical | high | medium | low
start_date: "2026-09-15"
target_date: ""
last_updated: "2026-09-23"
owner: "rs"
stakeholders: []
tags: [c, sqlite, cli, suckless, terminal, completion]
depends_on: []
blocks: []
references:
  - "docs/ARCHITECTURE.md"
  - "docs/development-workflow.md"
  - "docs/redstone.1"
  - "README.md"
  - "https://sqlite.org/c3ref/intro.html"
  - "https://sqlite.org/cli.html"
notes: "Phases 0-8 complete and gate-green. Waiting on owner sign-off (Phase 8's one remaining manual check) and on the manual checks table."
---

# redstone

## Overview

**Purpose.** `sqlite3(1)` is a capable shell with a poor interactive surface: no
completion worth the name, so every session involves recalling table names,
column names and legal values from memory or from a second terminal. `redstone` is
a **drop-in replacement** for it — same dot commands, same output modes, same
CLI flags — with two things added on top: **zsh-style completion**, where
`<Tab>` opens a navigable menu of candidates drawn from the live schema and
data, and a **presentation layer worth looking at**.

**Context.** C99 against the public `sqlite3` C API, under suckless
constraints: one library dependency, a small auditable codebase, no speculative
abstraction. The environment is a pinned Nix flake.

**Scope.** Everything `sqlite3(1)` does that is reachable through the public C
API — 54 of the 65 dot commands its `.help` lists, all 22 output-mode presets
its binary accepts, and its command-line
flags — plus completion, colour and theming. The 11 dot commands backed by
vendored extensions or internal APIs are recognised and refused with a pointer
to `sqlite3(1)`, never silently missing.

**Explicitly excluded**, by decision: bundling SQLite's extension sources
(`sqlar`, `zipfile`, `sha3`, `sqlite3expert`, `sqlite3recover`, `sqlite3_intck`,
`sqlite3session`, `dbdata`), a complete SQL parser, and Windows support.

## Goals

**Goals**

1. **Parity.** A user can alias `sqlite3` to `redstone` and not notice anything
   missing in ordinary work — same commands, same modes, same flags, same
   behaviour.
2. **Completion.** Context-aware candidates wherever a known list exists: dot
   commands and their arguments, tables, columns scoped to the query's `FROM`
   set, column values, and the 147 keywords / 219 functions / 66 pragmas the
   library can enumerate at runtime.
3. **A navigable menu**, not a printed list: arrows and Tab to move, Enter to
   accept, typing to narrow in place.
4. **Beauty.** Unicode box output with type-aware colour, syntax highlighting
   while typing, a rich completion menu, and a theme file so none of it is
   hardcoded.
5. A codebase one person can read in an afternoon.

**Success criteria**

- The four scenarios from the original request work end to end:
  `<tab>` · `.tables <tab>` · `SELECT <tab> FROM <tab>` · `WHERE <tab>`.
- Every one of the 54 portable dot commands has a test or a parity check
  asserting its effect; every one of the 11 unsupported commands is refused
  with a useful message.
- `redstone --compat` output is byte-identical to `sqlite3(1)` across the classic
  mode matrix, verified by differential test in the gate.
- `make gate` prints PASS: formatter, both builds, sanitized tests, cppcheck
  and clang-tidy clean, zero warnings.
- Total `src/` stays in the region of 13,500 lines (owner's revision at Phase 7
  planning, from 10,000 at Phase 6 and 6,000 originally: the dot-command surface
  turned out to cost what it costs, and the budget was moved to reality rather
  than reality trimmed to the budget). Upstream `shell.c` is 37,373. **Currently
  14,008** — Phase 7 added `hl.c` (232) and roughly 400 lines of theme-file
  parser, leaving the total about 4% over the accepted budget.

**Constraints**

- C99 (`-std=c99`) plus POSIX.1-2008 for termios/isatty/sigaction. No GNU
  extensions.
- Exactly one library dependency: `libsqlite3`. No readline, libedit,
  linenoise, ncurses or terminfo.
- Completion must never stall the prompt: any query run on its behalf is capped
  and time-limited.
- `companies.db` is a read-only symlink into the owner's notes. **Never written
  to.** Tests use the generated `tests/test.db`.

## Development Guidelines

- **Environment**: Nix flakes. `nix develop` provides the toolchain; enter the
  shell once and work inside it rather than re-entering per command.
- **Version control**: `git`. **One commit per phase**, made only when that
  phase's gate is green and its manual checks are confirmed by the owner.
  Stage explicit paths; never stage-all. Never push, never tag.
- **Workflow**: planning is supervised, implementation is autonomous. A phase
  is implemented only after its plan is approved.
- **Manual checks do not block the commit** (owner's decision, 2026-09-16,
  overriding the default in `docs/development-workflow.md`). A phase is
  committed once its automatic gate is green; its manual boxes stay unticked
  until the owner has actually looked. Every outstanding manual check is
  collected in "Manual checks outstanding" below, with how to reproduce it. A
  box is never ticked on the agent's say-so — the roadmap must not claim a
  human verified something no human saw.
- **Sub-agents**: implementation work is checked and corrected by Sonnet 5
  sub-agents — running the gate, writing the repetitive per-command and
  per-mode tests, and fixing mechanical findings. The orchestrator never trusts
  a sub-agent's "done": it re-runs `make gate` itself before accepting a phase.
- **Undefined at implementation time**: flag, never guess. Blocks the phase's
  oracle → halt. Peripheral → record in Deferred work and continue.
- **Style**: suckless. The simplest construction that meets the requirement;
  deleting code is progress. Comments explain *why*, not *what*.
- **Warnings are defects.** Fixed at the cause or waived below with a
  rationale. Never suppressed silently or globally.
- **Parity is verified, not asserted.** Where behaviour is meant to match
  `sqlite3(1)`, the test runs both binaries and compares, rather than encoding
  someone's belief about what upstream does.
- **Roadmap expansion**: when all defined phases are complete, extend the
  roadmap with the next phases and their verification plans.

### Commit convention

```
phase(N): imperative summary

Body: what changed and why, when the summary is insufficient.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
```

### The gate

One command, one verdict: `make gate` → PASS or a non-zero exit.

| Step | Command | Fails on |
|------|---------|----------|
| format | `make format-check` | any file the formatter would change |
| build, system sqlite | `make release WARNINGS_AS_ERRORS=1` | any warning |
| build, vendored sqlite | `make SQLITE=vendored release WARNINGS_AS_ERRORS=1` | any warning |
| tests | `make test` (ASan + UBSan) | a failed test, a leak, UB |
| cppcheck | `make cppcheck` | any finding |
| clang-tidy | `make tidy` | any finding |

Both SQLite backends are built because they are separate compilations and a
warning can surface in only one of them. From Phase 5 the gate also runs the
differential parity suite.

### Waived diagnostics

Every suppression in the project, with its reason. Nothing is silenced outside
this list.

| Diagnostic | Where | Rationale |
|---|---|---|
| `readability-identifier-length` | `.clang-tidy` | `i`, `n`, `db` are clearer than enforced long names in code this dense. |
| `readability-magic-numbers` | `.clang-tidy` | Local constants in formatting code read better inline; shared ones are `#define`d. |
| `readability-uppercase-literal-suffix` | `.clang-tidy` | The codebase uses lowercase `u`/`l` consistently. |
| `misc-include-cleaner` | `.clang-tidy` | Reports transitive sqlite3.h symbols as needing direct includes. |
| `bugprone-easily-swappable-parameters` | `.clang-tidy` | Fires on every `(stmt, ncol, out)`; the fix would be worse than the risk. |
| `cert-err33-c` | `.clang-tidy` | `fputc`/`fprintf` returns are unchecked by design; `db_exec` does one `fflush`+`ferror` per statement instead, since stream errors are sticky. |
| `missingIncludeSystem`, `unusedFunction`, `checkersReport`, `toomanyconfigs` | `Makefile` cppcheck | Statements about cppcheck's own analysis or about sqlite3.h, not about `src/`. `unusedFunction` is false on a two-binary build. |
| `readability-function-cognitive-complexity` | `tests/.clang-tidy`, and from Phase 6 `.clang-tidy` | In tests it counts the branch minunit's `mu_run_test` expands to, so the score is just the test count. In `src/` the functions over the threshold are flat option dispatchers (`out_command`, `apply_option`, `import_cmd_import`, `main`) and line-by-line ports of upstream `shell.c` (`format_schema`, `schema_cmd_dump`, `split_cell`, `dot_split`): they score high because they have many one-line branches, not because any branch is deep, and splitting an option table into helpers would hide the correspondence with `sqlite3(1)` that the parity suite measures. |
| `bugprone-multi-level-implicit-pointer-conversion` | `.clang-tidy` | Fires on `malloc`/`realloc` into a `char **`. C converts `void *` implicitly by design and casting the result of `malloc` is discouraged, so the "fix" adds noise and can hide a missing `<stdlib.h>`. |
| `constParameterPointer` on `db_out` | `src/db.c`, inline | The `Db` is not const to the caller: `db_out` exists to hand out a formatter the caller then reconfigures. Const-qualifying the parameter would launder that away. |
| `staticFunction` on `out_set_table_name` | `src/out.c`, inline | One of `out.h`'s uniform setter family. Called from `schema.c` since Phase 6, so the finding is stale on the current tree; the suppression stays because cppcheck analyses translation units one at a time. |
| `constParameterCallback` on `value_progress` | `src/db.c`, inline | `sqlite3_progress_handler` dictates the `void *` signature; const-qualifying it would need a function-pointer cast, which is worse. |
| `clang-format` on `g_width[]` and `g_preset[]` | `src/width.c`, `src/out.c`, inline `clang-format off` | Both are hand-aligned data tables — a width range and a mode preset are read across a row. Reflowing them to 100 columns destroys the grid and nothing else. |
| `_FORTIFY_SOURCE` glibc `#warning` | `flake.nix` `hardeningDisable`, `Makefile` `TIDY_EXTRA` | nix's cc-wrapper injects it; glibc then warns at the `-O0` used by debug and compdb builds. Environmental, not ours. |

## Architecture

Detailed design lives in `docs/ARCHITECTURE.md`; this is the shape.

**Structure.** One concern per module — a `.c` in `src/`, a header in
`include/` — with dependencies pointing one way and no module reaching into
another's internals.

```
main.c      argument parsing (upstream flag set), REPL driver, signals
  |
  +-- shell.c   the session: connection, redirects, switches, REPL, init files
  |     |
  |     +-- dot.c     dot-command table and dispatch (63 + 12 refusals)
  |     +-- schema.c  .schema .dump .databases .dbinfo .clone .lint .dbtotxt
  |     +-- import.c  .import, .excel/.www, temp files
  |     |
  |     +-- out.c    the output modes, box drawing, colour
  |           |
  |           +-- width.c  UTF-8 display width, shared with menu.c
  |
  +-- line.c    raw-mode line editor: keys, cursor, history, redraw
  |     |
  |     +-- hl.c      syntax highlighting of the buffer being typed
  |     +-- menu.c    completion menu: layout, navigation, rendering
  |           |
  |           +-- comp.c    candidate generation
  |                 |
  |                 +-- sqlctx.c   SQL tokenizer + cursor-context machine
  |
  +-- theme.c   colour table, capability detection, theme file parsing
  +-- db.c      sqlite3 handle, execution, schema introspection, value cache
```

**Key property.** `sqlctx.c` and `comp.c` are pure with respect to the
terminal: they take a buffer and a cursor offset and return data. That is what
makes the completion engine unit-testable without a pty, and it is the main
reason the decomposition looks the way it does. `hl.c` reuses `sqlctx.c`'s
tokenizer, so highlighting costs a mapping from token kind to colour and
nothing more.

**Components**

| Module | Status | Responsibility |
|---|---|---|
| `src/db.c` · `include/db.h` | implemented | The only code that includes `<sqlite3.h>`. Handle, execution, introspection, value cache. Formatting moves to `out.c` in Phase 5. |
| `src/main.c` | implemented | Argument parsing only: upstream's flag set, then it builds the `Db`, the `Line` and the `Shell` and hands over. |
| `src/shell.c` · `include/shell.h` | implemented | The session: the current connection and the auxiliary ones, the `.output`/`.once` redirect, the switches (`echo`, `bail`, `timer`, `stats`, `changes`, `eqp`, safe mode), statement execution, the REPL and the init files. Dot commands are handed a `Shell`, so nothing below `main.c` needs a global. |
| `src/edit.c` · `include/edit.h` | implemented | The editing core: buffer, cursor, emacs and vi keymaps, history ring. Performs no I/O, so every keybinding is unit-testable. |
| `src/line.c` · `include/line.h` | implemented | Terminal layer: termios raw mode, signal-safe restoration, redraw, escape timeout, history file. |
| `src/sqlctx.c` · `include/sqlctx.h` | implemented | Tokenizer and cursor-context machine. Pure: no allocation, no I/O, no recursion. Phase 7's highlighter reuses the lexer. |
| `src/comp.c` · `include/comp.h` | implemented | Context → candidate list. Pure given a `Db`: no terminal, no globals. The dot-command table is injected as a `CompDotSource`, so Phase 6 owns it alone. |
| `src/menu.c` · `include/menu.h` | implemented | The navigable menu: layout, selection, rendering. Told its size, returns bytes; `line.c` owns the terminal. |
| `src/out.c` | Phase 5 | 13 output modes, box drawing, type-aware colour. |
| `src/theme.c` · `include/theme.h` | implemented | Colour capability detection, the 25-style table, the INI theme-file parser and `theme_dump`. The four built-in palettes are theme-file *text* parsed by that same parser, so a shipped theme cannot accept anything a user's file may not. |
| `src/dot.c` · `include/dot.h` | implemented | The command table (76 entries: 64 implemented, 12 refused — Phase 7 added `.theme`), the splitter, dispatch, `.help`, and the small commands that are one setting each. |
| `src/schema.c` · `include/schema.h` | implemented | The introspection commands: `.schema`, `.fullschema`, `.dump`, `.databases`, `.indexes`, `.tables`, `.dbinfo`, `.dbtotxt`, `.clone`, `.lint`. Byte-for-byte ports, which is why they are their own module. |
| `src/import.c` · `include/import.h` | implemented | `.import` (RFC 4180 CSV and ASCII-delimited), `.excel`/`.www`, and the temp-file handling they need. |
| `src/hl.c` · `include/hl.h` | implemented | Syntax highlighting while typing. Pure and database-unaware: schema lookups arrive as four callbacks in an `HlSchema`, the same injection pattern as `LineCompleter`, so every colouring decision is testable without a connection. |

**Runtime-enumerable candidate sources.** Nothing is hardcoded that the library
can report: 147 keywords via `sqlite3_keyword_count`/`sqlite3_keyword_name`,
219 functions via `pragma_function_list`, 66 pragmas via `pragma_pragma_list`,
plus schema objects from `sqlite_schema` and `pragma_table_info`. These lists
therefore track whichever libsqlite3 is linked, and cost no maintenance.

**Resources.** `libsqlite3` (pinned 3.53.3, system or vendored amalgamation);
Nix flake for the toolchain; upstream `shell.c` consulted on demand in
`reference/`, never committed, never linked; the `sqlite3` binary from the
flake, used as the oracle in differential parity tests.

## Roadmap

**Hierarchy**: Phase → Task → Check

| Symbol | Meaning          |
| ------ | ---------------- |
| `[ ]`  | To-do            |
| `[~]`  | In-progress      |
| `[✓]`  | Done / completed |
| `[x]`  | Failed / blocked |
| `[?]`  | Optional / TBD   |
| `[!]`  | Critical         |

### General conditions

Phases are ordered so each one ends with a program that builds, runs, and is
more useful than the last. Every phase ends with `make gate` green; a phase
carrying manual checks stops there and waits for the owner.

All work happens inside `nix develop`. The gate builds both SQLite backends.
`companies.db` is read-only; tests use `tests/test.db`, generated by
`make fixtures` from `tests/fixtures.sql` and never committed.

Phases 1–4 build the completion feature; 5–6 deliver parity; 7 finishes the
presentation. 5 and 6 depend on 3 only for completable arguments, so they could
be reordered if parity becomes urgent.

---

### Phase 0: Tooling and skeleton `[✓]`

**Description**

Establish the repository, adapt the C template, and get a REPL that opens a
database and runs SQL. No completion.

**Tasks**

- [✓] git repository; `.gitignore` excluding user databases and `reference/`
- [✓] `flake.nix`: devShell, `libsqlite3`, pinned amalgamation for vendored builds
- [✓] `Makefile`: C99, dual SQLite backend, sanitizers, fixtures, `gate`
- [✓] `.clangd` / `.clang-tidy` / `.clang-format` adapted from the template
- [✓] `docs/ARCHITECTURE.md`, `PROJECT.md`
- [✓] `src/db.c`: open, execute, five output modes, error reporting
- [✓] `src/main.c`: argument parsing, multi-line REPL, stub dot commands
- [✓] `tests/fixtures.sql` and a generated `tests/test.db`
- [✓] `tests/test_db.c`: 7 tests including CSV quoting and write-failure

**Checks**

*Automatic*

- [✓] `make gate` → PASS
- [✓] `redstone tests/test.db 'SELECT ...'` prints rows; exit 0
- [✓] exit 1 on SQL error and on an incomplete statement at EOF
- [✓] `make SQLITE=vendored` produces a binary with no `libsqlite3` linkage
- [✓] `make valgrind` reports 0 errors

*Manual*

- [✓] Interactive session against `companies.db` behaves sanely

**Design decisions**

- Decision: **Clean-room reimplementation against the sqlite3 C API, not a
  textual fork of `shell.c`.**
  Rationale: `shell.c` is 37,373 lines in the pinned 3.53.3, machine-generated
  by concatenating `shell.c.in` with a dozen extension sources. It cannot be
  meaningfully diffed or hand-maintained, and patching completion into it means
  owning a blob nobody here wrote.
  Trade-offs: parity must be built and verified deliberately rather than
  inherited. Mitigated by the differential test suite from Phase 5, which makes
  parity a measured property instead of a claim.

- Decision: **No line-editing library.**
  Rationale: forced, not ascetic. readline, libedit and linenoise all implement
  completion as *print candidates, return to the prompt*. None has a
  persistent, navigable menu that narrows as you type. The headline feature is
  unreachable through any of them.

- Decision: **Build objects live in `build/<backend>/<mode>/`.**
  Rationale: a shared `build/` let `make test` link objects an earlier
  `make debug` had compiled without sanitizers — the suite ran uninstrumented
  while appearing to pass.

- Decision: **Dual SQLite backend**, system by default, amalgamation under
  `make SQLITE=vendored`. The amalgamation version is pinned to `pkgs.sqlite`
  and the devShell warns on drift.

**Notes**

Three real bugs were found by the tooling during this phase: `CC ?=` never
firing because make predefines `CC`; the stale-object contamination above; and
`db_exec` reporting success after a failed write, because `ferror` was
consulted before anything had been flushed to the descriptor.

---

### Phase 1: Raw-mode line editor `[x]`

**Description**

`src/edit.c`/`include/edit.h` and `src/line.c`/`include/line.h`. Take ownership
of the terminal and give the shell a real line editor. No completion yet — this
is the foundation Phase 4 renders into, and on its own it must already beat
`sqlite3(1)`'s prompt.

Correctness under every exit path is the point: a shell that leaves a terminal
in raw mode after a crash is worse than no shell.

**Tasks**

- [x] `line_new` / `line_free`: raw mode via `tcsetattr`, original termios
      saved, restored by `atexit` and by `SIGTERM`/`SIGQUIT`/`SIGHUP`/`SIGSEGV`/
      `SIGABRT` handlers using only async-signal-safe calls
- [x] input decoding: printable bytes, backspace, `Ctrl-C`, `Ctrl-D` on an
      empty line quits, `Ctrl-L` clears
- [x] cursor motion and editing: `←` `→` `Ctrl-A` `Ctrl-E` `Ctrl-W` `Ctrl-U`
      `Ctrl-K` `Ctrl-T`
- [x] escape-sequence parser for arrow/Home/End/Delete, tolerant of unknown
      sequences and of a split read
- [x] **modal editing**: an emacs keymap (default) and a vi keymap with normal
      and insert states
- [x] vi normal mode: `h j k l` `0 $ ^` `w b e` `i a I A` `x` `d` with `dw`/`dd`,
      `c` with `cw`/`cc`, `r`, `u`, and counts
- [x] `.editor emacs|vi` selects the keymap; the mode is shown in the prompt in
      vi mode
- [x] history: in-memory ring, `↑` `↓`, deduplicated, persisted to
      `$XDG_STATE_HOME/redstone/history` (default `~/.local/state/redstone/history`)
      with a bounded size
- [x] redraw: prompt plus buffer, correct when the line exceeds terminal width;
      `SIGWINCH` sets a flag that triggers re-layout
- [x] non-tty fallback path, no escapes emitted, pipes keep working
- [x] `main.c` uses `line.c` in place of `read_line`, continuation prompt intact
- [x] pty test harness, driving the editor with a scripted byte stream — the
      mechanism every later interactive phase is gated on

**Checks**

*Automatic*

- [x] `make gate` → PASS
- [x] pty tests: each editing key produces the expected buffer, asserted on the
      final line delivered rather than on the escape bytes
- [x] negative: an unknown escape sequence is swallowed, never inserted as
      literal text
- [x] history survives a save/load round trip; the ring drops the oldest entry
      at its bound, and the state directory is created when absent
- [x] vi keymap: a table of (keystrokes, starting buffer) → expected buffer
      covering motions, counts and operator-motion pairs
- [x] negative: in vi normal mode a printable key that is not a command is
      ignored, never inserted
- [x] a non-tty read writes nothing to the output stream (no prompt, no escapes)
- [x] a child killed with `SIGTERM` mid-line leaves the pty out of raw mode

*Manual*

- [ ] A real session feels right: no flicker, no cursor drift on a long line,
      `Ctrl-C` behaves as expected

**Design decisions**

- Decision: **Build the pty harness in this phase, not at the end.**
  Rationale: the workflow requires automatic checks that would fail a plausible
  wrong implementation, and a line editor cannot be gated any other way. Every
  later interactive phase needs the same mechanism; building it late means
  Phases 1 and 4 land ungated.
  Trade-offs: adds pty work here. **Resolved at implementation:** POSIX
  `posix_openpt`/`grantpt`/`unlockpt`/`ptsname` is enough, so the harness links
  no libutil — verified with `ldd`. The harness lives in `tests/test_line.c`
  rather than a separate `tests/pty.c`; it is ~60 lines and has one consumer.

- Decision: **Byte-oriented editing first; UTF-8 display width in Phase 5.**
  Rationale: multi-byte cursor arithmetic is separable, and getting key
  handling right matters more. Recorded so it is a choice, not a bug.

- Decision: **Both an emacs and a vi keymap, designed in from the start.**
  Rationale: owner's call, and the reason to decide it now rather than defer is
  structural — modality changes how keys are dispatched, so retrofitting it
  means rewriting the dispatch. Keymaps are tables of (state, key) → action;
  emacs is the degenerate case with one state.
  Trade-offs: more surface in Phase 1 and more tests. Accepted, because the
  alternative is rewriting `line.c` later.

- Decision: **POSIX `posix_openpt`/`grantpt`/`unlockpt` for the pty harness,
  not `forkpty`.** Verified at planning time to work under `-std=c99` with
  `_XOPEN_SOURCE 700` and to link with no libutil dependency, keeping the
  one-dependency constraint intact.

- Decision: **XDG paths, with `~/.sqliterc` still honoured.** History at
  `$XDG_STATE_HOME/redstone/history`, config and theme under
  `$XDG_CONFIG_HOME/redstone/`. `~/.sqliterc` is read for parity (Phase 6),
  followed by `~/.config/redstone/redstonerc` so ours wins on conflict.

**Dependencies** — Phase 0.

**Notes / Risks**

Signal-safe restoration is the risky part: handlers may call only
async-signal-safe functions, ruling out `fprintf`. Plan is a flag plus
`write(2)` of a fixed reset sequence, with `tcsetattr` restoring the saved
termios.

---

### Phase 2: SQL context analysis `[x]`

**Description**

`src/sqlctx.c`. Given a buffer and a cursor offset, decide what may legally
appear there. Pure, terminal-free, and the most testable part of the program.
Not a parser: completion needs to know *where the cursor is*, not what the
query means. Phase 7's highlighter reuses this tokenizer unchanged.

**Tasks**

- [x] tokenizer: identifiers, quoted identifiers (`"x"`, `` `x` ``, `[x]`),
      string literals, blobs, numbers, `--` and `/* */` comments, operators,
      punctuation, parameters (`?`, `:name`, `@name`, `$name`)
- [x] `SqlContext` derivation: kind, the partial word under the cursor, the
      `FROM`/`JOIN` table set with aliases, and the column under comparison
- [x] contexts: `CTX_DOT_COMMAND`, `CTX_DOT_ARG`, `CTX_STATEMENT_START`,
      `CTX_SELECT_LIST`, `CTX_TABLE`, `CTX_COLUMN`, `CTX_VALUE`, `CTX_FUNCTION`,
      `CTX_PRAGMA`, `CTX_KEYWORD`, `CTX_UNKNOWN`
- [x] token kinds exposed in the header for the highlighter's later use
- [x] table-driven tests over (input, cursor) → expected context

**Checks**

*Automatic*

- [x] `make gate` → PASS
- [x] the context table passes, including aliases, `JOIN`, and a subquery in
      `FROM`
- [x] negative: cursor inside a string literal, a comment, or a quoted
      identifier yields `CTX_UNKNOWN`, not a plausible-looking wrong context
- [x] fuzz: random byte strings at every cursor offset over a fixed corpus
      terminate with no crash and no ASan finding

**Dependencies** — Phase 0. Independent of Phase 1.

---

### Phase 3: Candidate generation `[x]`

**Description**

`src/comp.c` plus the introspection half of `src/db.c`. Context in, candidate
list out. Scoping is the point: `SELECT * FROM companies WHERE <tab>` must
offer the columns of `companies`, not every column in the database.

**Tasks**

- [x] schema introspection: tables, views, indexes, columns, types; cached,
      invalidated when a statement modifies the schema
- [x] runtime-enumerated sources: keywords (`sqlite3_keyword_name`), functions
      (`pragma_function_list`), pragmas (`pragma_pragma_list`)
- [x] candidate source per context, alias-qualified where relevant
- [x] prefix filtering: case-insensitive match, case-preserving insertion
- [x] value completion: `SELECT DISTINCT <col> FROM <tbl> LIMIT 200`,
      identifiers quoted, aborted after ~150 ms by a
      `sqlite3_progress_handler`, cached per `(table, column)`
- [x] candidates carry a description (type, or `table`/`view`/`function`) and a
      group label for the menu

**Checks**

*Automatic*

- [x] `make gate` → PASS
- [x] per-context expectations against `tests/test.db`, including the fixture's
      reserved-word column and the identifier needing quoting
- [x] negative: a column of a table *not* in the `FROM` set is absent
- [x] keyword/function/pragma counts match what the linked library reports, so
      the lists cannot silently go stale
- [x] a value query against a synthetic table large enough to exceed the time
      limit returns promptly with no candidates — asserted on elapsed time
- [x] the cache is invalidated by DDL (add a column, complete again, see it)

**Dependencies** — Phase 2.

**Notes / Risks**

Value completion is the only context that reads user data. The caps, the time
limit and the truncation notice are what make it safe to leave on by default.

**Outcome.** `db.c` grew the introspection half — `DbList`, a ring-buffer cache
per source keyed by name and invalidated by the `PRAGMA schema_version` cookie,
and the four runtime-enumerated sources. `comp.c` turns a `SqlContext` into a
sorted, deduplicated, prefix-filtered candidate list, quoting identifiers that
need it (`"order"`, `"total amount"`) and single-quoting non-numeric values.
Value completion is bounded twice: `LIMIT 201` and a `clock_gettime`-based
deadline enforced from a `sqlite3_progress_handler`; a table of 400k rows
returns in milliseconds with `truncated` set. 15 tests in `tests/test_comp.c`,
54 in total, gate PASS.

---

### Phase 4: The completion menu `[x]`

**Description**

`src/menu.c`. The feature the project exists for, rendered richly: descriptions
beside candidates, the matched prefix highlighted, group headers separating
tables from columns from keywords, and a highlighted selection.

**Tasks**

- [x] render below the prompt, multi-column, sized to the terminal
- [x] navigation: `Tab` `Shift-Tab` `←` `→` `↑` `↓` `Ctrl-N` `Ctrl-P`
- [x] `Enter` accepts, `Esc`/`Ctrl-G` dismisses, typing narrows in place
- [x] a unique candidate is inserted without ever drawing a menu
- [x] description column, matched-prefix emphasis, group headers, selected-row
      highlight — all colours from `theme.c`
- [x] truncation shown explicitly when a cap was hit
- [x] scrolling when candidates exceed the available rows
- [x] `SIGWINCH` re-layout of an open menu

**Checks**

*Automatic*

- [x] `make gate` → PASS
- [x] pty tests for the four headline scenarios: `<tab>`, `.tables <tab>`,
      `SELECT <tab> FROM <tab>`, `WHERE <tab>` — asserted on the buffer after
      accepting a selection
- [x] typing after `<tab>` narrows rather than dismisses
- [x] `Esc` restores the buffer and the screen exactly as before the menu
- [x] a single candidate inserts with no menu drawn (assert absence of menu
      output in the capture)
- [x] with `NO_COLOR` set, the capture contains no SGR sequences

*Manual*

- [ ] The menu feels like zsh's: no flicker, correct placement near the bottom
      of the screen, readable columns

**Dependencies** — Phases 1 and 3.

**Notes / Risks**

`theme.c` was brought forward from Phase 5: the menu needs styles, and a
private palette in `menu.c` would have had to be deleted a phase later. Only
the capability rules and the menu's styles exist so far; the output modes' add
to the same table.

**Outcome.** The key bindings live in `edit.c`, which grew a completion state
and six actions, so there is one decoder for escape sequences and the menu
bindings are testable without a pty. `menu.c` is told its size and returns the
bytes to draw, which puts layout, scrolling and selection arithmetic under
ordinary unit tests; `line.c` writes them, erases with `0J` so a shrinking menu
leaves nothing behind, and moves the cursor back up to the prompt. Typing
narrows by regenerating the list on every buffer change, and the menu closes
itself when nothing matches. `main.c` supplies the generator as a `LineCompleter`
struct, so `line.c` never sees a database handle. 73 tests, gate PASS.

---

### Phase 5: Output modes, colour and compatibility `[x]`

**Description**

`src/out.c`, `src/width.c` and `src/theme.c`. Upstream's output modes, the
beautiful defaults, and the switch that makes the shell byte-compatible when
asked.

Formatting moves out of `db.c`, which keeps the handle, execution and
introspection. Nothing above `db.c` includes `<sqlite3.h>`, and `db.c` itself
decides nothing about appearance.

**Tasks**

- [x] the 13 classic modes: `ascii` `box` `column` `csv` `html` `insert` `json`
      `line` `list` `markdown` `quote` `table` `tabs`
- [x] the remaining presets 3.53.3 accepts: `c` `count` `jatom` `jobject` `off`
      `psql` `qbox` `split` `tcl` — 22 in total (see Notes on `www`)
- [x] `.mode` option flags: `--wrap` `--wordwrap` `--ww` `--quote` `--noquote`
      `--colsep` `--rowsep` `--escape` `--border` `--align` `--charlimit`
      `--linelimit` `--titlelimit` `--limits` `--tablename` `--multiinsert`
      `--blob-quote` `--title` `--nulls`
- [x] Unicode box drawing with ASCII fallback when the locale is not UTF-8
- [x] UTF-8 display width (wide and combining characters) for alignment —
      `width.c`, shared with the completion menu
- [x] `theme.c`: capability detection honouring `NO_COLOR`, `TERM=dumb` and
      non-tty; a default palette with headers bold, NULLs dim, and integers,
      reals, text and blobs distinguished
- [x] **pretty by default**: interactive *and* piped output uses `box` with
      headers and colour where the terminal supports it
- [x] `--compat` (and `-compat`): restores upstream defaults exactly — `list`
      mode, `|` separator, headers off, no colour, no box
- [x] screen-width shrinking: `.mode --sw`/`--screenwidth off|auto|N` and the
      startup `-screenwidth N`/`-sw N` flags, porting upstream's
      `qrfRestrictScreenWidth` (give up the margin first, then repeatedly
      halve the widest non-fixed column) against our own border geometry
      instead of its per-style formulas, so it falls out the same for box,
      table, plain, markdown and column. Headers wrap the same way data cells
      do (`columnar_header` now drives through `split_cell`, like
      `columnar_row`), so a shrunk column's border still lines up. Enhanced
      (non-`--compat`) mode auto-detects the terminal width via
      `ioctl(TIOCGWINSZ)` and re-reads it before every result, so a live
      resize is honoured; `--compat` and an explicit width both turn
      auto-detection back off.

**Checks**

*Automatic*

- [x] `make gate` → PASS
- [x] **differential suite**: `tests/parity.sh` runs 8 fixture queries across
      the 13 classic modes, plus a headers-on/off pass, and diffs
      `redstone --compat` byte-for-byte against `sqlite3(1)`. 112 checks, all
      passing. Wired into the gate; skippable only with an explicit
      `SKIP_PARITY=1`.
- [x] without `--compat`, output is box-formatted with headers — asserted
      positively, so a regression to upstream defaults fails
- [x] no SGR sequence is emitted when colour is off, asserted on `box` and
      `list` output over an int/text/NULL row. `NO_COLOR` and `TERM=dumb`
      themselves are covered a layer down, in the `theme` and `menu` suites.
- [x] a CJK string and a combining-character string align correctly in `box`
      and `column` (byte-compare against expected)
- [x] `LC_ALL=C` falls back to ASCII borders, still aligned
- [x] `--screenwidth`/`--sw off|auto|N` parsing, a narrow screen wrapping a
      wide column onto more lines than the unrestricted layout, `off`
      restoring it, and a `--widths`-pinned column staying untouched while its
      unpinned neighbour gives up the width instead

*Manual*

- [ ] The default output is genuinely nicer to read than `sqlite3(1)`'s

**Design decisions**

- Decision: **Pretty by default everywhere; upstream behaviour behind
  `--compat`.**
  Rationale: owner's call. The shell should look good without configuration,
  and a script that needs stability opts in explicitly.
  Alternatives: pretty only when stdout is a tty, which keeps pipes compatible
  automatically; always upstream with beauty opt-in.
  Trade-offs: a script that pipes `redstone` without `--compat` sees box drawing
  where it expected `|`-separated rows. Accepted deliberately — `redstone` is not
  `sqlite3` unless asked to be, and the failure is loud rather than subtle.
  The differential suite guarantees `--compat` is exact.

- Decision: **`-compat` is free in the upstream flag set** (checked against
  `sqlite3 --help`), so it collides with nothing.

- Decision: **Own formatter, classic subset exact.** `out.c` is written from
  the spec rather than transliterated from `shell.c`. Byte parity is
  *guaranteed and gate-enforced* for the 13 classic modes; the other nine are
  best-effort, since they are rare and several have no stable contract.
  Rationale: a transliteration would drag upstream's global state and its
  30-year accretion into a module meant to be readable in one sitting, and the
  differential suite pins the behaviour that actually matters either way.
  Alternatives: vendor upstream's renderer wholesale (exact everywhere, but
  unreadable and unowned); parity for every mode (weeks of work for modes
  nobody runs).
  Trade-offs: a script relying on `--compat -jatom` could see a difference.
  Recorded as a known gap below rather than silently ignored.

- Decision: **The linked 3.53.3 binary is the parity oracle, not
  `reference/shell.c`.** The reference file is a trunk snapshot and disagrees
  with the shipped binary in at least two places (`www`; `html` NULL text).
  Where they differ, the binary wins and the difference is recorded in
  `docs/notes/phase5-parity-findings.md`.

**Known gaps**

| Gap | Why | Escape hatch |
|---|---|---|
| Byte parity is asserted only for the 13 classic modes | `count` `jatom` `jobject` `off` `psql` `qbox` `split` `tcl` `c` are rare and several have no stable output contract | `sqlite3(1)` itself, for a script that needs one of them exactly |
| `www` mode is absent | 3.53.3's binary rejects `.mode www` ("unknown mode") although trunk `shell.c` carries the preset. Implementing it would mean shipping behaviour our own oracle calls an error | Add the preset row when a release actually accepts it |
| `-insert`, `-tcl`, `-qbox` are refused as *command-line* flags | Upstream accepts them only via `.mode`, not on the command line; accepting them would be a parity break in the permissive direction | `.mode insert` etc. once the shell is running |
| A blob containing an embedded NUL prints truncated under `--blob-quote text` | The mode asks for the bytes as text, and C strings end at the NUL. Upstream truncates identically | `--blob-quote hex` or `sql` |

**Dependencies** — Phase 0. Phase 4 consumed `theme.c`, so it landed with a
minimal palette that this phase completes.

**Notes / Risks**

Two real bugs were paid for during the differential work, both found by the
suite rather than by reading: `width_char` searched its table with unsigned
indices, so `last - 1` wrapped at index 0 and the search spun forever on a
combining mark; and the streaming styles increment the row count without
allocating cells, which made `free_result` walk a NULL array (found by
`make asan`). Both are now covered by unit tests.

**Outcome.** `out.c` is one spec with 22 presets over it, not 22 renderers:
a style, three encodings (text, title, blob), a control-character escape, the
separators, the border, per-column widths and alignments, and four limits.
Adding a mode is adding a row to `g_preset[]`. The encoders compose the way
upstream's do — every encoder's output goes through the control-character
escape, which is the rule that took longest to find and fixed `insert`,
`quote` and `csv` at once.

The differential suite is what made this tractable: 112 byte-exact diffs
against the real `sqlite3` turned parity from a reading exercise into a
failing test, and it is now a gate step, so a future change to a shared
encoder cannot quietly break a mode nobody was looking at. It also settled
three questions the source could not: `.mode www` is rejected by the shipped
binary, `html` prints `null` regardless of `-nullvalue`, and the command-line
mode flags are a strict subset of the `.mode` names.

`width.c` came out of the columnar work and immediately replaced the private
width helper in `menu.c` — along with its truncation companion, which shared
the same width model and would have desynced from it. `db.c` lost ~360 lines
and now decides nothing about appearance. 104 tests, 112 parity checks, gate
PASS.

### Phase 6: Dot-command and CLI parity `[✓]`

**Description**

The dot-command surface, split across four modules rather than the single
`dot.c` the plan named: `shell.c` (the session), `dot.c` (the table, the
splitter and the one-setting commands), `schema.c` (the introspection
commands) and `import.c` (`.import` and friends). Plus upstream's
command-line flags, and `main.c` reduced to argument parsing.

**Tasks**

- [✓] table-driven dispatch, one entry per command: name, arity, help text,
      argument-completion source — 75 rows, 63 implemented and 12 refused
- [✓] the 54 portable commands of upstream's `.help`, plus the aliases it
      accepts but hides (`.ar` `.crnl` `.indices` `.limits` `.vfsinfo`) and
      three of redstone's own (`.editor`, and Phase 7 adds `.theme`)
- [✓] the 11 unsupported commands (`.archive`/`.ar` `.check` `.expert`
      `.imposter` `.intck` `.recover` `.scanstats` `.selftest` `.session`
      `.sha3sum` `.testcase`) recognised and refused with a message naming the
      extension source they need; listed as such in `.help`
- [✓] upstream CLI flags: `-init` `-echo` `-header` `-bail` `-batch`
      `-interactive` `-readonly` `-safe` `-nonce` `-cmd` `-separator`
      `-nullvalue` `-newline` `-vfs` `-noinit` and the twelve mode shorthands.
      `-memtrace`, `-deserialize`/`-maxsize`, `-append`, `-zip`/`-A`,
      `-multiplex` and the five allocator-tuning flags are refused by name
      with the reason, on the same principle as the dot commands
- [✓] `~/.sqliterc` read at startup for parity, then
      `$XDG_CONFIG_HOME/redstone/redstonerc` so ours wins on conflict; `-noinit`
      skips both
- [✓] dot-command and argument values are completable (Phase 3's
      `CTX_DOT_ARG` is fed by `dot_comp_source()`)
- [✓] remove the Phase 0 stub from `main.c`

**Checks**

*Automatic*

- [✓] `make gate` → PASS (118 tests, 138 parity checks)
- [✓] every command has a test or a parity check asserting its **effect**: the
      formatting and introspection commands are diffed against `sqlite3(1)`
      byte for byte, the settings commands are asserted through their effect on
      the next statement's output in `tests/test_dot.c`
- [✓] every one of the 11 refused commands reports failure with a message
      naming the extension (`test_refusals`)
- [✓] differential: `.tables` `.indexes` `.databases` `.schema` (plain,
      `--indent`, `--nosys`, per-object) `.fullschema` `.dump` (plain,
      `--data-only`, `--nosys`, per-table) `.show` `.dbinfo` `.limit`
      `.dbconfig` `.lint fkey-indexes` all match `sqlite3(1)` byte for byte
      under `--compat`, plus a `.dump` round-trip through both shells
- [✓] `.import` round-trips a CSV containing embedded commas, quotes and
      newlines — as a unit test and as a parity check against `sqlite3(1)`
- [✓] negative: `.open` on a nonexistent path reports an error and leaves the
      previous database usable; an unknown dot command suggests the nearest
      match
- [ ] `-noinit` suppresses a `~/.sqliterc` that would otherwise be visible
      — **not automated**: it needs a planted file in a real `$HOME`, which the
      suite will not write. Moved to the manual checks below

*Manual*

- [ ] `alias sqlite3=redstone` for a day's work surfaces nothing missing

**Design decisions**

- Decision: **Refuse the 11 extension-backed commands explicitly rather than
  omitting them.**
  Rationale: owner's call. A recognised command that explains why it cannot run
  is a better failure than "unknown command", and it keeps the parity claim
  honest — the gap is documented in `.help` rather than discovered.
  Trade-offs: not literally 1:1. Vendoring the extensions would cost tens of
  thousands of lines nobody here wrote, which is what the clean-room decision
  exists to avoid.
- Decision: **Four modules, not one `dot.c`** (owner, Phase 6 planning:
  "Three files" plus "New `src/shell.c`").
  Rationale: the introspection commands are long byte-exact ports and `.import`
  is a parser; keeping them out of the dispatch table leaves `dot.c` readable.
  Trade-offs: four headers instead of one, and a `Shell` accessor for every
  piece of session state a command touches.
- Decision: **Raise the `src/` line budget to ~10,000** (owner, Phase 6
  planning). Outcome below: it was not enough either.

**Known gaps**

| Gap | Why | Escape hatch |
|---|---|---|
| `.import` does not skip a UTF-8 BOM on the first field | Upstream does; the port did not carry it | Strip the BOM before importing |
| `.import`'s duplicate-column renaming is an approximation | Upstream's `zAutoColumn` is a small SQL program that also chops redundant suffixes; ours appends `_N` | Name the columns in the CSV header |
| `.import` has no `-esc`/`-qesc` backslash-escape option | Rarely used, and it interacts with every other quoting rule | `sqlite3(1)` |
| `.vfslist` differs | The oracle binary registers `apndvfs`, which redstone deliberately does not vendor | None; the list is honest about what is linked |

**Dependencies** — Phase 5 for `.mode`; Phase 3 for completable arguments.

**Notes / Risks**

Three real defects were found by writing the tests rather than by reading the
code, which is the argument for the differential suite in one paragraph:

- `.import`'s CSV reader destroyed every quoted field. The trim-back after a
  closing quote was a loop scanning for the *previous* `"` in the buffer, so
  `"one,two"` imported as the empty string. Nothing above the reader could
  have noticed; the formatting parity checks all passed.
- `.dump` never emitted `CREATE TABLE IF NOT EXISTS`. Upstream rewrites the
  statement when the table name is quoted — exactly the tables `.import`
  generates — so a dump of an imported table would not replay into a database
  that already had it.
- `shell_set_prompt` freed `sh->nonce`, an unrelated field, on every
  `.prompt`.

The connection settings were a subtler one. `redstone` accepted `SELECT "foo"`
where the oracle rejected it, because upstream compiles its own SQLite with
`-DSQLITE_DQS=0` while redstone links a shared library that may be built either
way. `db.c` now applies the same `sqlite3_db_config` set upstream's `open_db`
does — DQS off, defensive on, trusted-schema off — on the first open and on
every `.open`, so the behaviour no longer depends on how the library was
built.

**Outcome.** `main.c` is 352 lines of argument parsing; everything else moved
behind `shell.c`, which owns the connection, the redirect and the switches, so
no dot command needs a global. The three command modules total 3,704 lines,
and that is the honest cost of parity: `schema.c` alone is 1,623, because
`.dump` and `.schema` have to reproduce upstream's output character for
character.

That puts `src/` at 12,877 lines against the ~10,000 the owner set at the
start of this phase. The criterion is **missed, not met**. Nothing here is
padding — the parity suite would catch a simplification that changed output —
so closing the gap means dropping a capability rather than tightening code,
which is the owner's call, not the agent's.

118 tests, 138 parity checks, cppcheck and clang-tidy clean, gate PASS.

---

### Phase 7: Syntax highlighting and theming `[x]`

**Description**

`src/hl.c` plus the theme-file half of `src/theme.c`. The buffer is coloured as
it is typed, and none of the colour in the program is hardcoded any more.

**Tasks**

- [x] `hl.c`: map `sqlctx.c` token kinds to theme colours; re-render the buffer
      on every edit
- [x] highlight keywords, functions, strings, numbers, comments, parameters and
      quoted identifiers distinctly — plus, beyond the original plan, tables,
      views and in-scope columns, coloured from the completion engine's caches
- [x] unbalanced quote or paren shown as an error colour, which is also the
      cue for why the prompt is asking for a continuation line
- [x] theme file at `$XDG_CONFIG_HOME/redstone/theme` (default
      `~/.config/redstone/theme`): `[section]` headers over `key = value` lines,
      unknown sections, keys and values warned about and ignored, missing file
      means the built-in palette
- [x] `.theme` dot command: dump, `list`, `reload`, `on`/`off`, or load a
      built-in palette or a file by name
- [x] four built-in themes — `default`, `dark`, `light` and the 16-colour-safe
      `basic`

**Checks**

*Automatic*

- [x] `make gate` → PASS (138 unit tests, 138 parity checks)
- [x] pty test: typing a statement produces the expected SGR sequence at each
      token boundary — `test_pty_highlight`, plus `test_token_kinds`, which
      asserts one style per token kind without needing a terminal
- [x] highlighting adds no visible latency — assert redraw stays under a fixed
      budget on a long line (`test_long_line_is_fast`: 20 renders of a 4 KB
      line, 20 ms each)
- [x] a malformed theme file is diagnosed and falls back to the built-in
      palette rather than failing to start (`test_bad_input_is_diagnosed`)
- [x] `NO_COLOR` disables highlighting as well as output colour — the pty tests
      now install a highlighter, so `test_pty_no_color` covers both

*Manual*

- [ ] Colours are legible on both the owner's dark and light terminal profiles

**Design decisions**

- Decision: **Highlighting reuses the Phase 2 tokenizer rather than its own
  lexer.** Two lexers would drift, and the tokenizer already reports the kinds
  the highlighter needs — which is why Phase 2 exposes token kinds in its
  header.

- Decision: **A theme file is now in scope**, promoted from the deferred log,
  because with colour in the output, the menu and the editor there is finally
  something worth configuring.

- Decision (owner, Phase 7 planning): **INI with `[section]` headers** —
  `[menu]`, `[syntax]`, `[output]` — rather than one flat namespace. The three
  groups are what a user thinks in, and the section is what makes `menu.value`
  and `output.string` readable as two different things.

- Decision (owner, Phase 7 planning): **tokens plus schema awareness.** A name
  the database knows is coloured as what it is — table, view, column — and a
  name it does not know stays plain. This is the feature: an unknown name is
  visible as a typo before the statement is ever run. The lookups run against
  `db.c`'s existing caches only, never a query, because they are asked once per
  identifier per keystroke.

- Decision: **`hl.c` is database-unaware**, reached through an `HlSchema` of
  four callbacks, exactly as `line.c` reaches the completer. The layering rule
  holds — nothing above `db.c` includes `<sqlite3.h>` — and the highlighter is
  testable against a stub schema.

- Decision: **the built-in palettes are theme-file text inside the binary**,
  not files installed into `themes/`. The roadmap said "two built-in themes";
  what shipped is four, parsed by the same parser a user's file goes through.
  There is nothing to install, nothing to lose, and `theme_dump` round-trips —
  `.theme > ~/.config/redstone/theme` is a working way to start editing one, and a
  test asserts the round trip for every shipped palette.

- Decision: **`#` comments a whole line only; `--` comments to end of line.**
  A value is the one place a user writes `#rrggbb`, so `#` cannot mean "comment
  from here" without eating colours. `--` is what SQL uses, which is what the
  audience already knows.

- Decision: **`-noinit` suppresses the theme file too.** It already suppresses
  `~/.sqliterc`; "no configuration" has to mean all of it, or the flag is a
  half-truth when a session comes back coloured.

**Dependencies** — Phases 1, 2, 5.

---

### Phase 8: Hardening and release `[✓]`

**Description**

Close out: documentation, remaining edge cases, and everything earlier phases
deliberately postponed.

**Tasks**

- [✓] `README.md`: build, usage, keybindings, the parity table
- [✓] `redstone(1)` man page (`docs/redstone.1`)
- [✓] `make valgrind` extended to cover the pty suite
- [✓] input lines longer than the terminal width, verified under the pty
      (`test_pty_long_line_redraw`, `tests/test_line.c`: a 40-character line
      against a 20-column pty, edited at the far end from the cursor)
- [✓] a fuzz target over the tokenizer run in CI-length batches (`make fuzz`,
      `tests/fuzz_tokenizer.c`, default `FUZZ_TIME=30`)
- [✓] `make install` installs `redstone` and the man page only — never as, or
      symlinked to, `sqlite3`, which would shadow the binary the parity suite
      tests against
- [✓] review the Deferred-work log; promote or close each entry

**Checks**

*Automatic*

- [✓] `make gate` → PASS (143 tests, 138 parity checks, cppcheck and
      clang-tidy clean)
- [✓] `make valgrind` reports 0 errors across the whole suite — including the
      pty/fork-based `test_sigterm_restores_termios`, now reached via
      `--trace-children=yes` over an uninstrumented `MODE=debug` build (ASan
      and valgrind both intercept malloc and cannot run one under the other)
- [✓] a clean checkout builds both backends from `nix develop` with no network
      access beyond the flake inputs — exercised by `make gate`, which builds
      system and vendored
- [✓] the documented parity table is generated from `dot.c`'s dispatch table:
      `.help` prints directly from `g_cmd[]` (see Phase 6), so the man page and
      README point to it rather than duplicating a list that could drift

*Manual*

- [ ] Owner sign-off on the README and man page

---

### Phase 9: Identity, Windows and publication `[x]`

**Description**

Rename to redstone with its own identity shared with librarian
(`docs/family.md`), port to Windows 10 1809+ behind a platform layer, and
prepare the repository for publication: CI, release archives, winget, demo.

**Tasks**

- [x] Rename to redstone; redstone-ore banner and `● redstone ❯` prompt
      (`src/brand.c`, `[brand]` theme slots); SVG logo from the same bitmap;
      no fallback to `~/.config/sqlsh`
- [x] Theme split into the shared engine (`theme.c`/`theme.h`) and the
      per-program slots (`theme_slots.h`, `theme_builtin.c`)
- [x] `include/plat.h` with `plat_posix.c`/`plat_win32.c`; every POSIX call
      moved behind it; UTF-8 argv, VT console, binary stdio on Windows;
      `plat_mkdir_p` and `plat_nprocs` for librarian
- [x] `nix develop .#windows`: MinGW-w64 static cross-build, tests under Wine
- [x] `.github/workflows/ci.yml` (Linux gate; native MSYS2 build and tests)
      and `release.yml` (tar.gz, zip, SHA256SUMS on `v*` tags)
- [x] `packaging/winget/0.1.0/` manifest set and per-release steps
- [x] README per `docs/family.md`: Install, Platforms, License, demo
      (`docs/demo.tape` -> `docs/demo.gif`)

**Checks**

*Automatic*

- [x] `make gate` -> PASS (162 tests, 138 parity checks)
- [x] Windows cross-build with `-Werror`; 142 tests pass under Wine (the
      `.edit` tests need a `/bin/sh` editor stub and are POSIX-only)
- [x] CI green on GitHub for both jobs (run 37240885649)

*Manual*

- [ ] Interactive use on real Windows: Windows Terminal, `cmd.exe`,
      PowerShell (banner, completion menu, resize, Ctrl-C, history)
- [ ] First tagged release; winget hash filled in and submitted

---

## Deferred work

Recorded so each is a choice rather than an omission. Reconsidered after
Phase 8.

| Item | Raised | Status |
|---|---|---|
| Syntax highlighting while typing | Phase 0 planning | **Promoted** to Phase 7. |
| Configuration / theme file | Phase 0 planning | **Promoted** to Phase 7. |
| UTF-8 display width | Phase 1 decision | **Scheduled** into Phase 5. |
| `dot.c` extraction | Phase 0 implementation | **Scheduled** into Phase 6. |
| `.import` / `.dump` | Phase 0 planning | **Promoted** to Phase 6 — parity requires them. |
| Vi keybindings | Phase 1 planning | **Promoted** into Phase 1 — modality cannot be retrofitted cheaply. |
| Vendoring the 11 extension-backed commands | Phase 6 planning | **Closed**, Phase 8 review. Phase 6 already ships refusal-with-message for all 11; vendoring them would mean owning tens of thousands of lines of `sqlar.c`/`zipfile.c`/etc. nobody here wrote, against the clean-room decision Phase 0 made. |
| Multiple attached databases in completion scoping | Phase 0 planning | **Closed**, Phase 8 review, out of scope for v1. `sqlctx.c` would need schema-qualified name resolution across every attached database's live schema; no user request has surfaced needing it. |
| Query result paging | Phase 0 planning | **Closed**, Phase 8 review. `reference/shell.c` has no pager feature at all under this or trunk versions of upstream — it was never a parity gap, only an early planning idea. `redstone ... \| less` already covers it externally. |

## Manual checks outstanding

Checks no agent can honestly perform. Each phase is committed on a green
automatic gate; these boxes stay unticked until the owner has looked. Populated
as phases land.

| Phase | Check | How to reproduce |
|---|---|---|
| 5 | The default output is genuinely nicer to read than `sqlite3(1)`'s | `make && ./bin/redstone tests/test.db`, then `SELECT * FROM employees LIMIT 20;`. Compare against `sqlite3 tests/test.db` running the same query, and against `./bin/redstone --compat`. Check a NULL-heavy and a blob-heavy table too. |
| 4 | The menu feels like zsh's: no flicker, correct placement near the bottom of the screen, readable columns | `make && ./bin/redstone tests/test.db`, then type `SELECT * FROM ` and press Tab. Repeat with the window scrolled so the prompt is on the last row, and with a narrow window. |
| 6 | `alias sqlite3=redstone` for a day's work surfaces nothing missing | `make && alias sqlite3=$PWD/bin/redstone`, then use it for whatever the day brings. Anything that behaves differently from the real `sqlite3(1)` is a parity bug worth a line in the next phase. |
| 7 | Colours are legible on both the owner's dark and light terminal profiles | `make && ./bin/redstone tests/test.db`, then type a statement mixing known and unknown names, e.g. `SELECT id, nosuch FROM employees WHERE 'x'`. Try `.theme dark`, `.theme light` and `.theme basic` under each terminal profile, and `.theme` to see the palette as a file. |
| 6 | `-noinit` suppresses a `~/.sqliterc` that would otherwise be visible | Put `.mode box` in `~/.sqliterc`, run `./bin/redstone tests/test.db "SELECT 1;"` (box) and `./bin/redstone -noinit tests/test.db "SELECT 1;"` (list). Not automated: the suite will not plant files in a real `$HOME`. |
| 5 | Resizing the terminal mid-session actually re-wraps the next result, live | `make && ./bin/redstone tests/test.db`, run a query with a wide row (`SELECT * FROM employees;`), then narrow the terminal window and re-run it without restarting `redstone`. The column shrinking should track the new width. Not automated: `ioctl(TIOCGWINSZ)` needs a real controlling terminal. |
| 9 | redstone works interactively on real Windows 10/11 | Install from the release zip (or `winget install --manifest packaging/winget/X.Y.Z`), then run `redstone` in Windows Terminal, `cmd.exe` and PowerShell. Check the banner and colours, Tab completion menu, arrow keys, resizing the window, Ctrl-C on a line, `.edit` opening notepad, and that history survives a restart (`%LOCALAPPDATA%\redstone\`). |
| 8 | Owner sign-off on `README.md` and `docs/redstone.1` | Read both; check they match how the shell actually behaves. Not automated by design — the check is a human judgement of the docs' quality and accuracy, not a scriptable property. |
