---
title: "sqlsh — a drop-in SQLite shell with zsh-style completion"
id: "sqlsh"
status: in-progress           # initiated | defined | in-research | in-progress | waiting | completed
priority: medium              # critical | high | medium | low
start_date: "2026-09-15"
target_date: ""
last_updated: "2026-09-16"
owner: "rs"
stakeholders: []
tags: [c, sqlite, cli, suckless, terminal, completion]
depends_on: []
blocks: []
references:
  - "docs/ARCHITECTURE.md"
  - "docs/development-workflow.md"
  - "https://sqlite.org/c3ref/intro.html"
  - "https://sqlite.org/cli.html"
notes: "Phases 0-1 complete and gate-green. Phase 2 (sqlctx.c) next."
---

# sqlsh

## Overview

**Purpose.** `sqlite3(1)` is a capable shell with a poor interactive surface: no
completion worth the name, so every session involves recalling table names,
column names and legal values from memory or from a second terminal. `sqlsh` is
a **drop-in replacement** for it — same dot commands, same output modes, same
CLI flags — with two things added on top: **zsh-style completion**, where
`<Tab>` opens a navigable menu of candidates drawn from the live schema and
data, and a **presentation layer worth looking at**.

**Context.** C99 against the public `sqlite3` C API, under suckless
constraints: one library dependency, a small auditable codebase, no speculative
abstraction. The environment is a pinned Nix flake.

**Scope.** Everything `sqlite3(1)` does that is reachable through the public C
API — 44 of its 65 dot commands, all 13 output modes, and its command-line
flags — plus completion, colour and theming. The ~21 dot commands backed by
vendored extensions or internal APIs are recognised and refused with a pointer
to `sqlite3(1)`, never silently missing.

**Explicitly excluded**, by decision: bundling SQLite's extension sources
(`sqlar`, `zipfile`, `sha3`, `sqlite3expert`, `sqlite3recover`, `sqlite3_intck`,
`sqlite3session`, `dbdata`), a complete SQL parser, and Windows support.

## Goals

**Goals**

1. **Parity.** A user can alias `sqlite3` to `sqlsh` and not notice anything
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
- Every one of the 44 portable dot commands has a test asserting its effect;
  every one of the 21 unsupported commands is refused with a useful message.
- `sqlsh --compat` output is byte-identical to `sqlite3(1)` across the mode
  matrix, verified by differential test.
- `make gate` prints PASS: formatter, both builds, sanitized tests, cppcheck
  and clang-tidy clean, zero warnings.
- Total `src/` stays under roughly 6,000 lines. Upstream `shell.c` is 37,373.

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
| `_FORTIFY_SOURCE` glibc `#warning` | `flake.nix` `hardeningDisable`, `Makefile` `TIDY_EXTRA` | nix's cc-wrapper injects it; glibc then warns at the `-O0` used by debug and compdb builds. Environmental, not ours. |

## Architecture

Detailed design lives in `docs/ARCHITECTURE.md`; this is the shape.

**Structure.** One concern per module — a `.c` in `src/`, a header in
`include/` — with dependencies pointing one way and no module reaching into
another's internals.

