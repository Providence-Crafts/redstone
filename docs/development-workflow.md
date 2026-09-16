# Development workflow

A project-agnostic methodology for **specification-driven development executed by an autonomous coding
agent**. It names **roles**, not tools or assistants; each project binds the roles to concrete files
and commands in its own contract / agent-instructions file.

## Principles

1. **Ground truth is on disk, never in conversation.** The **project file** is authoritative; its
   roadmap checkboxes are the state of record. A fresh context (after compaction, a new session, or a
   sub-agent spawn) resumes from *nothing but* "execute Phase N" + the project file.
2. **Planning is supervised; implementation is autonomous.** Decisions are resolved with the owner at
   planning time and written into the roadmap; execution then runs one phase per fresh context with no
   human in the loop — except at the manual-check gate below.
3. **Undefined-at-implementation → flag, never guess.** Never invent a design decision.
4. **Nothing is tracked outside the roadmap.** Work discovered mid-phase is recorded there (as a task,
   a deferred-work entry, or a new phase), never carried in conversation or in the agent's head.

## The project file

Single source of truth, structured as: overview, goals, development guidelines, architecture, and a
**roadmap** of `Phase → Task → Check`, plus design decisions, dependencies and a deferred-work log.
Status markers follow the project file's own legend — a done marker and a failed/blocked marker are
distinct, and an unfinished phase is never marked done to "keep moving".

Every phase records its **checks** split into two kinds, decided at planning time:

- **Automatic checks** — a runnable command with a PASS/FAIL result.
- **Manual checks** — anything the agent cannot honestly verify itself: visual and interactive
  behaviour, subjective output quality, hardware-dependent results, anything needing a human's eyes.

The split is itself a planning decision, written into the phase. Maximise the automatic side: a check
is only manual when automating it is genuinely impractical, not when it is merely inconvenient.

## Planning (supervised)

A phase is ready only when it is **decision-closed** (no open choice or unspecified quantity),
**machine-gated** (its automatic checks are a runnable acceptance test, not prose), and **robust** (a
check would fail a plausible wrong implementation — a positive oracle plus a negative test;
lint/type/reproducible output are necessary, never sufficient).

The owner reviews and approves the phase plan before implementation begins. Approval of a plan is not
approval of the next one.

## Implementation (autonomous)

1. Re-verify the phase's named commands and APIs exist (halt on drift).
2. Read the phase spec and its references.
3. Decision check → halt or flag-and-postpone if anything is undefined.
4. Implement the tasks **and** the committed acceptance test.
5. Run the **gate** (below).
6. On green: update the roadmap and hand off per the commit rules.
7. Report phase, files touched, gate result, and either the commit or the blocker.

**Flag unexpected issues immediately.** Anything materially affecting scope, architecture, correctness
or the plan's validity is surfaced to the owner when found — not deferred to the end-of-phase report,
and not silently worked around.

## The gate

One command, one verdict: PASS / FAIL / BLOCKED. The gate — not the agent's assertion of "done" — is
the arbiter.

Green requires **all** of:

- formatter clean,
- linter clean,
- type check clean,
- **zero errors and zero warnings** across all of the above and the build; warnings are defects, not
  noise. A warning is fixed at its cause or explicitly waived in the project file with a rationale —
  never suppressed silently or globally,
- at least one acceptance test **passed** — not merely collected or skipped. **Skip is not pass.**

Phases needing an external toolchain are certified only where that toolchain runs; absent, they are
*blocked: needs-gated-env*. Release closeouts run the mechanical steps then **halt** for owner-only
actions. Research and judgment phases are **owner-track** and excluded from the unattended queue.

## Manual-check gate

**If a phase carries any manual check, the agent does not commit.** It completes the implementation,
runs the automatic gate to green, updates the roadmap to in-progress, then **halts** and reports what
the owner needs to verify and how to reproduce it.

The owner performs the manual checks and confirms. Only then are the phase's check boxes marked and
the commit made. This is a hard stop: a green automatic gate is not authority to commit a phase whose
manual checks are outstanding.

Phases with no manual checks proceed to commit autonomously.

## Halt vs. flag-and-postpone vs. proceed

The oracle defines a phase's essential scope. Undefined work that **blocks the oracle → halt** (record
a blocker note, leave the phase open). **Peripheral** undefined work → **flag-and-postpone**: record it
in the deferred-work log, then let the phase go green. Nothing is silently dropped. Fully defined →
proceed.

## Sub-agent workflow

The executor is a **phase-runner** that runs exactly one phase and stops. Spawning one per phase keeps
the orchestrator's context short.

- **In-session:** spawn one runner per phase, each with an isolated context. Never trust its "done" —
  the orchestrator re-runs the gate before accepting the phase.
- **Headless (unattended):** each phase runs in a fresh process, so no session grows and no compaction
  is needed; a queue driver walks the backlog and stops at the first phase that is non-green, blocked,
  or carries manual checks. Requires explicit opt-in, since it bypasses permission prompts.

## Environment and reproducibility

- **Nix flakes** define the environment. `nix develop` provides the full toolchain; enter the shell
  once and work inside it rather than re-entering per command. The flake is the single description of
  system-level dependencies.
- **`uv`** manages Python dependencies and virtual environments, with a committed lockfile. Pinned,
  reproducible resolution — never ad-hoc installs into an ambient interpreter.
- Versions are pinned, not floating. A build that cannot be reproduced on another machine from a clean
  checkout is a defect.

## Code standards

Production quality, not prototype quality. Concretely:

- **Modular** — clear boundaries, one responsibility per module, dependencies pointing one way.
  Anything reachable only through a long chain of side effects is a design error.
- **Functional** — prefer pure functions and explicit data flow over hidden state; isolate I/O and
  mutation at the edges. Deterministic given inputs, and therefore testable.
- **Suckless** — the simplest construction that satisfies the requirement. No speculative abstraction,
  no configurability without a caller, no framework where a function suffices. Deleting code is
  progress.
- **Errors handled explicitly** — no bare catches, no silently swallowed failures, no unchecked
  partial results. Failure modes are as designed as success paths.
- **Linters and formatters are mandatory** and run in the gate, not by habit. Their configuration is
  committed and identical for every contributor and every agent.

## Logging and assertions

A **single standardised logging facility** shared by the whole project: one configuration point,
structured records, consistent levels, no stray prints. Log at boundaries and decisions, not at every
line. Logs are an operational interface, so their format is part of the design.

A **single standardised assertion facility** for invariants: preconditions, postconditions and
internal-consistency checks, raising a project-specific error type with the violated invariant and its
context in the message. Assertions state what must be true, are never used for expected runtime
conditions such as user input validation, and are never compiled away in a configuration that anyone
runs for real.

## Version control

- `git` tracks all changes.
- **One commit per phase**, made only when that phase's gate is green and its manual checks (if any)
  are confirmed by the owner.
- **Standardised commit messages** per the project's committed convention: a consistent prefix
  identifying the phase, a concise imperative summary, and a body covering what changed and why when
  the summary is insufficient. Authorship trailers follow the project convention.
- Stage explicit paths only; never stage-all.
- Never push and never cut a release tag autonomously.
