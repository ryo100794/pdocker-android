import json
import importlib.machinery
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VERIFY = ROOT / "scripts" / "verify-image-pull-crash-safety.py"
RUNNER = ROOT / "scripts" / "verify" / "runner" / "image_pull_crash_safety_device.py"
DEVICE_RUNNER = ROOT / "scripts" / "verify" / "runner" / "image-pull-crash-safety-device.sh"
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"


def load_pdockerd(home: Path, tmp: Path):
    old_home = os.environ.get("PDOCKER_HOME")
    old_tmp = os.environ.get("PDOCKER_TMP_DIR")
    os.environ["PDOCKER_HOME"] = str(home)
    os.environ["PDOCKER_TMP_DIR"] = str(tmp)
    try:
        name = f"pdockerd_crash_safety_{os.getpid()}_{len(sys.modules)}"
        loader = importlib.machinery.SourceFileLoader(name, str(PDOCKERD))
        spec = importlib.util.spec_from_loader(name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        return module
    finally:
        if old_home is None:
            os.environ.pop("PDOCKER_HOME", None)
        else:
            os.environ["PDOCKER_HOME"] = old_home
        if old_tmp is None:
            os.environ.pop("PDOCKER_TMP_DIR", None)
        else:
            os.environ["PDOCKER_TMP_DIR"] = old_tmp


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
        self.assertEqual(data["schema_version"], 2)
        self.assertEqual(data["status"], "planned-gap")
        self.assertFalse(data["success"])
        self.assertIn("artifact_schema", data)
        self.assertEqual(data["phases"], ["prepare-residue", "kill-daemon", "restart-and-probe", "cleanup"])
        self.assertFalse(data["coverage"]["live_interrupted_network_pull"])
        self.assertEqual(data["live_pull_interruption"]["phase"], "timed-live-pull-interruption")
        self.assertEqual(data["live_pull_interruption"]["status"], "planned-gap")
        self.assertFalse(data["live_pull_interruption"]["success"])
        self.assertFalse(data["live_pull_interruption"]["runnable"])
        self.assertIn("--execute-live-pull-interruption", data["live_pull_interruption"]["required_cli"])
        self.assertIn("--live-fixture-owned", data["live_pull_interruption"]["required_cli"])
        self.assertTrue(any("--live-image" in item for item in data["live_pull_interruption"]["required_cli"]))
        self.assertIn("remaining_gap", data)
        self.assertIn("negative_expected_conditions", data)
        self.assertIn("cleanup_policy", data)
        self.assertIn("partial_image_inspect_after_restart", data["evidence"])
        self.assertIn("partial_image_create_after_restart", data["evidence"])
        self.assertIn("partial_image_inspect_rejected", data["assertions"])
        self.assertIn("partial_image_create_rejected", data["assertions"])
        self.assertGreaterEqual(len(data["commands"]), 8)
        joined_negative = "\n".join(data["negative_expected_conditions"])
        self.assertIn(".pull-", joined_negative)
        self.assertIn(".tmp-", joined_negative)
        self.assertIn("old tag", joined_negative)
        self.assertIn("partial image", joined_negative)

    def test_device_runner_execute_without_device_is_blocked_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "artifact.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--adb",
                    "__missing_adb_for_unit_test__",
                    "--artifact",
                    str(artifact),
                    "--execute-device",
                ],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            data = json.loads(artifact.read_text())

        self.assertEqual(result.returncode, 2)
        self.assertEqual(data["status"], "blocked")
        self.assertFalse(data["success"])
        self.assertEqual(data["phase_results"], [])
        self.assertIsNone(data["assertions"]["old_tag_restored"])

    def test_live_pull_interruption_opt_in_stays_planned_gap_until_device_phase_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            artifact = Path(tmp) / "artifact.json"
            result = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--adb",
                    "__missing_adb_for_unit_test__",
                    "--artifact",
                    str(artifact),
                    "--execute-live-pull-interruption",
                    "--live-image",
                    "127.0.0.1:5000/pdocker-crash-safety-fixture:test",
                    "--live-fixture-owned",
                    "--live-interrupt-after-seconds",
                    "1.5",
                ],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            )
            data = json.loads(artifact.read_text())

        self.assertIn("status=planned-gap", result.stdout)
        self.assertEqual(data["status"], "planned-gap")
        self.assertFalse(data["success"])
        self.assertFalse(data["coverage"]["live_interrupted_network_pull"])
        live = data["live_pull_interruption"]
        self.assertTrue(live["requested"])
        self.assertEqual(live["live_image"], "127.0.0.1:5000/pdocker-crash-safety-fixture:test")
        self.assertTrue(live["fixture_owned_or_isolated"])
        self.assertEqual(live["interrupt_after_seconds"], 1.5)
        self.assertFalse(live["runnable"])
        self.assertFalse(live["success"])
        self.assertEqual(live["status"], "planned-gap")
        self.assertIn("device-side timed live pull interruption phase is not implemented", live["blocked_reason"])

    def test_device_side_runner_is_scenario_scoped(self):
        text = DEVICE_RUNNER.read_text()
        for marker in ["prepare-residue", "kill-daemon", "restart-and-probe", "cleanup"]:
            self.assertIn(marker, text)
        for marker in [".pull-$TOKEN", ".old-$TOKEN", ".tmp-$TOKEN", "inspect-restored.raw", "inspect-never.raw"]:
            self.assertIn(marker, text)
        for marker in ["$PARTIAL_BASE", "inspect-partial.raw", "create-partial.raw", "partial_image_create_rejected"]:
            self.assertIn(marker, text)
        self.assertIn("pkill -TERM -f pdockerd", text)
        self.assertIn("rm -rf \\", text)
        self.assertIn("$IMG_BASE", text)
        self.assertIn("$NEVER_BASE", text)
        self.assertIn("$TOKEN", text)
        for forbidden in [
            "rm -rf files/pdocker",
            "rm -rf pdocker/images",
            "rm -rf pdocker/layers",
            "rm -rf /data",
            "rm -rf /sdcard",
        ]:
            self.assertNotIn(forbidden, text)

    def test_layer_cache_requires_meta_tree_and_matching_diff_id(self):
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as tmp:
            pdockerd = load_pdockerd(Path(home), Path(tmp))
            did = "a" * 64
            ldir = Path(pdockerd.LAYERS_DIR) / did
            (ldir / "tree").mkdir(parents=True)
            self.assertFalse(pdockerd._layer_exists(did))

            (ldir / "meta.json").write_text(json.dumps({"diff_id": "sha256:" + ("b" * 64), "size": 0}))
            self.assertFalse(pdockerd._layer_exists(did))

            (ldir / "meta.json").write_text(json.dumps({"diff_id": "sha256:" + did, "size": 0}))
            self.assertTrue(pdockerd._layer_exists(did))

    def test_partial_image_with_incomplete_layer_is_not_inspectable_or_runnable(self):
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as tmp:
            pdockerd = load_pdockerd(Path(home), Path(tmp))
            ref = "pdocker-crash-safety-partial:unit"
            norm = pdockerd.normalize_image(ref)
            img_dir = Path(pdockerd.image_dir(norm))
            did = "c" * 64
            (img_dir / "rootfs").mkdir(parents=True)
            (img_dir / "config.json").write_text(json.dumps({
                "architecture": "arm64",
                "os": "linux",
                "rootfs": {"type": "layers", "diff_ids": ["sha256:" + did]},
                "config": {"Cmd": ["/bin/true"]},
            }))
            (img_dir / "manifest.json").write_text(json.dumps({
                "schemaVersion": 2,
                "layers": [{"digest": "sha256:" + did, "diff_id": "sha256:" + did}],
                "config_ref": "config.json",
            }))
            (img_dir / "image_ref").write_text(norm)
            ldir = Path(pdockerd.LAYERS_DIR) / did
            ldir.mkdir(parents=True)
            (ldir / "meta.json").write_text(json.dumps({"diff_id": "sha256:" + did, "size": 1}))

            self.assertIsNone(pdockerd.image_config(ref))
            with self.assertRaisesRegex(ValueError, "incomplete or has partial layers"):
                pdockerd.create_container({"Image": ref, "Cmd": ["/bin/true"]}, name="partial-unit")


if __name__ == "__main__":
    unittest.main()
