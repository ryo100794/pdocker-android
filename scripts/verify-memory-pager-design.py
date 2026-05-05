#!/usr/bin/env python3
"""Static contract checks for the APK-scoped memory pager design."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/design/APK_MEMORY_PAGER.md"
PROBE_DOC = ROOT / "docs/test/APK_MEMORY_PAGER_PROBE.md"
DIRECT_EXEC = ROOT / "app/src/main/cpp/pdocker_direct_exec.c"
ANDROID_SMOKE = ROOT / "scripts/android-device-smoke.sh"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(name: str, condition: bool) -> None:
    if not condition:
        fail(name)
    print(f"ok: {name}")


def main() -> int:
    text = DOC.read_text()
    probe = PROBE_DOC.read_text()
    direct = DIRECT_EXEC.read_text()
    smoke = ANDROID_SMOKE.read_text()
    flat = " ".join(text.split())
    require("records that system swap is unavailable to non-root apk", "swapon" in text and "Operation not permitted" in flat and "adb root" in text)
    require("states normal page faults are not globally catchable", "Normal Linux page faults" in flat and "not delivered to user space" in flat)
    require("documents sigsegv pager origin without external code", "Source of the SIGSEGV Pager Idea" in text and "not copied from an external component" in flat and "Guard pages" in text)
    require("defines userfaultfd path but not as default", "userfaultfd" in text and "root-only" in text and "not the current default" in text)
    require("defines ptrace sigsegv fallback", "ptrace SIGSEGV Pager" in text and "PTRACE_GETSIGINFO" in text and "suppress delivery of `SIGSEGV`" in text)
    require("explains how fault address becomes backed", "virtual address must already belong to a reserved managed VMA" in flat and "mmap(PROT_NONE)" in text and "same virtual address" in text)
    require("keeps managed pager opt-in", "PDOCKER_MEMORY_PAGER=managed" in text and "opt-in" in text)
    require("requires sdk28 compat probe gate before runtime feature", "SDK28 Compat Probe Gate" in text and "must not become a runtime feature on hope alone" in flat and "target SDK 28" in text)
    require("probe gate covers android-blockable syscalls", "PTRACE_GETSIGINFO" in text and "process_vm_writev" in text and "mprotect(PROT_READ|PROT_WRITE)" in text)
    require("direct executor exposes apk memory pager probe", "--pdocker-memory-pager-probe" in direct and "pager-probe:ptrace_path" in direct)
    require("device smoke checks compat memory pager probe", "--pdocker-memory-pager-probe" in smoke and "pager-probe:ptrace_path=ok" in smoke)
    require("latest apk memory pager probe result is recorded", "pager-probe:ptrace_path=ok" in probe and "pager-probe:userfaultfd=blocked" in probe and "exact_rc=0" in probe)
    require("excludes unsafe mappings", "thread stacks" in text and "GPU shared buffers" in text and "MAP_SHARED" in text)
    require("keeps llama gpu performance priority separate", "persistent registered buffers" in text and "not expected to make token generation faster" in text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
