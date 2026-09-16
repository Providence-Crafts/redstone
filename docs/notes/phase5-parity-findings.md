# Phase 5: ground truth gathered from sqlite3 3.53.3

Facts established by reading `reference/shell.c` (the bundled QRF formatter) and
by running the `sqlite3` binary in the dev shell. Where the two disagree, the
**binary wins** — `reference/shell.c` is a trunk snapshot and is not necessarily
the code the linked 3.53.3 was built from.

## The preset table

`aModeInfo[]` at `reference/shell.c:23903` is the authority for the 23 modes
(22 plus `www`, an alias of `html` that `.mode` refuses to select by name).
Separators and NULL texts are indices into `aModeStr[]`:

    0 -      1 "\n"    2 "|"     3 " "    4 ","    5 "\r\n"
    6 "\036" 7 "\037"  8 "\t"    9 ""     10 "NULL" 11 "null" 12 "\"\"" 13 ": "

A zero index means "leave the current value alone".

`modeChange()` (`:24981`) applies a preset: separators, NULL text, text/title/
blob encodings, header default, border (`mFlg&1` forces border off, as in
`psql`), split-column (`mFlg&2`, as in `split`, which also turns on automatic
screen width).

Two pseudo-modes matter for our defaults:

- `MODE_BATCH` = `list`. That is sqlite3(1) non-interactive, i.e. `--compat`.
- `MODE_TTY` = `qbox` + relaxed text + charlimit 300 + linelimit 5 +
  titlelimit 20 + multiinsert 3000 + textjsonb + automatic screen width. That
  is what upstream shows an interactive user, and it is very close to what
  sqlsh calls "pretty".

## Encoders (`qrfEncodeText`, `qrfRenderValue`)

- **CSV** quotes when any byte is in `qrfCsvQuote[]` *or* when the field
  contains the column separator; the quoted form doubles `"`. Empirically the
  quote set includes non-ASCII bytes: `SELECT '日本'` yields `"日本"`, while
  `'plain'` is bare.
- **Control-character escaping runs after every encoder** (`eEsc != off`
  applies `qrfEscape` over the bytes just appended), so csv, html, json and tcl
  all show `^A` too, not only the plain modes. Escaping is `^`+0x40 for ascii
  and U+2400+c for symbol; tab, newline and the CR of a CRLF survive.
- **HTML** escapes `< > & " '` as `&lt; &gt; &amp; &quot; &#39;`.
- **JSON/TCL** share one encoder: `"`-delimited, `\" \\ \b \f \n \r \t`, and
  otherwise `\u%04x` (json) or `\%03o` (tcl).
- **SQL** uses `%Q`/`%#Q`; `%#Q` is what produces `unistr('...')`.
- **NULL** renders as `spec.zNull`. Note the observed exception: in `html`
  mode 3.53.3 prints `null` regardless of `-nullvalue`, so the html preset's
  NULL text is `"null"` for our purposes.
- **`--blob-quote size`** renders `(N-byte blob)`, not `blob(N)`.
- **`--charlimit`** truncates on display width and appends `...`.

## `.mode` option semantics that differ from a first guess

- `--align STRING` is a string of `L`/`C`/`R` characters, one per column
  (unspecified columns default to `L`) — not a comma-separated word list.
- `--limits L,C,T` is shorthand for linelimit, charlimit, titlelimit; `,T` may
  be omitted; `off` means `0,0,0` and `on` means `5,300,20`.
- `--quote ARG`: `off on sql relaxed csv html tcl json`, where `on` is an alias
  for `sql` and `off` means as-is.
- `--title ARG`: `off on sql csv html tcl json` — it sets both whether titles
  show and how they are encoded.
- `--widths LIST` pads with `0` (dynamic) for unlisted columns.
- `.mode insert TABLE` is legacy-supported: a non-option argument after
  `insert` is the table name.
- In insert mode with titles on, the statement carries the column list:
  `INSERT INTO tab(a,b) VALUES(...)`.
- Also accepted: `--once`, `--tag NAME`, `--textjsonb BOOLEAN`, `-v`.

## Bugs this note already paid for

- `width_char` must use signed indices; the unsigned binary search wraps at
  `last == 0` and spins forever on the first combining mark.
- Streaming styles count rows without allocating cells, so `free_result` must
  tolerate a NULL cell array.

## Trunk vs. the shipped binary: `www`

`reference/shell.c` carries a `www` preset (`{ "www", 0, 0, 9, 4, 4, 0, 2, 7, 0, 0 }`)
and a dispatch arm for it. The linked 3.53.3 binary rejects it:

```
$ printf '.mode www\nselect 1;\n' | sqlite3
line 1:       ^--- unknown mode
line 1: Use ".help .mode" for more info
```

So `g_preset[]` has 22 rows, not 23. The rule stands: where the snapshot and
the binary disagree, the binary is the oracle, because it is what a user's
`sqlite3` actually does. The second instance of the same disagreement is `html`
NULL text, which the trunk table says is `eNull=9` but the binary prints as
`null` regardless of `-nullvalue`.

## Command-line flags are a subset of `.mode` names

`sqlite3(1)` accepts only `-ascii -box -column -csv -html -json -line -list
-markdown -quote -table -tabs` as command-line mode flags. `-insert`, `-tcl`,
`-www` and `-qbox` are "unknown option" on the command line even though `.mode`
accepts the corresponding names. `src/main.c`'s `is_cmdline_mode()` enforces
this; without it `sqlsh -insert` printed rows where `sqlite3 -insert` prints
nothing and errors.
