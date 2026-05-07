import importlib.machinery
import importlib.util
import json
import os
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"
ASSET_PDOCKERD = ROOT / "app" / "src" / "main" / "assets" / "pdockerd" / "pdockerd"


def load_pdockerd(home):
    module_name = f"pdockerd_state_io_{uuid.uuid4().hex}"
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


class ContainerStateIoTest(unittest.TestCase):
    def test_container_state_save_uses_unique_atomic_temp_and_fsync(self):
        source = PDOCKERD.read_text()
        self.assertIn('tmp = f"{path}.tmp.{os.getpid()}.{threading.get_ident()}"', source)
        self.assertIn("os.fsync(f.fileno())", source)
        self.assertIn("os.replace(tmp, path)", source)
        self.assertEqual(source, ASSET_PDOCKERD.read_text())

    def test_container_state_loader_repairs_trailing_stale_json(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            mod = load_pdockerd(root / "pdocker")
            cid = "stateio"
            cdir = Path(mod.CONTAINERS_DIR) / cid
            cdir.mkdir(parents=True)
            state = {"Id": cid, "State": {"Running": True}, "Config": {"Env": []}}
            path = cdir / "state.json"
            path.write_text(json.dumps(state, indent=2) + "       " + '"stale": true}\n')

            loaded = mod.load_container_state(cid)

            self.assertEqual(loaded, state)
            self.assertEqual(json.loads(path.read_text()), state)


if __name__ == "__main__":
    unittest.main()
