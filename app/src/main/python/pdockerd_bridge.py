"""Kotlin -> pdockerd adapter run inside Chaquopy.

Kotlin stages the expected pdockerd project layout at `runtime_dir`
(bin/pdockerd, docker-bin/crane, docker-bin/proot, lib/libcow.so), so
pdockerd's `_PROJECT_DIR = dirname(dirname(__file__))` resolves to that
directory and the daemon finds all native deps via the same relative
paths it uses on a Linux host. We just exec the script via runpy.
"""
import os
import runpy
import sys


def run_daemon(sock_path: str, home: str, runtime_dir: str) -> None:
    os.environ["PDOCKER_HOME"] = home

    # The bundled proot is the aarch64 Android ELF shipped via jniLibs.
    # PDOCKER_RUNNER makes pdockerd skip its own runner discovery and
    # use exactly this binary. The symlink resolves to nativeLibraryDir,
    # which is the only exec-allowed location on API 29+.
    os.environ["PDOCKER_RUNNER"] = os.path.join(runtime_dir, "docker-bin", "proot")

    bin_dir = os.path.join(runtime_dir, "docker-bin")
    os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")

    pdockerd_path = os.path.join(runtime_dir, "bin", "pdockerd")
    sys.argv = ["pdockerd", "--socket", sock_path]
    runpy.run_path(pdockerd_path, run_name="__main__")
