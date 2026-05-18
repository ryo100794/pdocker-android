#!/usr/bin/env python3
"""Lightweight verifier for pdocker ADB-free self-debug bundles."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCHEMA = "pdocker.self-debug.bundle.v1"
LATEST_DOCUMENTS_TARGET = "pdocker/diagnostics/self-debug-bundle-latest.json"
EVIDENCE_DOCUMENTS_PREFIX = "pdocker/diagnostics/self-debug-bundle-"
EVIDENCE_DOCUMENTS_SUFFIX = ".json"


class VerificationError(AssertionError):
    pass


def _fail(message: str) -> None:
    raise VerificationError(message)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _obj(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail(f"{path} must be an object")
    return value


def _arr(value: Any, path: str) -> list[Any]:
    if not isinstance(value, list):
        _fail(f"{path} must be an array")
    return value


def _nonempty_str(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value.strip():
        _fail(f"{path} must be a non-empty string")
    return value


def _bool(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        _fail(f"{path} must be a boolean")
    return value


def _number(value: Any, path: str) -> int | float:
    if not _is_number(value):
        _fail(f"{path} must be numeric")
    return value


def _has_explicit_error(obj: dict[str, Any]) -> bool:
    for key in ("Error", "Reason", "Message", "Detail"):
        if isinstance(obj.get(key), str) and obj[key].strip():
            return True
    for attempt in obj.get("Attempts", []):
        if isinstance(attempt, dict) and _has_explicit_error(attempt):
            return True
    return False


def _verify_engine_probe(name: str, value: Any) -> None:
    probe = _obj(value, f"engine.{name}")
    if "Error" in probe:
        _nonempty_str(probe.get("Error"), f"engine.{name}.Error")
        if "Type" in probe:
            _nonempty_str(probe.get("Type"), f"engine.{name}.Type")
        return
    if name == "Ping":
        _number(probe.get("Status"), "engine.Ping.Status")
        _nonempty_str(probe.get("Text"), "engine.Ping.Text")
        return
    if name == "ContainersAll":
        _number(probe.get("Status"), "engine.ContainersAll.Status")
        _arr(probe.get("Items"), "engine.ContainersAll.Items")
        return
    if "Status" in probe:
        _number(probe.get("Status"), f"engine.{name}.Status")
        _nonempty_str(probe.get("Error"), f"engine.{name}.Error")
        return
    _number(probe.get("_HttpStatus"), f"engine.{name}._HttpStatus")


def _verify_documents_export(key: str, value: Any, expected_target: str | None) -> None:
    export = _obj(value, key)
    _nonempty_str(export.get("Source"), f"{key}.Source")
    target = _nonempty_str(export.get("Target"), f"{key}.Target")
    _nonempty_str(export.get("MimeType"), f"{key}.MimeType")
    if export["MimeType"] != "application/json":
        _fail(f"{key}.MimeType must be application/json")
    if expected_target is not None and target != expected_target:
        _fail(f"{key}.Target must be {expected_target}")
    if expected_target is None:
        if not (target.startswith(EVIDENCE_DOCUMENTS_PREFIX) and target.endswith(EVIDENCE_DOCUMENTS_SUFFIX)):
            _fail(f"{key}.Target must be timestamped self-debug bundle path")
        stamp = target[len(EVIDENCE_DOCUMENTS_PREFIX):-len(EVIDENCE_DOCUMENTS_SUFFIX)]
        if not stamp.isdigit():
            _fail(f"{key}.Target must be timestamped self-debug bundle path")
    if "PersistedWriteGrant" in export:
        _bool(export.get("PersistedWriteGrant"), f"{key}.PersistedWriteGrant")
    if "PathValidationPolicy" in export:
        if export.get("PathValidationPolicy") != "fail-closed":
            _fail(f"{key}.PathValidationPolicy must be fail-closed")
    success = _bool(export.get("Success"), f"{key}.Success")
    if "Attempts" in export:
        _arr(export.get("Attempts"), f"{key}.Attempts")
    if success:
        _number(export.get("Bytes"), f"{key}.Bytes")
        _nonempty_str(export.get("Mode"), f"{key}.Mode")
    elif not _has_explicit_error(export):
        _fail(f"{key} failed/planned export must include an explicit Error/Reason/Message")


def verify(path: str | Path) -> dict[str, Any]:
    bundle_path = Path(path)
    try:
        bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        _fail(f"invalid JSON: {exc}")
    bundle = _obj(bundle, "bundle")

    if bundle.get("schema") != SCHEMA:
        _fail(f"schema must be {SCHEMA}")
    if bundle.get("adb_independent") is not True:
        _fail("adb_independent must be true")
    if bundle.get("requires_adb") is not False:
        _fail("requires_adb must be false")
    _number(bundle.get("created_at_epoch_ms"), "created_at_epoch_ms")

    app = _obj(bundle.get("app"), "app")
    for field in ("Package", "Version", "BuildGitCommit", "BuildTimeUtc", "Device", "Abi"):
        _nonempty_str(app.get(field), f"app.{field}")
    _number(app.get("Uid"), "app.Uid")
    _number(app.get("SdkInt"), "app.SdkInt")

    engine = _obj(bundle.get("engine"), "engine")
    for name in ("Ping", "Version", "Info", "ContainersAll"):
        if name not in engine:
            _fail(f"engine.{name} is required")
        _verify_engine_probe(name, engine[name])

    documents = _obj(bundle.get("documents"), "documents")
    _obj(documents.get("Metadata"), "documents.Metadata")
    grant = _obj(documents.get("PersistedGrant"), "documents.PersistedGrant")
    _bool(grant.get("Read"), "documents.PersistedGrant.Read")
    _bool(grant.get("Write"), "documents.PersistedGrant.Write")

    roots = _arr(bundle.get("debug_roots"), "debug_roots")
    if not roots:
        _fail("debug_roots must not be empty")
    for index, root in enumerate(roots):
        item = _obj(root, f"debug_roots[{index}]")
        for field in ("Label", "Path", "Summary"):
            _nonempty_str(item.get(field), f"debug_roots[{index}].{field}")
        _bool(item.get("Writable"), f"debug_roots[{index}].Writable")
        _bool(item.get("Exists"), f"debug_roots[{index}].Exists")

    layers = _obj(bundle.get("memory_layers"), "memory_layers")
    for field in (
        "OsMemTotal",
        "OsMemAvailable",
        "OsSwapTotal",
        "OsSwapFree",
        "PdockerProcessCount",
        "PdockerRss",
        "PdockerSwap",
        "ManagedReserveBytes",
        "ManagedResidentBytes",
    ):
        _number(layers.get(field), f"memory_layers.{field}")
    _bool(layers.get("TransparentRegistered"), "memory_layers.TransparentRegistered")
    _nonempty_str(layers.get("Source"), "memory_layers.Source")

    for field in ("memory_snapshot_text", "process_snapshot_text", "handle_snapshot_text"):
        _nonempty_str(bundle.get(field), field)

    local = _obj(bundle.get("LocalEvidenceFiles"), "LocalEvidenceFiles")
    for field in ("Latest", "Timestamped"):
        _nonempty_str(local.get(field), f"LocalEvidenceFiles.{field}")

    _verify_documents_export("DocumentsExport", bundle.get("DocumentsExport"), LATEST_DOCUMENTS_TARGET)
    _verify_documents_export("DocumentsEvidenceExport", bundle.get("DocumentsEvidenceExport"), None)
    if "DocumentsExportRetry" in bundle:
        _verify_documents_export("DocumentsExportRetry", bundle.get("DocumentsExportRetry"), LATEST_DOCUMENTS_TARGET)

    return bundle


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path, help="self-debug-bundle-latest.json to verify")
    args = parser.parse_args(argv)
    try:
        verify(args.bundle)
    except VerificationError as exc:
        print(f"self-debug bundle verification failed: {exc}", file=sys.stderr)
        return 1
    print(f"OK: {args.bundle} is a valid {SCHEMA} bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
