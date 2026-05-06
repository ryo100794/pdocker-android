import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class TestDriverManifestTest(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((ROOT / "tests" / "test_driver_manifest.json").read_text())
        self.driver = (ROOT / "scripts" / "pdocker-test-driver.py").read_text()

    def test_canonical_driver_and_manifest_are_declared(self):
        self.assertEqual(self.manifest["schema"], "pdocker.test-driver.v1")
        self.assertEqual(self.manifest["policy"]["canonical_driver"], "scripts/pdocker-test-driver.py")
        self.assertEqual(self.manifest["artifact_manifest"], "docs/test/test-run-latest.json")
        self.assertIn("def run_command", self.driver)
        self.assertIn("manifest.json", self.driver)

    def test_every_command_has_stable_id_and_executable_form(self):
        lanes = self.manifest["lanes"]
        self.assertIn("host-smoke", lanes)
        self.assertIn("android-test-suite", lanes)
        self.assertIn("android-file-io-microbench", lanes)
        ids = set()
        for lane_name, lane in lanes.items():
            self.assertGreater(len(lane.get("commands", [])), 0, lane_name)
            for command in lane["commands"]:
                cid = command.get("id")
                self.assertIsInstance(cid, str)
                self.assertNotIn(cid, ids)
                ids.add(cid)
                self.assertTrue(
                    ("argv" in command) ^ ("shell" in command),
                    f"{lane_name}/{cid} must use exactly one command form",
                )

    def test_artifact_management_is_single_manifest_based(self):
        policy = self.manifest["policy"]
        self.assertIn("one run manifest", policy["artifact_rule"])
        self.assertIn("sha256", self.driver)
        self.assertIn('"artifacts"', self.driver)
        self.assertNotIn("docs/test/*-latest.json", policy["artifact_rule"])

    def test_benchmark_lane_exports_documents_evidence(self):
        lane = self.manifest["lanes"]["android-file-io-microbench"]
        command = lane["commands"][0]
        self.assertEqual(command["id"], "android-file-io-microbench")
        self.assertIn("docs/test/file-io-microbench-latest.json", command["artifacts"])
        self.assertEqual(command["env"]["PDOCKER_FILE_IO_MICRO_EXPORT_DOCUMENTS"], "1")


if __name__ == "__main__":
    unittest.main()
