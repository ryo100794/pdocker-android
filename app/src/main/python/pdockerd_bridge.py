"""Thin Kotlin→Python adapter for invoking pdockerd under Chaquopy.

pdockerd itself is shipped as a submodule-extracted copy under
`src/main/python/pdockerd/` (populated by scripts/copy-native.sh at
build time). This module sets the right environment variables and
hands off to pdockerd's main() — so the daemon code stays identical
to the Linux path.
"""
import os
import sys

def run_daemon(sock_path: str, home: str) -> None:
    os.environ.setdefault("PDOCKER_HOME", home)
    os.environ.setdefault("PDOCKER_NO_COW", "1")  # Android bionic, no glibc shim
    # docker-bin is staged alongside pdockerd by copy-native.sh
    bin_dir = os.path.join(os.path.dirname(__file__), "pdockerd", "docker-bin")
    os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "")

    src_dir = os.path.join(os.path.dirname(__file__), "pdockerd", "bin")
    sys.path.insert(0, src_dir)

    # pdockerd uses __main__ guard, so import its main directly
    import runpy
    sys.argv = ["pdockerd", "--socket", sock_path]
    runpy.run_path(os.path.join(src_dir, "pdockerd"), run_name="__main__")
