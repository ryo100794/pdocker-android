# Documentation map

Snapshot date: 2026-05-02.

Use this file to avoid duplicating the same status or plan in multiple docs.

| Topic | Canonical doc | Notes |
|---|---|---|
| Active TODOs and temporary workarounds | [`TODO.md`](TODO.md) | Update this whenever a workaround is added or removed. |
| Docker compatibility, protocol coverage, gaps | [`COMPATIBILITY.md`](COMPATIBILITY.md) | Generated latest audit result: [`compat-audit-latest.md`](compat-audit-latest.md). |
| High-level implementation shape | [`STATUS.md`](STATUS.md) | Keep as a system summary; do not duplicate full API/gap tables here. |
| Runtime replacement and PRoot retirement | [`RUNTIME_STRATEGY.md`](RUNTIME_STRATEGY.md) | Execution-backend strategy and acceptance criteria. |
| Historical steering snapshot | [`REPLAN_2026-05-01.md`](REPLAN_2026-05-01.md) | Historical matrix only; live plan is `TODO.md`. |
| Android GPU benchmark and cuVK direction | [`GPU_COMPAT.md`](GPU_COMPAT.md) | Backend request/env contract lives in `docker-proot-setup/docs/GPU_COMPAT.md`. |
| Backend GPU request/env contract | [`../docker-proot-setup/docs/GPU_COMPAT.md`](../docker-proot-setup/docs/GPU_COMPAT.md) | Keep backend-specific behavior here. |
| Backend network and port rewrite plan | [`../docker-proot-setup/docs/NETWORK_COMPAT.md`](../docker-proot-setup/docs/NETWORK_COMPAT.md) | Backend-specific metadata and future rewrite plan. |
| Default dev workspace | [`DEFAULT_DEV_WORKSPACE.md`](DEFAULT_DEV_WORKSPACE.md) | Template contents and expected user flow. |
| Android self-debug workflow | [`ANDROID_SELFDEBUG.md`](ANDROID_SELFDEBUG.md) | ADB/debugging procedure. |
| Third-party licenses | [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) | Distribution/license inventory. |

When adding a new doc, prefer linking to the canonical source above instead of
copying large status tables or TODO lists.
