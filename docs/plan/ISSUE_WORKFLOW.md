# GitHub Issue Workflow

Snapshot date: 2026-05-04.

This repository should use GitHub Issues as the primary work tracker. Planning
docs are still useful, but they should not become the only place where active
rules, blockers, and delegated work live.

## Source Of Truth

- GitHub Issues own actionable work, owner handoff, discussion, and closure.
- `docs/plan/TODO.md` is the compact project-board summary and release-facing
  index. It should link or mirror only the issues that affect the near-term
  plan.
- `docs/plan/AGENT_COORDINATION.md` is short-lived execution memory for active
  or recently recovered agent work.
- Design docs own architecture decisions and trade-offs.
- Test docs own repeatable evidence, commands, and result artifacts.
- `docs/showcase/ROADMAP_TIMELINE.md` is generated from TODO and should be
  treated as a public projection, not the private task database.

## Issue Types

Use a small number of issue shapes:

| Type | Use for |
|---|---|
| Roadmap epic | A broad public theme, such as Compose parity or GPU bridge work |
| Implementation task | One shippable code/doc/test change with acceptance checks |
| Compatibility gap | Behavior that differs from upstream Docker |
| Device report | Real Android device results and logs |
| Release readiness | F-Droid, signing, license, reproducibility, and packaging gates |

## Promotion Rule

When a conversation, TODO entry, or agent result creates work that can be
assigned or closed independently, promote it to an issue if any of these are
true:

- It touches more than one file or subsystem.
- It needs device validation, benchmark evidence, or repeated follow-up.
- It is blocked on design, Android behavior, or external policy.
- It came from an agent investigation and should survive context compaction.
- It is important enough to appear on the public roadmap.

Keep tiny same-turn fixes in the commit only. Do not create an issue for every
line edit.

## Required Issue Body

Every implementation task should include:

- Problem statement.
- Scope and non-goals.
- File/module ownership.
- Acceptance checks.
- Relevant docs or artifacts.
- Current blocker, if any.

If the task came from an agent, include the recovered conclusion and the agent
name in the issue body or the first comment.

## TODO Synchronization

Use this sync direction:

1. Create or update the GitHub issue.
2. Put only the short issue-linked summary in `docs/plan/TODO.md`.
3. Regenerate showcase docs with `python3 scripts/update-showcase.py`.
4. Close the issue only after the acceptance check passes and the evidence is
   committed or linked.

The TODO ledger should not carry long implementation transcripts after an issue
exists. Keep the details in the issue and use TODO as the dashboard.

## Agent Synchronization

For delegated work:

1. Assign the agent a narrow file scope.
2. Record active delegation in `AGENT_COORDINATION.md` only while it matters.
3. When the agent returns, move conclusions into code, docs, tests, or an
   issue.
4. Delete or compress stale coordination entries before commit.

## Timeline Synchronization

The timeline comes from TODO, not directly from Issues. To make an issue appear
on the public timeline, add a short `[doing]`, `[next]`, or `[blocked]` TODO
entry that references the issue number and then regenerate the showcase docs.

Keep the timeline conservative. A GPU or runtime feature should not be called
done until the issue acceptance checks include a repeatable artifact under
`docs/test/` or a device smoke result.
