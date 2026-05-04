import importlib.machinery
import importlib.util
import json
import os
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"
ASSET_PDOCKERD = ROOT / "app" / "src" / "main" / "assets" / "pdockerd" / "pdockerd"
BRIDGE = ROOT / "app" / "src" / "main" / "python" / "pdockerd_bridge.py"
SERVICE = ROOT / "app" / "src" / "main" / "kotlin" / "io" / "github" / "ryo100794" / "pdocker" / "PdockerdService.kt"


def load_pdockerd(env):
    module_name = f"pdockerd_media_contract_{uuid.uuid4().hex}"
    loader = importlib.machinery.SourceFileLoader(module_name, str(PDOCKERD))
    spec = importlib.util.spec_from_loader(module_name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class MediaBridgeContractTest(unittest.TestCase):
    def test_media_environment_is_phase1_and_not_capture_ready_without_executor(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            media_dir = root / "runtime" / "media"
            media_dir.mkdir(parents=True)
            descriptor = media_dir / "pdocker-media-capabilities.json"
            descriptor.write_text(json.dumps({"Video": {"Devices": [{"Facing": "rear"}]}}))
            env = {
                "PDOCKER_HOME": str(root / "home"),
                "PDOCKER_TMP_DIR": str(root / "tmp"),
                "PDOCKER_MEDIA_HOST_DIR": str(media_dir),
                "PDOCKER_MEDIA_DESCRIPTOR_HOST_PATH": str(descriptor),
                "PDOCKER_MEDIA_DESCRIPTOR_PATH": "/run/pdocker-media/pdocker-media-capabilities.json",
                "PDOCKER_MEDIA_CONTAINER_DIR": "/run/pdocker-media",
                "PDOCKER_MEDIA_QUEUE_SOCKET": "/run/pdocker-media/pdocker-media.sock",
                "PDOCKER_MEDIA_SHARED_DIR": "/run/pdocker-media",
                "PDOCKER_MEDIA_COMMAND_API": "pdocker-media-command-v1",
                "PDOCKER_MEDIA_ABI_VERSION": "0.1",
                "PDOCKER_MEDIA_CONTRACT": "linux-like-socket-env-v1",
                "PDOCKER_MEDIA_DEVICE_PASSTHROUGH": "0",
                "PDOCKER_MEDIA_VIDEO_API": "android-camera2",
                "PDOCKER_MEDIA_AUDIO_API": "android-audiorecord-audiotrack",
                "PDOCKER_MEDIA_AUDIO_DEVICE_API": "android-audiomanager",
                "PDOCKER_MEDIA_CAPTURE_READY": "1",
                "PDOCKER_MEDIA_CAMERA_READY": "1",
                "PDOCKER_MEDIA_AUDIO_READY": "1",
                "PDOCKER_MEDIA_EXECUTOR_AVAILABLE": "0",
            }

            with mock.patch.dict(os.environ, env, clear=False):
                mod = load_pdockerd(env)
                media = mod.collect_media_environment()

        self.assertEqual(media["Kind"], "android-media-bridge-phase1")
        self.assertEqual(media["Contract"], "linux-like-socket-env-v1")
        self.assertFalse(media["RawDevicePassthrough"])
        self.assertFalse(media["CaptureReady"])
        self.assertFalse(media["CameraReady"])
        self.assertFalse(media["AudioReady"])
        self.assertFalse(media["Enabled"])
        self.assertEqual(media["Descriptor"]["Video"]["Devices"][0]["Facing"], "rear")
        self.assertIn("camera.front", media["Video"]["Targets"])
        self.assertIn("audio.usb.multichannel", media["Audio"]["Targets"])

    def test_media_request_env_exposes_specific_targets_without_dev_passthrough(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            media_dir = root / "runtime" / "media"
            media_dir.mkdir(parents=True)
            env = {
                "PDOCKER_HOME": str(root / "home"),
                "PDOCKER_TMP_DIR": str(root / "tmp"),
                "PDOCKER_MEDIA_HOST_DIR": str(media_dir),
                "PDOCKER_MEDIA_CONTAINER_DIR": "/run/pdocker-media",
                "PDOCKER_MEDIA_QUEUE_SOCKET": "/run/pdocker-media/pdocker-media.sock",
                "PDOCKER_MEDIA_SHARED_DIR": "/run/pdocker-media",
                "PDOCKER_MEDIA_CONTRACT": "linux-like-socket-env-v1",
                "PDOCKER_MEDIA_DEVICE_PASSTHROUGH": "0",
            }

            with mock.patch.dict(os.environ, env, clear=False):
                mod = load_pdockerd(env)
                state = {
                    "HostConfig": {
                        "DeviceRequests": [
                            {
                                "Driver": "pdocker-media",
                                "Capabilities": [["camera", "microphone"]],
                                "Options": {"pdocker.camera.rear": "true", "pdocker.usb_audio": "true"},
                            }
                        ]
                    },
                    "Labels": {"pdocker.media": "speaker"},
                }
                media_env = mod._media_env(state)
                binds = mod._media_binds(state)

        modes = set(media_env["PDOCKER_MEDIA_MODES"].split(","))
        self.assertTrue({
            "video.camera2",
            "camera.front",
            "camera.rear",
            "audio.capture",
            "audio.playback",
            "audio.usb.multichannel",
        }.issubset(modes))
        self.assertEqual(media_env["PDOCKER_MEDIA"], "phase1")
        self.assertEqual(media_env["PDOCKER_MEDIA_ENABLED"], "0")
        self.assertEqual(media_env["PDOCKER_MEDIA_DEVICE_PASSTHROUGH"], "0")
        self.assertEqual(binds, [f"{media_dir}:/run/pdocker-media"])
        self.assertNotIn("/dev/", "".join(binds))

    def test_android_service_scaffold_targets_public_media_apis(self):
        source = SERVICE.read_text()

        self.assertIn("CameraManager", source)
        self.assertIn("CameraCharacteristics.LENS_FACING_FRONT", source)
        self.assertIn("CameraCharacteristics.LENS_FACING_BACK", source)
        self.assertIn("AudioManager", source)
        self.assertIn("AudioRecord", source)
        self.assertIn("AudioTrack", source)
        self.assertIn("AudioDeviceInfo.TYPE_USB_DEVICE", source)
        self.assertIn('"RawDevicePassthrough", false', source)
        self.assertIn('"CaptureReady", false', source)

    def test_bridge_exports_socket_env_contract_and_asset_matches_source(self):
        bridge = BRIDGE.read_text()

        self.assertIn('PDOCKER_MEDIA_CONTRACT"] = "linux-like-socket-env-v1"', bridge)
        self.assertIn('PDOCKER_MEDIA_DEVICE_PASSTHROUGH"] = "0"', bridge)
        self.assertIn('PDOCKER_MEDIA_QUEUE_SOCKET"] = "/run/pdocker-media/pdocker-media.sock"', bridge)
        self.assertIn("audio.usb.multichannel", bridge)
        self.assertEqual(PDOCKERD.read_text(), ASSET_PDOCKERD.read_text())


if __name__ == "__main__":
    unittest.main()
