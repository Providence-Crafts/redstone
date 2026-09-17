# Phase 6 plan: dot commands, CLI flags and rc files

Ground truth for this phase, the partition of the command set, and the four
decisions that need the owner before implementation starts.

## The command set, measured

`sqlite3 3.53.3` documents **65** dot commands in `.help`:

    .archive .auth .backup .bail .cd .changes .check .clone .connection .crlf
    .databases .dbconfig .dbinfo .dbtotxt .dump .echo .eqp .excel .exit
    .expert .explain .filectrl .fullschema .headers .help .import .imposter
    .indexes .intck .limit .lint .load .log .mode .nonce .nullvalue .once
    .open .output .parameter .print .progress .prompt .quit .read .recover
    .restore .save .scanstats .schema .session .sha3sum .shell .stats .system
    .tables .testcase .timeout .timer .trace .version .vfsinfo .vfslist
    .vfsname .www

It also **accepts, but does not document**, nine more. Eight were verified by
running them against the binary; a drop-in replacement has to take them too:

| Name | Note |
|---|---|
| `.separator` | still functional, still in `.show` |
| `.width` | still functional, still in `.show` |
| `.show` | prints the settings block |
| `.selftest` | runs the SELFTEST table |
| `.binary` | legacy on/off |
| `.indices` | alias of `.indexes` |
| `.crnl` | alias of `.crlf` |
| `.limits` | alias of `.limit` |
| `.ar` | alias of `.archive` |

The roadmap's "44 portable + 21 refused" was written before this count and is
off by one on each side: the documented set partitions 45/20 under the
roadmap's own rule, and `.separator`/`.width` — which the roadmap lists among
the 44 — are not part of the documented 65 at all. The roadmap's wording is
corrected as part of this phase either way.

## What the public API can actually reach

The exclusion decision is "no vendored extension sources". Applying only that
rule, and not the roadmap's older guess, the refusals shrink to **eleven**:

| Refused | Needs |
|---|---|
| `.archive` / `.ar` | `sqlar.c` + `zipfile.c` |
| `.expert` | `sqlite3expert.c` |
| `.recover` | `dbdata.c` / `sqlite3recover.c` |
| `.intck` | `sqlite3_intck.c` |
| `.session` | `sqlite3session.c` |
| `.sha3sum` | `shathree.c` |
| `.selftest` | depends on `.sha3sum` |
| `.imposter` | `sqlite3_test_control`, test-only |
| `.testcase` | TCL test harness |
| `.check` | TCL test harness |
| `.scanstats` | `SQLITE_ENABLE_STMT_SCANSTATUS` build option |

Everything else the roadmap had marked unsupported is plain public API or
plain file I/O, and is cheap to do properly:

- `.dbinfo` — the 100-byte file header plus pragmas
- `.dbtotxt` — a hex dump of the database file
- `.clone` — a second connection and a copy loop
- `.lint fkey-indexes` — one SQL query
- `.filectrl` — `sqlite3_file_control`, public opcodes
- `.vfsinfo` / `.vfslist` / `.vfsname` — `sqlite3_vfs_find` and friends
- `.www` — writes HTML to a temp file and opens a browser, like `.excel`
- `.nonce` — meaningful once `-safe` exists, which this phase adds

That is **54 supported / 11 refused** instead of 45/20. See decision 1.

## Module shape

`dot.c` as a single file lands around 2,000 lines, which is larger than
anything else in `src/` and mixes four unrelated jobs. Proposed split:

| File | Contents | Est. |
|---|---|---|
| `src/dot.c` | the command table, dispatch, arity and help, refusals, the settings commands | ~900 |
| `src/schema.c` | `.dump` `.schema` `.fullschema` `.indexes` `.databases` `.dbinfo` `.dbtotxt` — everything that renders the schema or the file as text | ~550 |
| `src/import.c` | `.import`, the CSV/ASCII reader, `.excel` and `.www` | ~350 |

Each keeps the existing rule: nothing above `db.c` includes `<sqlite3.h>`, so
`.filectrl`, `.vfs*`, `.backup`, `.clone` and `.dbconfig` get narrow
accessors in `db.h` rather than a leaked handle.

## Shell state

Dot commands need state the REPL does not currently have: the output
redirect (`.output`, `.once`, `.excel`), `echo`, `bail`, `timer`, `stats`,
`changes`, both prompt strings, bound parameters, `-safe`, the init-file
flag, and a connection list for `.connection`. Today `main.c` threads
`FILE *out, FILE *err` through every call, which does not survive `.output`.

Proposed: a `Shell` struct in a new `src/shell.c`, owning the `Db`, the
`Line`, the redirect stack and those flags; `main.c` shrinks to argv parsing
and calls `shell_run`. `dot.c` then takes a `Shell *` and needs no globals.
See decision 3.

## Init files

Order, per the existing decision: `~/.sqliterc` first (parity), then
`$XDG_CONFIG_HOME/sqlsh/sqlshrc` (ours, so it wins on conflict). `-noinit`
skips both. Each is fed line by line through the same dispatch as the REPL,
so an rc file can contain SQL as well as dot commands — that is what upstream
does.

## Verification plan

- Per-command effect tests, not exit codes: one per supported command.
- One test per refusal asserting a non-zero exit and the extension named.
- Differential against the oracle under `--compat`, added to `tests/parity.sh`:
  `.schema`, `.tables`, `.indexes`, `.databases`, `.fullschema`, `.dump`,
  `.show`, `.dbinfo` over the fixture database, byte for byte.
- `.import` round-trips a CSV with embedded commas, quotes, CRLF and newlines.
- `.open` on a bad path leaves the previous connection usable.
- An unknown command suggests the nearest name by edit distance.
- `-noinit` suppresses a `~/.sqliterc` planted in a `HOME` override.

`.dump` byte-parity is the hard one: upstream's dump depends on
`writable_schema`, trigger and view ordering, and a quoting routine of its
own. The plan is byte-parity for databases the fixture can express (tables,
indexes, views, triggers, foreign keys, blobs, NULLs) and a documented gap
for corrupt or virtual-table schemas, which is the same shape as the Phase 5
classic-subset claim.

## Decisions taken

Owner, 2026-09-17. All four as proposed above:

1. **Refusal set**: the wider **54 supported / 11 refused**. Anything the
   public API or plain file I/O can reach is implemented.
2. **Module split**: three files — `dot.c`, `schema.c`, `import.c`.
3. **Shell state**: new `src/shell.c` owning a `Shell` struct; `main.c`
   shrinks to argv parsing.
4. **Line budget**: the success criterion moves to **~10,000 lines** of
   `src/`. The number exists to keep the codebase readable against upstream's
   37,373-line `shell.c`, and 10,000 still makes that point.
