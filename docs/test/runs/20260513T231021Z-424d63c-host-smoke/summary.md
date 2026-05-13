# pdocker Test Run 20260513T231021Z-424d63c-host-smoke

- Status: `pass`
- Git: `424d63c517942b709cae82813bf0f8378312eb05`
- Branch: `main`
- Lanes: `host-smoke`
- Commands: `16`
- Artifacts: `13`

## Commands

| Lane | Command | Status | Seconds | Log |
|---|---|---:|---:|---|
| host-smoke | `python3 -m unittest discover -s tests -p 'test_*.py'` | pass | 44.788 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/001-host-smoke-unittest-all.log` |
| host-smoke | `python3 docker-proot-setup/scripts/verify_runtime_contract.py` | pass | 8.469 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/002-host-smoke-verify-runtime-contract.log` |
| host-smoke | `python3 scripts/verify_direct_syscall_contracts.py` | pass | 1.566 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/003-host-smoke-verify-direct-syscall.log` |
| host-smoke | `python3 scripts/verify-build-profile.py` | pass | 6.899 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/004-host-smoke-verify-build-profile.log` |
| host-smoke | `python3 scripts/verify-service-truth-plan.py` | pass | 1.621 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/005-host-smoke-verify-service-truth-plan.log` |
| host-smoke | `python3 scripts/verify-image-pull-crash-safety.py` | pass | 5.208 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/006-host-smoke-verify-image-pull-crash-safety.log` |
| host-smoke | `python3 scripts/verify-project-library.py` | pass | 1.565 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/007-host-smoke-verify-project-library.log` |
| host-smoke | `python3 scripts/verify-input-validation.py --write-artifact docs/test/input-validation-latest.json` | pass | 6.242 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/008-host-smoke-verify-input-validation.log` |
| host-smoke | `python3 scripts/verify-abnormal-events.py --write-artifact docs/test/abnormal-events-latest.json` | pass | 3.961 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/009-host-smoke-verify-abnormal-events.log` |
| host-smoke | `python3 scripts/verify-refactor-resilience.py --write-artifact docs/test/refactor-resilience-latest.json` | pass | 2.404 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/010-host-smoke-verify-refactor-resilience.log` |
| host-smoke | `python3 scripts/verify-input-grammar-coverage.py` | pass | 1.573 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/011-host-smoke-verify-input-grammar-coverage.log` |
| host-smoke | `python3 scripts/verify-stress-regression.py --write-artifact docs/test/stress-regression-latest.json` | pass | 9.956 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/012-host-smoke-verify-stress-regression.log` |
| host-smoke | `python3 scripts/verify-blackbox-requirements.py` | pass | 2.482 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/013-host-smoke-verify-blackbox-requirements.log` |
| host-smoke | `python3 scripts/verify-feature-scenarios.py` | pass | 1.524 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/014-host-smoke-verify-feature-scenarios.log` |
| host-smoke | `python3 scripts/verify-ui-actions.py` | pass | 1.783 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/015-host-smoke-verify-ui-actions.log` |
| host-smoke | `python3 scripts/verify_terminal_editor_contracts.py` | pass | 1.647 | `docs/test/runs/20260513T231021Z-424d63c-host-smoke/016-host-smoke-verify-terminal-editor.log` |

## Artifacts

- `tests/direct_syscall_coverage.json` (34273 bytes, sha256 `5db1aeca67ef2624fde3bd148279b37686a4a5f01c1ce05a0082771fe1d3ca0b`)
- `docs/test/input-validation-latest.json` (3506 bytes, sha256 `10692a52a6d11ec858b5b8ce4e2c77c0ac8cd80d943cea5d330bc0a211fdbfaf`)
- `tests/input_validation_cases.json` (3414 bytes, sha256 `6f1235fc8151c7c2973a2808bd0369396558f036254ee3815f2614e658df1d2b`)
- `tests/input_grammar_coverage.json` (7025 bytes, sha256 `1803a498a5102f19a4dae8a29658eb4b3f1e5daa2a651311713b10dbab23420c`)
- `docs/test/abnormal-events-latest.json` (20702 bytes, sha256 `48c26e281eeda107f7caf7acdf79e4d6e1e8eafa3006d4ac718cd9c1c2d596b3`)
- `tests/abnormal_event_cases.json` (13824 bytes, sha256 `f2b42246c013cf1ceaa0094f315192c8b28b19a8924e7eaa4a3cc8381572707b`)
- `docs/test/refactor-resilience-latest.json` (5959 bytes, sha256 `8b47b4f2f5d45fd0d8b43d23e29a4f8e9467ebb43a8690a7306573c1a0a138ae`)
- `tests/refactor_resilience_cases.json` (6244 bytes, sha256 `be28349a5c554bc862b07abf5c9577f6cf7b4e894d4bd6887ed65b2ebfee2d52`)
- `tests/input_grammar_coverage.json` (7025 bytes, sha256 `1803a498a5102f19a4dae8a29658eb4b3f1e5daa2a651311713b10dbab23420c`)
- `docs/test/stress-regression-latest.json` (1033 bytes, sha256 `8e84e02291af783bfd26261e1d2543ffd7d64129becf6484b68ea5f0c20aee54`)
- `tests/stress_regression_cases.json` (3997 bytes, sha256 `1691416b873a0bb2ea049da079ce9fb4790116b679bfa25440b6d2fbc5335b69`)
- `tests/blackbox_requirements.json` (11336 bytes, sha256 `d4b9af4dfd1fedc8aea81bc7b06de52bfec4b0d945db95e04055fa4612f4e649`)
- `tests/feature_scenarios.json` (34727 bytes, sha256 `742c81287d4cb6a4d69631ce4466172fecd0127487ecfce98012bf638f274e52`)
