#!/usr/bin/env python3
"""Planned device scenario ledger for interrupted image-pull crash safety.

This runner is deliberately evidence-first.  Without an attached Android device
(or until the real kill/restart automation is wired), it writes a `planned-gap`
artifact that contains the exact command plan, required artifact schema,
negative expected conditions, and cleanup policy.  It never reports success for
missing device evidence.
"""

from __future__ import annotations

import argparse
import json
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_ARTIFACT = ROOT / "docs" / "test" / "image-pull-crash-safety-latest.json"
SCENARIO_ID = "image.pull.interrupted-kill-restart"
PLAN_GATE = "python3 scripts/verify-image-pull-crash-safety.py"

ARTIFACT_SCHEMA: dict[str, Any] = {
    "schema_version": 1,
    "scenario_id": SCENARIO_ID,
    "status": "planned-gap|blocked|failed|passed",
    "success": False,
    "generated_at": "RFC3339 UTC timestamp",
    "device": {
        "adb_present": "boolean",
        "serial": "string|null",
        "state": "string|null",
        "fingerprint": "string|null",
    },
    "inputs": {"image": "registry reference", "package": "Android package id"},
    "commands": ["tokenizable host commands used or planned"],
    "evidence": {
        "pull_log": "path|null",
        "daemon_log_before_kill": "path|null",
        "daemon_log_after_restart": "path|null",
        "store_listing_after_restart": "path|null",
        "image_inspect_after_restart": "path|null",
        "container_run_after_restart": "path|null",
    },
    "negative_expected_conditions": ["strings that must not appear in evidence"],
    "cleanup_policy": ["cleanup steps safe to run after pass/fail/interrupt"],
    "notes": ["operator-readable notes"],
}

NEGATIVE_EXPECTED_CONDITIONS = [
    "partial .pull-* image stage is accepted as a tag after restart",
    "partial .tmp-* layer directory is accepted as a complete layer",
    "missing or corrupt layer meta.json is treated as reusable cache",
    "old tag backup is lost when replacement pull is killed before publish",
    "docker image inspect succeeds for a never-published interrupted tag",
    "docker run succeeds from a tag whose pull was killed before atomic publish",
    "cleanup deletes the previously published tag when only the replacement stage was interrupted",
]

CLEANUP_POLICY = [
    "Always collect daemon log, image store listing, and layer store listing before cleanup.",
    "Run the backend prune/startup recovery path after restart; do not manually delete evidence first.",
    "Remove only scenario-owned test tags/containers and generated artifacts after evidence capture.",
    "Leave unrelated images, layers, containers, app data, and other workers' files untouched.",
    "If cleanup itself fails, keep success=false and record the remaining paths in notes.",
]


def host_command(adb: str, serial: str | None, *args: str) -> str:
    base = [adb]
    if serial:
        base += ["-s", serial]
    base += list(args)
    return shlex.join(base)


def scenario_commands(adb: str, serial: str | None, package: str, image: str, artifact: Path) -> list[str]:
    device_runner = "/data/local/tmp/pdocker-image-pull-crash-safety.sh"
    return [
        shlex.join(["python3", "scripts/verify-image-pull-crash-safety.py"]),
        host_command(adb, serial, "get-state"),
        host_command(adb, serial, "shell", "getprop", "ro.build.fingerprint"),
        host_command(adb, serial, "shell", "am", "force-stop", package),
        host_command(adb, serial, "push", "scripts/verify/runner/image-pull-crash-safety-device.sh", device_runner),
        host_command(adb, serial, "shell", "chmod", "755", device_runner),
        host_command(adb, serial, "shell", device_runner, "--package", package, "--image", image, "--phase", "start-pull"),
        host_command(adb, serial, "shell", "pkill", "-TERM", "-f", "pdockerd"),
        host_command(adb, serial, "shell", device_runner, "--package", package, "--image", image, "--phase", "restart-and-probe"),
        host_command(adb, serial, "pull", "/sdcard/pdocker/image-pull-crash-safety", str(artifact.parent)),
    ]


def detect_device(adb: str, serial: str | None) -> tuple[dict[str, Any], list[str]]:
    notes: list[str] = []
    present = shutil.which(adb) is not None
    device: dict[str, Any] = {"adb_present": present, "serial": serial, "state": None, "fingerprint": None}
    if not present:
        notes.append(f"ADB executable {adb!r} was not found; device evidence remains planned-gap.")
        return device, notes

    cmd = [adb]
    if serial:
        cmd += ["-s", serial]
    cmd += ["get-state"]
    try:
        state = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    except Exception as exc:  # pragma: no cover - depends on host adb/device state
        notes.append(f"ADB get-state failed: {exc}")
        return device, notes
    device["state"] = state.stdout.strip() or None
    if state.returncode != 0 or device["state"] != "device":
        notes.append("No ready Android device was available; not executing interrupted-pull scenario.")
        if state.stderr.strip():
            notes.append(state.stderr.strip())
        return device, notes

    fcmd = [adb]
    if serial:
        fcmd += ["-s", serial]
    fcmd += ["shell", "getprop", "ro.build.fingerprint"]
    fp = subprocess.run(fcmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    device["fingerprint"] = fp.stdout.strip() or None
    notes.append("Device detected, but automated kill/restart evidence collection is not implemented yet; keeping planned-gap.")
    return device, notes


def build_artifact(args: argparse.Namespace) -> dict[str, Any]:
    artifact_path = Path(args.artifact).resolve()
    device, notes = detect_device(args.adb, args.serial)
    status = "planned-gap"
    if args.execute_device and device.get("state") != "device":
        status = "blocked"
    elif args.execute_device and device.get("state") == "device":
        status = "planned-gap"
        notes.append("--execute-device was requested, but this runner currently records the executable plan only; success remains false.")

    return {
        "schema_version": 1,
        "scenario_id": SCENARIO_ID,
        "plan_gate": PLAN_GATE,
        "status": status,
        "success": False,
        "generated_at": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "device": device,
        "inputs": {"image": args.image, "package": args.package},
        "commands": scenario_commands(args.adb, args.serial, args.package, args.image, artifact_path),
        "artifact_schema": ARTIFACT_SCHEMA,
        "evidence": {
            "pull_log": None,
            "daemon_log_before_kill": None,
            "daemon_log_after_restart": None,
            "store_listing_after_restart": None,
            "image_inspect_after_restart": None,
            "container_run_after_restart": None,
        },
        "negative_expected_conditions": NEGATIVE_EXPECTED_CONDITIONS,
        "cleanup_policy": CLEANUP_POLICY,
        "notes": notes,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", default=str(DEFAULT_ARTIFACT), help="JSON artifact to write")
    parser.add_argument("--adb", default="adb", help="adb executable name/path")
    parser.add_argument("--serial", default=None, help="adb serial to target")
    parser.add_argument("--package", default="com.pdocker.android", help="Android package id under test")
    parser.add_argument("--image", default="busybox:latest", help="small public image used for the interrupted pull")
    parser.add_argument("--execute-device", action="store_true", help="opt in to device probing; still never fakes success")
    parser.add_argument("--print-schema", action="store_true", help="print the artifact schema and exit")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.print_schema:
        print(json.dumps(ARTIFACT_SCHEMA, indent=2, sort_keys=True))
        return 0

    artifact = build_artifact(args)
    out = Path(args.artifact)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(artifact, indent=2, sort_keys=True) + "\n")
    print(f"wrote {out}: status={artifact['status']} success={artifact['success']}")
    return 2 if args.execute_device and artifact["status"] == "blocked" else 0


if __name__ == "__main__":
    raise SystemExit(main())
