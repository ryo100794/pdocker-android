import contextlib
import importlib.util
import io
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "verify-metadata-index.py"

spec = importlib.util.spec_from_file_location("verify_metadata_index", SCRIPT)
verify_metadata_index = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = verify_metadata_index
spec.loader.exec_module(verify_metadata_index)


class MetadataIndexVerifierTest(unittest.TestCase):
    def test_fixture_schema_validates(self):
        messages = verify_metadata_index.run_verification(check_doc=True)

        self.assertIn("schema applies", messages)
        self.assertIn("project rename preserves project_id", messages)

    def test_schema_rejects_name_as_project_identity(self):
        with tempfile.TemporaryDirectory() as td:
            db = Path(td) / "metadata.sqlite"
            with verify_metadata_index.connect(db) as con:
                verify_metadata_index.apply_schema(con)
                with self.assertRaises(sqlite3.IntegrityError):
                    con.execute(
                        """
                        INSERT INTO projects
                        (project_id, display_name, project_root, created_at_ms, updated_at_ms)
                        VALUES ('dev-workspace', 'Dev Workspace', 'projects/dev-workspace', 1, 1)
                        """
                    )

    def test_main_prints_pass(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            rc = verify_metadata_index.main([])

        self.assertEqual(rc, 0)
        self.assertIn("verify-metadata-index: PASS", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