```
main.c      argument parsing (upstream flag set), REPL driver, signals
  |
  +-- dot.c     dot-command table and dispatch (44 + 21 refusals)
  |     |
  |     +-- out.c    the 13 output modes, box drawing, colour
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
| `src/main.c` | implemented | Argument parsing, REPL loop, prompts. The dot stub moves to `dot.c` in Phase 6. |
| `src/edit.c` · `include/edit.h` | implemented | The editing core: buffer, cursor, emacs and vi keymaps, history ring. Performs no I/O, so every keybinding is unit-testable. |
| `src/line.c` · `include/line.h` | implemented | Terminal layer: termios raw mode, signal-safe restoration, redraw, escape timeout, history file. |
| `src/sqlctx.c` | Phase 2 | Tokenizer and cursor-context machine. Pure. |
| `src/comp.c` | Phase 3 | Context → candidate list. Pure given a `Db`. |
| `src/menu.c` | Phase 4 | The navigable menu. |
| `src/out.c` | Phase 5 | 13 output modes, box drawing, type-aware colour. |
| `src/theme.c` | Phase 5 | Colour capability detection; theme file in Phase 7. |
| `src/dot.c` | Phase 6 | Dot-command table, dispatch, refusals. |
| `src/hl.c` | Phase 7 | Syntax highlighting while typing. |

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
- [✓] `sqlsh tests/test.db 'SELECT ...'` prints rows; exit 0
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
      `$XDG_STATE_HOME/sqlsh/history` (default `~/.local/state/sqlsh/history`)
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
  `$XDG_STATE_HOME/sqlsh/history`, config and theme under
  `$XDG_CONFIG_HOME/sqlsh/`. `~/.sqliterc` is read for parity (Phase 6),
  followed by `~/.config/sqlsh/sqlshrc` so ours wins on conflict.

**Dependencies** — Phase 0.

**Notes / Risks**

Signal-safe restoration is the risky part: handlers may call only
async-signal-safe functions, ruling out `fprintf`. Plan is a flag plus
`write(2)` of a fixed reset sequence, with `tcsetattr` restoring the saved
termios.

---

### Phase 2: SQL context analysis `[ ]`

**Description**

`src/sqlctx.c`. Given a buffer and a cursor offset, decide what may legally
appear there. Pure, terminal-free, and the most testable part of the program.
Not a parser: completion needs to know *where the cursor is*, not what the
query means. Phase 7's highlighter reuses this tokenizer unchanged.

**Tasks**

- [ ] tokenizer: identifiers, quoted identifiers (`"x"`, `` `x` ``, `[x]`),
      string literals, blobs, numbers, `--` and `/* */` comments, operators,
      punctuation, parameters (`?`, `:name`, `@name`, `$name`)
- [ ] `SqlContext` derivation: kind, the partial word under the cursor, the
      `FROM`/`JOIN` table set with aliases, and the column under comparison
- [ ] contexts: `CTX_DOT_COMMAND`, `CTX_DOT_ARG`, `CTX_STATEMENT_START`,
      `CTX_SELECT_LIST`, `CTX_TABLE`, `CTX_COLUMN`, `CTX_VALUE`, `CTX_FUNCTION`,
      `CTX_PRAGMA`, `CTX_KEYWORD`, `CTX_UNKNOWN`
- [ ] token kinds exposed in the header for the highlighter's later use
- [ ] table-driven tests over (input, cursor) → expected context

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] the context table passes, including aliases, `JOIN`, and a subquery in
      `FROM`
- [ ] negative: cursor inside a string literal, a comment, or a quoted
      identifier yields `CTX_UNKNOWN`, not a plausible-looking wrong context
- [ ] fuzz: random byte strings at every cursor offset over a fixed corpus
      terminate with no crash and no ASan finding

**Dependencies** — Phase 0. Independent of Phase 1.

---

### Phase 3: Candidate generation `[ ]`

**Description**

`src/comp.c` plus the introspection half of `src/db.c`. Context in, candidate
list out. Scoping is the point: `SELECT * FROM companies WHERE <tab>` must
offer the columns of `companies`, not every column in the database.

**Tasks**

- [ ] schema introspection: tables, views, indexes, columns, types; cached,
      invalidated when a statement modifies the schema
- [ ] runtime-enumerated sources: keywords (`sqlite3_keyword_name`), functions
      (`pragma_function_list`), pragmas (`pragma_pragma_list`)
- [ ] candidate source per context, alias-qualified where relevant
- [ ] prefix filtering: case-insensitive match, case-preserving insertion
- [ ] value completion: `SELECT DISTINCT <col> FROM <tbl> LIMIT 200`,
      identifiers quoted, aborted after ~150 ms by a
      `sqlite3_progress_handler`, cached per `(table, column)`
- [ ] candidates carry a description (type, or `table`/`view`/`function`) and a
      group label for the menu

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] per-context expectations against `tests/test.db`, including the fixture's
      reserved-word column and the identifier needing quoting
- [ ] negative: a column of a table *not* in the `FROM` set is absent
- [ ] keyword/function/pragma counts match what the linked library reports, so
      the lists cannot silently go stale
- [ ] a value query against a synthetic table large enough to exceed the time
      limit returns promptly with no candidates — asserted on elapsed time
- [ ] the cache is invalidated by DDL (add a column, complete again, see it)

**Dependencies** — Phase 2.

**Notes / Risks**

Value completion is the only context that reads user data. The caps, the time
limit and the truncation notice are what make it safe to leave on by default.

---

### Phase 4: The completion menu `[ ]`

**Description**

`src/menu.c`. The feature the project exists for, rendered richly: descriptions
beside candidates, the matched prefix highlighted, group headers separating
tables from columns from keywords, and a highlighted selection.

**Tasks**

- [ ] render below the prompt, multi-column, sized to the terminal
- [ ] navigation: `Tab` `Shift-Tab` `←` `→` `↑` `↓` `Ctrl-N` `Ctrl-P`
- [ ] `Enter` accepts, `Esc`/`Ctrl-G` dismisses, typing narrows in place
- [ ] a unique candidate is inserted without ever drawing a menu
- [ ] description column, matched-prefix emphasis, group headers, selected-row
      highlight — all colours from `theme.c`
- [ ] truncation shown explicitly when a cap was hit
- [ ] scrolling when candidates exceed the available rows
- [ ] `SIGWINCH` re-layout of an open menu

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] pty tests for the four headline scenarios: `<tab>`, `.tables <tab>`,
      `SELECT <tab> FROM <tab>`, `WHERE <tab>` — asserted on the buffer after
      accepting a selection
- [ ] typing after `<tab>` narrows rather than dismisses
- [ ] `Esc` restores the buffer and the screen exactly as before the menu
- [ ] a single candidate inserts with no menu drawn (assert absence of menu
      output in the capture)
- [ ] with `NO_COLOR` set, the capture contains no SGR sequences

*Manual*

- [ ] The menu feels like zsh's: no flicker, correct placement near the bottom
      of the screen, readable columns

**Dependencies** — Phases 1 and 3.

---

### Phase 5: Output modes, colour and compatibility `[ ]`

**Description**

`src/out.c` and `src/theme.c`. All 13 upstream output modes, the beautiful
defaults, and the switch that makes the shell byte-compatible when asked.

Formatting moves out of `db.c`, which keeps the handle, execution and
introspection.

**Tasks**

- [ ] all 13 modes: `ascii` `box` `column` `csv` `html` `insert` `json` `line`
      `list` `markdown` `quote` `table` `tabs`
- [ ] `.mode` option flags: `--wrap` `--wordwrap` `--quote` `--noquote`
      `--colsep` `--rowsep` `--escape` `--border` `--align` `--charlimit`
      `--linelimit` `--titlelimit` `--tablename` `--multiinsert` `--blob-quote`
- [ ] Unicode box drawing with ASCII fallback when the locale is not UTF-8
- [ ] UTF-8 display width (wide and combining characters) for alignment
- [ ] `theme.c`: capability detection honouring `NO_COLOR`, `TERM=dumb` and
      non-tty; a default palette with headers bold, NULLs dim, and integers,
      reals, text and blobs distinguished
- [ ] **pretty by default**: interactive *and* piped output uses `box` with
      headers and colour where the terminal supports it
- [ ] `--compat` (and `-compat`): restores upstream defaults exactly — `list`
      mode, `|` separator, headers off, no colour, no box

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] **differential suite**: for every mode × a fixture query matrix,
      `sqlsh --compat` output is byte-identical to `sqlite3(1)` run with the
      same commands. This is the parity oracle and runs in the gate.
- [ ] without `--compat`, output is box-formatted with headers — asserted
      positively, so a regression to upstream defaults fails
- [ ] `NO_COLOR=1` and `TERM=dumb` each produce no SGR sequences
- [ ] a CJK string and a combining-character string align correctly in `box`
      and `column` (byte-compare against expected)
- [ ] `LC_ALL=C` falls back to ASCII borders, still aligned

*Manual*

- [ ] The default output is genuinely nicer to read than `sqlite3(1)`'s

**Design decisions**

- Decision: **Pretty by default everywhere; upstream behaviour behind
  `--compat`.**
  Rationale: owner's call. The shell should look good without configuration,
  and a script that needs stability opts in explicitly.
  Alternatives: pretty only when stdout is a tty, which keeps pipes compatible
  automatically; always upstream with beauty opt-in.
  Trade-offs: a script that pipes `sqlsh` without `--compat` sees box drawing
  where it expected `|`-separated rows. Accepted deliberately — `sqlsh` is not
  `sqlite3` unless asked to be, and the failure is loud rather than subtle.
  The differential suite guarantees `--compat` is exact.

- Decision: **`-compat` is free in the upstream flag set** (checked against
  `sqlite3 --help`), so it collides with nothing.

**Dependencies** — Phase 0. Phase 4 consumes `theme.c`, so if Phase 4 runs
first it lands with a minimal palette that this phase completes.

---

### Phase 6: Dot-command and CLI parity `[ ]`

**Description**

`src/dot.c`. The 44 dot commands reachable through the public API, the 21 that
are not, and upstream's command-line flags.

**Tasks**

- [ ] table-driven dispatch, one entry per command: name, arity, help text,
      argument-completion source
- [ ] the 44 portable commands: `.auth` `.backup` `.bail` `.cd` `.changes`
      `.connection` `.crlf` `.databases` `.dbconfig` `.dump` `.echo` `.eqp`
      `.excel` `.exit` `.explain` `.fullschema` `.headers` `.help` `.import`
      `.indexes` `.limit` `.load` `.log` `.mode` `.nullvalue` `.once` `.open`
      `.output` `.parameter` `.print` `.progress` `.prompt` `.quit` `.read`
      `.restore` `.save` `.schema` `.separator` `.shell` `.stats` `.system`
      `.tables` `.timeout` `.timer` `.trace` `.version` `.width`
- [ ] the 21 unsupported commands recognised and refused with a message naming
      the extension they need and pointing at `sqlite3(1)`; listed as such in
      `.help`
- [ ] upstream CLI flags: `-init` `-echo` `-header` `-bail` `-batch`
      `-interactive` `-readonly` `-safe` `-cmd` `-separator` `-nullvalue`
      `-newline` `-vfs` `-memtrace` `-stats` and the mode shorthands
      (`-box` `-column` `-csv` `-html` `-json` `-line` `-list` `-markdown`
      `-quote` `-table` `-tabs` `-ascii`)
- [ ] `~/.sqliterc` read at startup for parity, then
      `$XDG_CONFIG_HOME/sqlsh/sqlshrc` so ours wins on conflict; `-noinit`
      skips both
- [ ] dot-command and argument values are completable (feeds Phase 3's
      `CTX_DOT_ARG`)
- [ ] remove the Phase 0 stub from `main.c`

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] every one of the 44 has a test asserting its **effect**, not its exit code
- [ ] every one of the 21 exits non-zero with a message naming the extension
- [ ] differential: `.help` lists all 65; `.schema`, `.tables`, `.dump`,
      `.indexes`, `.databases`, `.fullschema` match `sqlite3(1)` byte for byte
      under `--compat`
- [ ] `.import` round-trips a CSV containing embedded commas, quotes and
      newlines
- [ ] negative: `.open` on a nonexistent path reports an error and leaves the
      previous database usable; an unknown dot command suggests the nearest
      match rather than only failing
- [ ] `-noinit` suppresses a `~/.sqliterc` that would otherwise be visible

*Manual*

- [ ] `alias sqlite3=sqlsh` for a day's work surfaces nothing missing

**Design decisions**

- Decision: **Refuse the 21 extension-backed commands explicitly rather than
  omitting them.**
  Rationale: owner's call. A recognised command that explains why it cannot run
  is a better failure than "unknown command", and it keeps the parity claim
  honest — the gap is documented in `.help` rather than discovered.
  Trade-offs: not literally 1:1. Vendoring the extensions would cost tens of
  thousands of lines nobody here wrote, which is what the clean-room decision
  exists to avoid.

**Dependencies** — Phase 5 for `.mode`; Phase 3 for completable arguments.

---

### Phase 7: Syntax highlighting and theming `[ ]`

**Description**

`src/hl.c` plus the theme-file half of `src/theme.c`. The buffer is coloured as
it is typed, and none of the colour in the program is hardcoded any more.

**Tasks**

- [ ] `hl.c`: map `sqlctx.c` token kinds to theme colours; re-render the buffer
      on every edit
- [ ] highlight keywords, functions, strings, numbers, comments, parameters and
      quoted identifiers distinctly
- [ ] unbalanced quote or paren shown as an error colour, which is also the
      cue for why the prompt is asking for a continuation line
- [ ] theme file at `$XDG_CONFIG_HOME/sqlsh/theme` (default
      `~/.config/sqlsh/theme`): `key = colour` lines, unknown keys
      warned about and ignored, missing file means the built-in palette
- [ ] `.theme` dot command to reload and to list the current palette
- [ ] two built-in themes, one for dark and one for light terminals

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] pty test: typing a statement produces the expected SGR sequence at each
      token boundary
- [ ] highlighting adds no visible latency — assert redraw stays under a fixed
      budget on a long line
- [ ] a malformed theme file is diagnosed and falls back to the built-in
      palette rather than failing to start
- [ ] `NO_COLOR` disables highlighting as well as output colour

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

**Dependencies** — Phases 1, 2, 5.

---

### Phase 8: Hardening and release `[ ]`

**Description**

Close out: documentation, remaining edge cases, and everything earlier phases
deliberately postponed.

**Tasks**

- [ ] `README.md`: build, usage, keybindings, the parity table
- [ ] `sqlsh(1)` man page
- [ ] `make valgrind` extended to cover the pty suite
- [ ] input lines longer than the terminal width, verified under the pty
- [ ] a fuzz target over the tokenizer run in CI-length batches
- [ ] `make install` installs `sqlsh` only — never as, or symlinked to,
      `sqlite3`, which would shadow the binary the parity suite tests against
- [ ] review the Deferred-work log; promote or close each entry

**Checks**

*Automatic*

- [ ] `make gate` → PASS
- [ ] `make valgrind` reports 0 errors across the whole suite
- [ ] a clean checkout builds both backends from `nix develop` with no network
      access beyond the flake inputs
- [ ] the documented parity table is generated from `dot.c`'s dispatch table,
      so it cannot drift from the code

*Manual*

- [ ] Owner sign-off on the README and man page

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
| Vendoring the 21 extension-backed commands | Phase 6 planning | Deferred indefinitely; refused with a message instead. |
| Multiple attached databases in completion scoping | Phase 0 planning | Deferred. `sqlctx` would need schema-qualified names. |
| Query result paging | Phase 0 planning | Deferred. An external pager may be the suckless answer. |

## Manual checks outstanding

Checks no agent can honestly perform. Each phase is committed on a green
automatic gate; these boxes stay unticked until the owner has looked. Populated
as phases land.

| Phase | Check | How to reproduce |
|---|---|---|
| — | — | — |
