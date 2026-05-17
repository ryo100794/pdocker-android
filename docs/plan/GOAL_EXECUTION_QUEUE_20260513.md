# Goal Execution Queue - 2026-05-13

This queue is the integration spine for parallel work.  It exists to keep
agent output, local edits, device evidence, and release messaging aligned
without mixing unrelated dirty lanes.

## Ground rules

- Do not modify llama.cpp, bundled model files, prompts, or library Dockerfiles
  unless a task explicitly says so.
- Do not claim a feature is complete without either runnable evidence or a
  `planned-gap` artifact that says what remains unproven.
- Keep GPU/runtime dirty-lane commits separate from documentation, test-ledger,
  and UI-contract commits.
- Device-only gates must fail safe: no fake success when ADB, service state, or
  evidence is unavailable.
- Service truth promotion is blocked unless listener/ports/log/UI card/state
  evidence all reduce to the same exact Engine container ID; otherwise the only
  allowed device result is planned-gap/Success: false, never success.
- Archive compatibility, terminal exec-it artifact verification, COW
  kill-at-step, OOM/LMK survival, and image live-pull interruption must stay
  non-promoting when they are host-only, planned-gap, skipped, or missing
  connected-device proof.
- Every adopted agent change needs a focused test command and a file list before
  commit.

## Active lanes

| Lane | Goal | Current owner | Commit unit | Acceptance gate |
|---|---|---:|---|---|
| P0-A service truth | UI cards, Engine API, persisted state, process table, listener, and logs agree on the same Engine container ID. | Goodall output awaiting integration | `service-truth artifact gate` | `bash -n scripts/android-device-smoke.sh`; `python3 scripts/verify-service-truth-plan.py`; service truth contract tests. |
| P0-B runtime teardown | Stop/kill/rm records process-tree cleanup evidence and never trusts HTTP 204 alone. | integrated T1 baseline | already committed `1c9558a` | Device artifact still planned-gap until real no-orphan evidence is captured. |
| P0-C1 terminal transport split | Container terminal sessions use Engine exec/HTTP upgrade only; local PTY/log panes stay distinct session types. | verifier represented; device proof pending | `terminal session-type split` | `python3 -m unittest tests.test_terminal_exec_it_contract`; host checks are regression evidence only. |
| P0-C2 terminal device evidence | UI self-test plus raw Engine JSONL proves Enter, Ctrl-C/ETX, cursor history, `top`/`q`, resize, and IME paths for a real container. | device proof pending | `terminal exec-it device artifact` | `python3 -m unittest tests.test_terminal_exec_it_artifact_verifier`; `scripts/verify-terminal-exec-it-artifact.py ... --require-container`; non-promoting without fresh paired device artifacts. |
| P0-D1 memory pager proof | Managed and transparent APK pager probes prove explicit opt-in, capability/fallback behavior, counters, dirty/writeback data, and unsupported mapping handling. | TODO/test audit sharpened; device replay pending | `memory pager device artifacts` | `bash scripts/android-memory-pager-managed-poc.sh`; `bash scripts/android-memory-pager-transparent-poc.sh`; artifacts remain non-promoting until the full pager plus LMK replay condition passes. |
| P0-D2 OOM/LMK diagnostics | Large allocation guard, system pressure, RSS/PSS/swap/headroom, last progress, backend death/exit status, LMK classifier, UI memory artifact source/age/status, retention, and stale UI guard are recorded without fake success. | static gate integrated; device replay pending | `oom-lmk diagnostic contract` | `python3 scripts/verify-oom-lmk-survival-gate.py`; memory pager/UI contract tests; abnormal/stress JSON validators. Device plan artifacts stay non-promoting until controlled LMK/backend-death replay proves unsafe allocation denial, classifier evidence, and stale UI rejection on hardware. |
| P0-E image pull crash safety | Interrupted pull never publishes a partial image/layer as valid after restart. | synthetic residue runner integrated; live pull pending | `image-pull device scenario ledger` | `python3 scripts/verify-image-pull-crash-safety.py`; image pull crash-safety tests. The live registry interruption lane stays planned-gap/non-promoting until run against a scenario-owned fixture. |
| P0-F1 llama GPU env/readiness | Compare script, pdockerd defaults, UI/compose path, and verifier use the same diagnostic environment and refuse unsafe Android memory headroom before model load. | Bohr audit pending | separate GPU dirty-lane commit only | `tests.test_gpu_abi_contract`; readiness artifact and verifier classification; readiness-blocked artifacts are non-promoting. |
| P0-F2 llama GPU Q6_K device run | NGL=1 Q6_K workgroup/writeback oracle matches before NGL>=2 transformer-layer or benchmark claims. | Bohr audit pending | separate GPU dirty-lane commit only | `scripts/android-llama-gpu-q6k-run.py`; `scripts/verify-llama-gpu-artifact.py`; benchmark claims blocked until `benchmark_claim_allowed=true` and oracle match. |
| P0-G COW/archive kill-at-step | COW copy-up, rename, whiteout, metadata, and archive PUT fail closed across daemon/helper interruption. | device lane represented; promotion pending | `cow-overlay kill-at-step gate` | `python3 -m unittest tests.test_cow_overlay_kill_at_step_device`; `python3 scripts/verify-cow-overlay-bench-recovery.py --run-local`; adb/run-as artifact required for promotion. |
| P1-A archive API compatibility | Docker archive GET/PUT/HEAD and copy semantics stay compatible with lower/upper merge behavior. | host gate integrated | `archive api compat gate` | `python3 scripts/verify-archive-api-compat.py`; host regression only, not stable release credit without storage device gates. |
| P1-B linkat C runtime semantics | Replace copy fallback with C runtime inode/hardlink/CoW semantics, errno parity, write-through, unlink/rename behavior, and kill/restart recovery. | granularity audit split | `linkat hardlink semantics` | Host/static contract remains non-promoting; promotion requires Android artifact proving inode identity, link-count transitions, write-through, errno parity, and recovery. |

