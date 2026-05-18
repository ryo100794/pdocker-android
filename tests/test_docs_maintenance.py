import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "verify-docs-maintenance.py"

spec = importlib.util.spec_from_file_location("verify_docs_maintenance", SCRIPT)
assert spec and spec.loader
verifier = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = verifier
spec.loader.exec_module(verifier)


BACKLOG = """# Documentation Deduplication Backlog

## Duplicate and scatter hotspots

### 1. Release facts

Canonical owners:

- Release posture: [`../release/RELEASE_READINESS.md`](../release/RELEASE_READINESS.md)

Backlog:

- Link duplicates.

### 2. Llama material

Canonical owners:

- GPU design: [`../design/GPU_COMPAT.md`](../design/GPU_COMPAT.md)

Backlog:

- Add index.

### 3. Memory material

Canonical owners:

- Pager design: [`../design/APK_MEMORY_PAGER.md`](../design/APK_MEMORY_PAGER.md)

Backlog:

- Link design.

### 4. Storage material

Canonical owners:

- Storage architecture: [`../design/STORAGE_LAYER_ARCHITECTURE.md`](../design/STORAGE_LAYER_ARCHITECTURE.md)

Backlog:

- Add index.

### 5. Runtime material

Canonical owners:

- Runtime direction: [`../design/RUNTIME_STRATEGY.md`](../design/RUNTIME_STRATEGY.md)

Backlog:

- Link commands.

### 6. Terminal material

Canonical owners:

- Terminal architecture: [`../design/TERMINAL_STREAM_ARCHITECTURE.md`](../design/TERMINAL_STREAM_ARCHITECTURE.md)

Backlog:

- Link gates.

### 7. Test evidence

Canonical owners:

- Test category rules: [`../test/README.md`](../test/README.md)

Backlog:

- Keep latest pointers.

### 8. Planning material

Canonical owners:

- Current TODOs: [`../plan/TODO.md`](../plan/TODO.md)

Backlog:

- Keep timelines immutable.

## Open backlog count

There are 8 active deduplication backlog groups in this inventory.
"""


class DocsMaintenanceVerifierTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="pdocker-docs-maint-test-"))
        (self.tmp / "docs" / "maintenance").mkdir(parents=True)
        (self.tmp / "docs" / "release").mkdir(parents=True)
        (self.tmp / "docs" / "design").mkdir(parents=True)
        (self.tmp / "docs" / "test").mkdir(parents=True)
        (self.tmp / "docs" / "plan").mkdir(parents=True)
        for path in [
            "docs/release/RELEASE_READINESS.md",
            "docs/design/GPU_COMPAT.md",
            "docs/design/APK_MEMORY_PAGER.md",
            "docs/design/STORAGE_LAYER_ARCHITECTURE.md",
            "docs/design/RUNTIME_STRATEGY.md",
            "docs/design/TERMINAL_STREAM_ARCHITECTURE.md",
            "docs/test/README.md",
            "docs/plan/TODO.md",
        ]:
            (self.tmp / path).write_text("# fixture\n", encoding="utf-8")
        (self.tmp / "docs" / "maintenance" / "DOCUMENTATION_DEDUP_BACKLOG.md").write_text(
            BACKLOG,
            encoding="utf-8",
        )

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_backlog_fixture_passes(self):
        verifier.check_backlog(self.tmp)

    def test_backlog_requires_eight_groups(self):
        backlog = self.tmp / "docs" / "maintenance" / "DOCUMENTATION_DEDUP_BACKLOG.md"
        backlog.write_text(BACKLOG.replace("### 8. Planning material", "## Planning material"), encoding="utf-8")

        with self.assertRaises(verifier.CheckFailure):
            verifier.check_backlog(self.tmp)

    def test_backlog_requires_canonical_owner_section(self):
        backlog = self.tmp / "docs" / "maintenance" / "DOCUMENTATION_DEDUP_BACKLOG.md"
        backlog.write_text(BACKLOG.replace("Canonical owners:", "Canonical owner:", 1), encoding="utf-8")

        with self.assertRaises(verifier.CheckFailure):
            verifier.check_backlog(self.tmp)

    def test_local_markdown_link_checker_ignores_external_urls(self):
        doc = self.tmp / "docs" / "maintenance" / "README.md"
        doc.write_text(
            "[ok](DOCUMENTATION_DEDUP_BACKLOG.md) [external](https://example.invalid/missing)",
            encoding="utf-8",
        )

        self.assertEqual([], verifier.check_local_markdown_links(self.tmp))

    def test_local_markdown_link_checker_reports_missing_relative_target(self):
        doc = self.tmp / "docs" / "maintenance" / "README.md"
        doc.write_text("[missing](NOPE.md)", encoding="utf-8")

        issues = verifier.check_local_markdown_links(self.tmp)
        self.assertEqual(1, len(issues))
        self.assertEqual("NOPE.md", issues[0].target)


if __name__ == "__main__":
    unittest.main()
