import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_direct_syscall_scenarios.py"

spec = importlib.util.spec_from_file_location("run_direct_syscall_scenarios", SCRIPT)
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)


def write_manifest(path: Path) -> None:
    path.write_text(
        json.dumps(
            {
                "schema": 1,
                "heavy_cases": [
                    {
                        "id": "local.echo",
                        "tier": "fast-local",
                        "command": "printf ok",
                        "runnable": True,
                        "checks": "prints ok",
                    },
                    {
                        "id": "android.plan",
                        "tier": "heavy-android",
                        "command": "Run an Android device probe later.",
                        "runnable": False,
                        "checks": "device behavior is documented",
                    },
                ],
            }
        )
    )


class DirectSyscallScenarioRunnerTest(unittest.TestCase):
    def test_manifest_marks_runnable_and_planned_cases(self):
        manifest = runner.load_manifest(runner.MANIFEST)
        cases = {case["id"]: case for case in runner.scenario_cases(manifest)}

        self.assertEqual(runner.case_status(cases["host.local.libcow"]), runner.STATUS_RUNNABLE)
        self.assertEqual(
            runner.case_status(cases["android.direct.path-open-stat-access-cwd"]),
            runner.STATUS_RUNNABLE,
        )
        self.assertEqual(runner.case_status(cases["android.direct.unix-socket-connect"]), runner.STATUS_PLANNED)

    def test_json_output_can_filter_to_runnable_cases(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / "manifest.json"
            write_manifest(manifest)

            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                rc = runner.main(["--manifest", str(manifest), "--status", "runnable", "--json"])

        self.assertEqual(rc, 0)
        payload = json.loads(stdout.getvalue())
        self.assertEqual([case["id"] for case in payload["cases"]], ["local.echo"])
        self.assertEqual(payload["cases"][0]["status"], runner.STATUS_RUNNABLE)

    def test_dry_run_prints_command_without_executing(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / "manifest.json"
            write_manifest(manifest)
            stdout = io.StringIO()

            with mock.patch.object(runner.subprocess, "run", side_effect=AssertionError("executed")):
                with contextlib.redirect_stdout(stdout):
                    rc = runner.main(
                        ["--manifest", str(manifest), "--status", "runnable", "--execute", "--dry-run"]
                    )

        self.assertEqual(rc, 0)
        self.assertIn("exec: printf ok", stdout.getvalue())
        self.assertIn("dry-run: command not executed", stdout.getvalue())

    def test_planned_cases_are_skipped_on_execute(self):
        with tempfile.TemporaryDirectory() as td:
            manifest = Path(td) / "manifest.json"
            write_manifest(manifest)
            stdout = io.StringIO()

            with mock.patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
                with contextlib.redirect_stdout(stdout):
                    rc = runner.main(["--manifest", str(manifest), "--status", "planned", "--execute"])

        self.assertEqual(rc, 0)
        self.assertIn("skip: scenario is still an implementation plan", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
