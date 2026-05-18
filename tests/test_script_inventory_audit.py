import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ScriptInventoryAuditTest(unittest.TestCase):
    def setUp(self):
        self.inventory = json.loads((ROOT / "scripts" / "script-inventory.json").read_text(encoding="utf-8"))
        self.readme = (ROOT / "scripts" / "README.md").read_text(encoding="utf-8")
        self.entries = {entry["path"]: entry for entry in self.inventory["entries"]}

    def test_obsolete_suspects_have_audit_decisions_and_replacements(self):
        expected_replacements = {
            "scripts/android-terminal-it-repro.sh": "python3 scripts/pdocker-test-driver.py --lane android-terminal-exec-it",
            "scripts/verify-llama-startup-logging.py": "python3 scripts/verify-project-library.py",
            "scripts/wrap-ndk-box64.sh": "bash scripts/build-native-termux.sh",
        }

        obsolete = {
            entry["path"]
            for entry in self.inventory["entries"]
            if entry["category"] == "obsolete-suspect"
        }
        self.assertEqual(set(expected_replacements), obsolete)

        for path, replacement in expected_replacements.items():
            with self.subTest(path=path):
                entry = self.entries[path]
                audit = entry.get("audit")
                self.assertIsInstance(audit, dict)
                self.assertEqual(audit["date"], "2026-05-18")
                self.assertIn("no", audit["reference_scan"].lower())
                self.assertEqual(audit["replacement_command"], replacement)
                self.assertIn("keep", audit["decision"].lower())
                self.assertRegex(audit["decision"].lower(), r"delet(e|ion)")
                self.assertIn(path, self.readme)
                self.assertIn(replacement, self.readme)


if __name__ == "__main__":
    unittest.main()
