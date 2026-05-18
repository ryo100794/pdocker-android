import importlib.util
import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "scripts" / "llama-gpu-env-manifest.json"
VERIFIER = ROOT / "scripts" / "verify-llama-gpu-artifact.py"
COMPARE = ROOT / "scripts" / "android-llama-gpu-compare.sh"
PDOCKERD = ROOT / "app" / "src" / "main" / "assets" / "pdockerd" / "pdockerd"
LLAMA_COMPOSE = ROOT / "app" / "src" / "main" / "assets" / "project-library" / "llama-cpp-gpu" / "compose.yaml"


def load_verifier():
    spec = importlib.util.spec_from_file_location("llama_gpu_artifact_verifier", VERIFIER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


class LlamaGpuEnvParityTest(unittest.TestCase):
    def test_manifest_env_groups_are_ordered_unique_and_subset_consistent(self):
        manifest = load_manifest()
        self.assertEqual(manifest["schema"], "pdocker.llama.gpu.env-manifest.v1")
        for key in [
            "ui_runtime_env_keys",
            "pdockerd_runtime_env_keys",
            "ui_compose_runtime_env_keys",
            "compare_diagnostic_env_keys",
            "compare_forward_env_keys",
        ]:
            values = manifest[key]
            self.assertTrue(values, key)
            self.assertEqual(len(values), len(set(values)), key)
            self.assertTrue(all(isinstance(value, str) and value for value in values), key)

        ui_runtime = set(manifest["ui_runtime_env_keys"])
        self.assertEqual(manifest["ui_runtime_env_keys"], manifest["pdockerd_runtime_env_keys"])
        self.assertLessEqual(set(manifest["ui_compose_runtime_env_keys"]), ui_runtime)
        self.assertLessEqual(set(manifest["compare_diagnostic_env_keys"]), set(manifest["compare_forward_env_keys"]))
        self.assertLessEqual(ui_runtime, set(manifest["compare_forward_env_keys"]))

    def test_verifier_constants_are_loaded_from_the_same_manifest(self):
        manifest = load_manifest()
        verifier = load_verifier()
        self.assertEqual(tuple(manifest["ui_runtime_env_keys"]), verifier.LLAMA_GPU_UI_RUNTIME_ENV_KEYS)
        self.assertEqual(tuple(manifest["pdockerd_runtime_env_keys"]), verifier.LLAMA_GPU_PDOCKERD_RUNTIME_ENV_KEYS)
        self.assertEqual(tuple(manifest["ui_compose_runtime_env_keys"]), verifier.LLAMA_GPU_UI_COMPOSE_RUNTIME_ENV_KEYS)
        self.assertEqual(tuple(manifest["compare_diagnostic_env_keys"]), verifier.LLAMA_GPU_COMPARE_DIAGNOSTIC_ENV_KEYS)
        self.assertEqual(tuple(manifest["compare_forward_env_keys"]), verifier.LLAMA_GPU_COMPARE_FORWARD_ENV_KEYS)

    def test_compare_pdockerd_and_ui_compose_env_surfaces_match_manifest(self):
        manifest = load_manifest()
        compare = COMPARE.read_text(encoding="utf-8")
        pdockerd = PDOCKERD.read_text(encoding="utf-8")
        compose = LLAMA_COMPOSE.read_text(encoding="utf-8")

        # The compare script must stay manifest-driven instead of carrying a
        # second hand-maintained diagnostic env list.
        self.assertIn("llama-gpu-env-manifest.json", compare)
        self.assertIn("compare_forward_env_keys", compare)
        self.assertIn('set_env(env, f"{key}={value}")', compare)

        for key in manifest["pdockerd_runtime_env_keys"]:
            self.assertRegex(
                pdockerd,
                rf'"{re.escape(key)}"\s*:\s*os\.environ\.get\("{re.escape(key)}"',
                key,
            )

        for key in manifest["ui_compose_runtime_env_keys"]:
            self.assertIn(f"{key}:", compose, key)
            self.assertIn(f"${{{key}:-", compose, key)

        # Compare-only diagnostics must remain absent from the UI compose
        # template until promoted to ordinary runtime behavior.
        for key in sorted(set(manifest["compare_diagnostic_env_keys"]) - set(manifest["ui_compose_runtime_env_keys"])):
            self.assertNotIn(f"{key}:", compose, key)


if __name__ == "__main__":
    unittest.main()
