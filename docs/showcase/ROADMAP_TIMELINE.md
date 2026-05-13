# Roadmap Timeline

<!-- SYNCHRONIZED with docs/plan/TODO.md; regenerate when the TODO ledger changes. -->

Source snapshot: `2026-05-13`.
Repository reference: source-controlled generated snapshot.

This timeline is derived from the live TODO ledger and is meant for GitHub
issues, release planning, and Wiki readers. It is not a promise of fixed
release dates; it is a public operating plan that updates as implementation
evidence changes.

## Velocity Assumption

Recent development has closed many UI, direct-runtime, storage, template, and
test-ledger tasks in a short window. The public plan deliberately spends only
part of that apparent velocity because Android device builds, GPU bridges,
package-manager behavior, and long Compose templates have high variance.

Current TODO counters:

| State | Count |
|---|---:|
| done | 52 |
| doing | 14 |
| next | 39 |
| blocked | 0 |

## Current Priority Gates

The public roadmap follows the same order as the internal execution ledger:

1. Service truth same-container-ID.
2. Runtime teardown and stale-process cleanup.
3. llama GPU Q6_K correctness with synchronized environment propagation.
4. Image-pull crash safety.
5. COW/overlay mutation safety.
6. Terminal `exec -it` hard gate.
7. VS Code workspace health gate.
8. SAF direct output for `/documents`.

## Timeline

### Now: 2026-05-13 to 2026-05-16

Keep the UI compose path truthful and visible while closing regressions that block demos.

- Close the service-truth same-container-ID gate so UI cards, `docker ps`,
  Engine state, process table, listeners, and logs agree before showing
  running or healthy.
- Add runtime teardown proof for stop/kill so stale direct children, GPU
  executor helpers, listeners, and duplicate-name residue cannot survive as
  hidden state.
- Continue the llama GPU Q6_K correctness lane without modifying llama.cpp,
  Dockerfiles, models, or prompts, and lock environment propagation across
  compare scripts, pdockerd defaults, and UI/compose launches.
- Turn image-pull crash safety from static recovery checks into an interrupted
  pull kill/restart artifact.

### Next: 2026-05-17 to 2026-05-23

Turn the remaining high-value TODOs into repeatable device checks and public artifacts.

- Add COW/overlay mutation safety tests for copy-up, whiteout, rename, archive
  PUT, hardlink metadata, low-space, kill-at-step behavior, and startup repair.
- Promote terminal `exec -it` from static checks to a real UI-driven device
  gate covering Enter, Ctrl-C, cursor keys, `top`, `q`, resize, and IME
  behavior.
- Add the VS Code workspace health gate: compose/build/run, code-server
  listener on `18080`, extension evidence, and UI/Engine same-ID proof.
- Add SAF direct output evidence for `/documents`, including sidecar UnixFS
  metadata and explicit fallback reporting.

### Then: 2026-05-24 to 2026-06-03

Use benchmark evidence to harden compatibility and publish tester-ready releases.

- Report llama GPU speed only after correctness and launch-environment parity
  are proven.
- Expand Docker compatibility gates for Compose grammar, build context tar,
  archive API, port mapping, and storage metrics.
- Promote verified templates, device reports, known limits, and release gates
  into release notes, GitHub issues, and Wiki mirrors.

## Posting Rule

Use this page as the source for public roadmap posts:

- Pin the current `Now` and `Next` windows in a GitHub issue comment.
- Link back to `docs/plan/TODO.md` for the full ledger.
- When a milestone moves because of measured device behavior, update TODO
  first and regenerate this page.
- Keep optimistic claims out of the post until a repeatable test artifact
  exists under `docs/test/`.
