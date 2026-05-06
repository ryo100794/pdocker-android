import importlib.machinery
import importlib.util
import os
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"


def load_pdockerd(home):
    module_name = f"pdockerd_bind_contract_{uuid.uuid4().hex}"
    loader = importlib.machinery.SourceFileLoader(module_name, str(PDOCKERD))
    spec = importlib.util.spec_from_loader(module_name, loader)
    module = importlib.util.module_from_spec(spec)
    env = {
        "PDOCKER_HOME": str(home),
        "PDOCKER_TMP_DIR": str(home / "tmp"),
        "PDOCKER_RUNTIME_BACKEND": "direct",
        "PDOCKER_DIRECT_EXECUTOR": "",
    }
    with mock.patch.dict(os.environ, env, clear=False):
        loader.exec_module(module)
    return module


class RuntimeBindContractTest(unittest.TestCase):
    def test_direct_runtime_creates_missing_bind_sources_before_validation(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            mod = load_pdockerd(root / "pdocker")
            binds = [
                f"{root}/models/llama-cpp-gpu:/models",
                f"{root}/workspaces/llama-cpp-gpu:/workspace",
                f"{root}/projects/llama-cpp-gpu/./profiles:/profiles",
                "named-volume:/ignored",
            ]

            mod._ensure_bind_hosts(binds)

            self.assertTrue((root / "models/llama-cpp-gpu").is_dir())
            self.assertTrue((root / "workspaces/llama-cpp-gpu").is_dir())
            self.assertTrue((root / "projects/llama-cpp-gpu/profiles").is_dir())

    def test_bind_host_path_normalizes_compose_dot_segments(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            mod = load_pdockerd(root / "pdocker")
            host = mod._bind_host_path(f"{root}/project/./profiles:/profiles")

            self.assertEqual(host, str(root / "project/profiles"))


if __name__ == "__main__":
    unittest.main()
