import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = ROOT / "docs" / "test" / "CI_GATE_LEDGER.md"
MANIFEST = ROOT / "tests" / "test_driver_manifest.json"


class CiGateLedgerTest(unittest.TestCase):
    def setUp(self):
        self.ledger = LEDGER.read_text(encoding="utf-8")
        self.manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def test_p0_p1_gate_table_names_all_current_focus_areas(self):
        for term in [
            "Service truth",
            "Runtime teardown",
            "Image pull crash safety",
            "OOM/LMK",
            "Terminal `-it`",
            "llama GPU correctness",
            "status=planned-gap",
            "success=false",
        ]:
            self.assertIn(term, self.ledger)

    def test_host_smoke_keeps_planned_gap_contracts_visible(self):
        host_ids = {command["id"] for command in self.manifest["lanes"]["host-smoke"]["commands"]}
        for command_id in [
            "verify-service-truth-plan",
            "verify-image-pull-crash-safety",
            "unittest-all",
        ]:
            self.assertIn(command_id, host_ids)
        for ledger_only in [
            "verify-memory-pager-design",
            "verify-llama-gpu-artifact.py",
        ]:
            self.assertIn(ledger_only, self.ledger)

    def test_device_gates_require_non_passing_artifacts_until_proven(self):
        for artifact in [
            "docs/test/service-truth-latest.json",
            "docs/test/runtime-teardown-latest.json",
            "docs/test/image-pull-crash-safety-latest.json",
            "docs/test/llama-gpu-q6k-workflow-latest.json",
        ]:
            self.assertIn(artifact, self.ledger)
        self.assertIn("must not silently pass", self.ledger)


if __name__ == "__main__":
    unittest.main()
