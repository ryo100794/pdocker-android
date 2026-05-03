# Test Documents

Snapshot date: 2026-05-03.

## Purpose

This category contains repeatable test procedures, compatibility audits, debug
workflows, and recorded test outputs. It should answer what is checked, how to
run it, and where the latest result is stored.

## Contents

| Document | Scope |
|---|---|
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | Docker API, data exchange, protocol, APK payload, and UI compatibility coverage |
| [`compat-audit-latest.md`](compat-audit-latest.md) | Latest recorded compatibility audit result |
| [`ANDROID_SELFDEBUG.md`](ANDROID_SELFDEBUG.md) | Android Wi-Fi ADB and self-debug workflow |
| [`SECRET_AUDIT.md`](SECRET_AUDIT.md) | Repeatable secret, signing material, and remote URL audit before publication |

## Maintenance

- Keep command examples reproducible from the repository root.
- Keep generated or recorded results in this category.
- Move product boundary decisions to [`../design/README.md`](../design/README.md).
- Move active implementation tasks to [`../plan/TODO.md`](../plan/TODO.md).