## Next integration order

1. Integrate non-GPU contract/test lanes first:
   service truth, terminal C1 transport split, terminal C2 device artifact,
   memory pager D1 proof scaffolds, OOM/LMK D2 diagnostics, image pull
   crash-safety. Keep each sub-lane as a separate commit unit when possible.
2. Run the lightweight combined gate:
   ```bash
   python3 -m unittest \
     tests.test_terminal_exec_it_contract \
     tests.test_memory_pager_contract \
     tests.test_memory_layer_ui_contract \
     tests.test_image_pull_crash_safety_verifier
   python3 -m pytest \
     tests/test_service_truth_ui_contract.py \
     tests/test_service_truth_artifact_contract.py -q
   bash -n scripts/android-device-smoke.sh
   python3 scripts/verify-service-truth-plan.py
   python3 scripts/verify-image-pull-crash-safety.py
   python3 scripts/verify-memory-pager-design.py
   ```
3. Commit only the files for those non-GPU lanes.
4. Rebase with autostash before pushing if the remote advanced.
5. Return to the GPU dirty lane with a clean list of changed native/runtime
   files and device artifacts. Run P0-F1 env/readiness before P0-F2 Q6_K
   device execution; do not start benchmark reporting from a readiness-blocked
   or oracle-mismatch artifact.
6. Keep the linkat C runtime work as P1-B units: errno matrix, metadata/index
   design, C implementation, kill/restart recovery, then Android promotion
   artifact. Host/static checks remain non-promoting.

## Device evidence waiting list

These gates are intentionally not complete until a real Android device artifact
is archived:

- service truth: `files/pdocker/diagnostics/service-truth-latest.json`
  - Same-container-ID proof must include `UICard`, `DockerPs`,
    `EngineApiContainersJson`, `PersistedStateJson`, `ProcessTable`,
    `ListenerProbe`, and `ContainerLogs`.
  - `ListenerProbe` must bind configured/listening ports through
    `listener-probe.json`, `listener-owner-map.json`, and `/proc/net/tcp` to the
    same selected process/container ID.
  - `ContainerLogs.CurrentServiceMarker` plus `logs-selected.out` must belong to
    that same Engine container ID.
  - UI card `TruthState: current` and persisted `state.json` comparison are
    required; stale/unknown UI or state-only card IDs keep the artifact
    planned-gap/Success: false.
- runtime teardown: `files/pdocker/diagnostics/runtime-teardown-latest.json`
- interrupted image pull: `docs/test/image-pull-crash-safety-latest.json`
- image live-pull interruption: planned
  `docs/test/image-pull-crash-safety-live-latest.json`; must use a
  scenario-owned/isolated fixture and remain non-promoting until implemented.
- OOM/LMK diagnostics: planned `pdocker.memory-oom-lmk-diagnostics.v1`
  - Required before promotion: large allocation guard decision with requested
    bytes/headroom/operation ID, last RSS/PSS/swap sample, last progress marker,
    backend pid/exit status or signal, classifier reason, memory ring/summary
    retention paths, and UI artifact source/age/status. Missing ADB, missing
    service evidence, missing ring data, or a planned-gap artifact is
    `success=false` and non-promoting; never synthesize a healthy/running card.
- APK memory pager proof: `docs/test/apk-memory-pager-managed-latest.json` and
  `docs/test/apk-memory-pager-transparent-latest.json`
  - Managed proof unit: explicit opt-in, managed-region table counters, page
    materialization, dirty/writeback accounting, and safe denial/fallback
    fields.
  - Transparent proof unit: capability detection, unsupported mapping exclusion
    or pass-through, unresolved-fault fail-closed diagnostics, and no claim that
    ordinary Dockerfile heap allocations are paged.
  - The pager artifacts do not promote without the OOM/LMK replay artifact and
    UI stale-evidence rejection on a connected device.
- Future mmap/userfault pager: planned non-promoting gate for explicit opt-in,
  kernel capability detection, unsupported mapping pass-through/`ENOMEM`, and
  unresolved-fault fail-closed diagnostics before any large-workload promotion.
- terminal exec-it: `docs/test/ui-it-selftest-latest.json` plus
  `docs/test/engine-exec-input-latest.jsonl`, verified with
  `scripts/verify-terminal-exec-it-artifact.py --require-container`.
  - Split evidence: session type/Engine exec route, single Enter, isolated
    ETX/Ctrl-C with no injected `c`, cursor-key history, stable `top` repaint,
    `q` shell recovery, resize route, and IME regression. Either file missing
    or stale keeps the result non-promoting.
- COW kill-at-step: `docs/test/cow-overlay-kill-at-step-latest.json`; planned
  gap or blocked-device artifacts do not promote.
- llama GPU correctness/performance: latest GPU compare artifact plus API prompt
  correctness sample.
  - Split evidence: env parity, readiness/headroom, Q6_K workgroup/writeback
    diagnostics, oracle match, NGL>=2 repeating-layer proof, verifier
    classification, and only then benchmark fields. Memory blockers,
    readiness-blocked runs, and oracle mismatches are non-promoting.
- linkat hardlink semantics: planned Android direct-syscall artifact
  - Split evidence: C errno matrix, metadata/index or CoW peer tracking,
    creation/unlink/rename/write-through behavior, kill/restart recovery, and
    final device proof of identical `st_dev/st_ino`, link-count transitions,
    Linux errno parity, and no divergent copy fallback promotion.
