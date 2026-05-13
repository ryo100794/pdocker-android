import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFY = ROOT / "scripts" / "verify-image-pull-crash-safety.py"
RUNNER = ROOT / "scripts" / "verify" / "runner" / "image_pull_crash_safety_device.py"


class ImagePullCrashSafetyVerifierTest(unittest.TestCase):
    def test_static_verifier_passes(self):
        subprocess.run([sys.executable, str(VERIFY)], cwd=ROOT, check=True)

    def test_device_runner_writes_planned_gap_without_adb(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "artifact.json"
            subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--adb",
                    "__missing_adb_for_unit_test__",
                    "--artifact",
                    str(artifact),
                ],
                cwd=ROOT,
                check=True,
            )
            data = json.loads(artifact.read_text())

        self.assertEqual(data["scenario_id"], "image.pull.interrupted-kill-restart")
        self.assertEqual(data["status"], "planned-gap")
        self.assertFalse(data["success"])
        self.assertIn("artifact_schema", data)
        self.assertIn("negative_expected_conditions", data)
        self.assertIn("cleanup_policy", data)
        self.assertGreaterEqual(len(data["commands"]), 8)
        joined_negative = "\n".join(data["negative_expected_conditions"])
        self.assertIn(".pull-", joined_negative)
        self.assertIn(".tmp-", joined_negative)
        self.assertIn("old tag", joined_negative)


if __name__ == "__main__":
    unittest.main()
