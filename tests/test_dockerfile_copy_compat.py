import importlib.machinery
import importlib.util
import json
import os
import tempfile
import unittest
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"


def load_pdockerd(home):
    module_name = f"pdockerd_copy_compat_{uuid.uuid4().hex}"
    old_env = os.environ.copy()
    os.environ.update({
        "PDOCKER_HOME": str(home),
        "PDOCKER_TMP_DIR": str(home / "tmp"),
        "PDOCKER_RUNTIME_BACKEND": "no-proot",
    })
    try:
        loader = importlib.machinery.SourceFileLoader(module_name, str(PDOCKERD))
        spec = importlib.util.spec_from_loader(module_name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        return module
    finally:
        os.environ.clear()
        os.environ.update(old_env)


class DockerfileCopyCompatibilityTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.mod = load_pdockerd(self.root / "home")
        self.context = self.root / "context"
        (self.context / "scripts").mkdir(parents=True)
        (self.context / "scripts" / "pdocker-a").write_text("a")
        (self.context / "scripts" / "pdocker-b").write_text("b")
        (self.context / "scripts" / "other").write_text("x")
        self._seed_base_image()

    def tearDown(self):
        self.tmp.cleanup()

    def _seed_base_image(self):
        image_dir = Path(self.mod.image_dir(self.mod.normalize_image("ubuntu:22.04")))
        (image_dir / "rootfs" / "bin").mkdir(parents=True, exist_ok=True)
        (image_dir / "rootfs" / "usr" / "local" / "bin").mkdir(parents=True, exist_ok=True)
        (image_dir / "rootfs" / "bin" / "sh").write_text("# test shell placeholder\n")
        (image_dir / "config.json").write_text(json.dumps({"config": {}}))
        (image_dir / "image_ref").write_text("ubuntu:22.04")

    def _build_context(self, dockerfile):
        (self.context / "Dockerfile").write_text(dockerfile)
        log = []

        def emit(message, newline=True):
            text = str(message)
            log.append(text + ("\n" if newline and not text.endswith("\n") else ""))

        result = self.mod.execute_dockerfile_build(
            str(self.context / "Dockerfile"),
            str(self.context),
            f"local/copy-compat-{uuid.uuid4().hex}:latest",
            {},
            emit,
        )
        return result, "".join(log)

    def test_copy_source_matches_expands_dockerfile_globs_in_context(self):
        matches = self.mod._copy_source_matches(str(self.context), "scripts/pdocker-*")
        rels = [os.path.relpath(path, self.context) for path in matches]

        self.assertEqual(rels, ["scripts/pdocker-a", "scripts/pdocker-b"])

    def test_dockerfile_build_expands_copy_glob_through_builder(self):
        result, log = self._build_context(
            'FROM ubuntu:22.04\n'
            'COPY scripts/pdocker-* /usr/local/bin/\n'
            'CMD ["/bin/sh", "-lc", "true"]\n'
        )

        self.assertIsNotNone(result)
        self.assertNotIn("COPY failed", log)
        self.assertIn("scripts/pdocker-a -> /usr/local/bin/", log)
        self.assertIn("scripts/pdocker-b -> /usr/local/bin/", log)

    def test_dockerfile_build_rejects_missing_copy_glob(self):
        result, log = self._build_context(
            'FROM ubuntu:22.04\n'
            'COPY scripts/missing-* /usr/local/bin/\n'
            'CMD ["/bin/sh", "-lc", "true"]\n'
        )

        self.assertIsNone(result)
        self.assertIn("COPY failed: scripts/missing-* not found", log)
        self.assertNotIn("Successfully built", log)

    def test_copy_source_matches_rejects_context_escape(self):
        with self.assertRaises(RuntimeError):
            self.mod._copy_source_matches(str(self.context), "../outside-*")

    def test_default_workspace_copy_wildcard_has_matching_assets(self):
        default_root = ROOT / "app" / "src" / "main" / "assets" / "default-project"
        dockerfile = (default_root / "Dockerfile").read_text()
        self.assertIn("COPY scripts/pdocker-* /usr/local/bin/", dockerfile)

        matches = self.mod._copy_source_matches(str(default_root), "scripts/pdocker-*")
        rels = [os.path.relpath(path, default_root) for path in matches]

        self.assertGreaterEqual(len(rels), 6)
        self.assertIn("scripts/pdocker-docker", rels)
        self.assertIn("scripts/pdocker-compose", rels)


if __name__ == "__main__":
    unittest.main()
