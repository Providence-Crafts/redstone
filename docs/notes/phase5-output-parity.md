# Phase 5 scope discovery: output formatting parity

Written while starting Phase 5. Phase 5's task list in `PROJECT.md` was drafted
against the *classic* sqlite3 shell (13 hand-written output modes). The version
we target, 3.53.3, no longer works that way.

## What upstream actually does now

`sqlite3` 3.53 delegates all result rendering to a bundled library,
`ext/qrf/qrf.c` ("query result format"), which is compiled into `shell.c` and
is **not** exported by `libsqlite3`. In `reference/shell.c` it occupies lines
675-3910 — about 3 200 lines.

* **23 modes**, not 13: `ascii box c column count csv html insert jatom jobject
  json line list markdown off psql qbox quote split table tabs tcl www`, plus
  the `batch` and `tty` pseudo-modes and up to 25 user-defined saved modes
  (`.mode --tag NAME`).
* Modes are not separate printers. Each is a **preset** over one orthogonal
  spec (`sqlite3_qrf_spec`): a style, a text-quoting encoding, a title
  encoding, a blob encoding, a control-character escape, column/row
  separators, a NULL rendering, borders, per-column widths and alignments,
  word wrap, screen width, and four limits (`--charlimit`, `--linelimit`,
  `--titlelimit`, `--multiinsert`).
* The spec also carries `xWrite` and `xRender` callbacks, so the renderer is
  already decoupled from stdio.

Byte-identical output therefore means reproducing that library's behaviour,
not thirteen printers. The `.mode` option flags listed in the roadmap are the
visible surface of the same thing.

## The choice

### A. Vendor `qrf.c`

Extract `ext/qrf/qrf.h` and `ext/qrf/qrf.c` from the amalgamation we already
pin, build them as part of `sqlsh`, and drive them from `out.c`.

* Parity is exact by construction, including every mode, flag and edge case,
  and it tracks upstream when we bump the pinned version.
* `out.c` shrinks to a spec builder plus our own colour renderer for the
  default mode.
* Costs ~3 200 lines of third-party code in the tree. It is public domain and
  it is the same project we already link against, but it is not code we can
  claim to have read line by line, and it is far more machinery than the rest
  of `sqlsh` put together.
* Colour is the awkward part: `qrf` measures display width itself, so SGR
  sequences injected through `xRender` would break its alignment. The pretty
  default would stay our own renderer, leaving two formatters in the binary.

### B. Our own formatter, classic subset exact

Implement the orthogonal spec ourselves, but only claim byte parity for the
modes that carry real traffic — `list csv tabs quote insert line json html
ascii column box table markdown` — and for the flags those modes use. The
remaining modes (`c tcl jatom jobject count off psql qbox split www`) are
accepted and rendered on a best-effort basis, with any divergence recorded.

* Roughly 1 000-1 300 lines we own and can read.
* The differential suite is the honest arbiter: it runs the whole matrix and
  we publish exactly which cells are green.
* `sqlsh --compat` is byte-exact where it matters and approximate at the
  edges. A script relying on `.mode tcl` would notice.

### C. Our own formatter, full parity

Same as B, extended until every mode and flag is byte-exact. Realistically two
to three phases of work, most of it in corners nobody exercises
(`--blob-quote tcl`, `--escape symbol`, split-column wrapping).

## Recommendation

**B.** The project's stated value is a shell that is nicer to use, with parity
as the safety net; spending three phases on `--blob-quote tcl` inverts that.
The differential suite makes the gaps visible rather than silent, and any gap
can be closed later by promoting a cell from red to green — or by falling back
to A for one mode at a time if a gap ever matters.
