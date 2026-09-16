---
title: "Project name"
id: ""                        # short slug or identifier, e.g. proj-001
#   Status rules:
#   * initiated/defined -> defined requirements exist
#   * in-research -> unknowns actively explored
#   * in-progress -> execution ongoing
#   * waiting condition -> blocked externally
#   * completed/finished -> goals met & verified
status: in-progress           # initiated | defined | in-research | in-progress | waiting | completed
priority: medium              # critical | high | medium | low
start_date: ""                # ISO 8601: YYYY-MM-DD
target_date: ""               # ISO 8601: YYYY-MM-DD
last_updated: ""              # ISO 8601: YYYY-MM-DD
owner: ""                     # primary responsible person
stakeholders: []              # list of names or handles
tags: []                      # free-form labels, e.g. [backend, research, infra]
depends_on: []                # IDs of projects this one depends on
blocks: []                    # IDs of projects blocked by this one
references: []                # URLs, tickets, related docs
notes: ""                     # one-liner freeform note for tooling / dashboards
---

# Project name

## Overview

Brief description of the project’s purpose, context, and scope.

Include:

- **Purpose**: Why it exists and what problem it solves  
- **Context**: Relevant background, constraints, environment  
- **Scope**: What’s included and excluded (high-level boundaries)  
- **References** (optional): Related projects, stakeholders, or requirements  

Guidelines:

- Keep it concise and high-level (no implementation details)  
- Focus on value and rationale, not tasks or milestones

## Goals

Defines intended outcomes and success criteria of the project.

Include:

- **Goals**: Desired outcomes  
- **Success criteria** (optional): Metrics or verification methods  
- **Constraints / priorities** (optional): Context to guide decisions  

Guidelines:

- Focus on outcomes, not tasks  
- Use measurable or verifiable criteria when possible

## Development Guidelines

- **Environment**: Use Nix flakes (`nix develop`) to setup the dependencies. Enter the nix shell once and perform all development inside it to prevent re-accessing every time.
- **Version Control**: Use `git` to track changes. Commit every time a new phase or feature is implemented and verified to work as expected.
- **Workflow**: After each phase implementation is done, wait for explicit user confirmation before marking the verification boxes in the roadmap and committing.
- **Style**: Use suckless coding style and robust coding practices.
- **Warnings**: Always address and fix compiler warnings.
- **Roadmap Expansion**: Once all currently defined phases are complete, expand the roadmap with consequent development phases and verification plans.

## Architecture

High-level structure of the project and its key elements.

Include:

- **Structure**: How the project is organized  
- **Components / assets**: Main internal parts  
- **Resources (optional)**: External dependencies  

Guidelines:

- Keep it high-level (no low-level implementation details)  
- Focus on structure and relationships between components  
- List key components/assets explicitly
- Include only relevant elements

## Roadmap

A roadmap defines a single coherent plan to achieve the project’s goals.
Alternative approaches should be defined as separate roadmaps (branches), with independent phases and tasks.

**Hierarchy**: Phase → Task → Check  

- **Phase**: Stage, feature, or milestone  
- **Task**: Actionable unit  
- **Check**: Validation criteria for completion  

Additional elements

- Design decision: Choice between alternatives (with rationale)  
- Dependency: Relationships between phases or tasks  

**Task status markers**
(keep this table for reference)

| Symbol | Meaning          |
| ------ | ---------------- |
| `[ ]`  | To-do            |
| `[~]`  | In-progress      |
| `[✓]`  | Done / completed |
| `[x]`  | Failed / blocked |
| `[?]`  | Optional / TBD   |
| `[!]`  | Critical         |

### General conditions

Execution context (environment, constraints, principles, version control, coding style).

### Phase 1: Phase name

**Description**

**Tasks**

- [] Task

**Checks**

*Automatic*

- [] Check

*Manual*

- [] Check

**Design decisions** (optional)

- Decision:  
  Rationale:  
  Alternatives (optional):  
  Trade-offs (optional):

**Dependencies** (optional)

**Notes / Risks / Resources** (optional)

- Context, constraints, risks or auxiliar resources to support planning and execution.

