#!/usr/bin/env python3
"""Detailed regression tests for the pdockerd backend selection contract.

These checks run quickly and focus on the behavior that is most likely to
regress while switching runtime backends (notably the no-PRoot/direct path).
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import shutil
import tempfile
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "bin" / "pdockerd"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def ok(msg: str) -> None:
    print(f"ok: {msg}")


def load_pdockerd_with_env(
    name: str,
    runtime_backend: str,
    home: Path,
    extra_env: dict[str, str] | None = None,
):
    env = os.environ.copy()
    os.environ["PDOCKER_RUNTIME_BACKEND"] = runtime_backend
    os.environ["PDOCKER_HOME"] = str(home)
    if extra_env:
        os.environ.update(extra_env)
    else:
        os.environ.pop("PDOCKER_DIRECT_EXECUTOR", None)
    if "PDOCKER_RUNTIME_PREFLIGHT" not in env:
        os.environ["PDOCKER_RUNTIME_PREFLIGHT"] = "0"
    # Force a deterministic no-proxy setup path in test env.
    os.environ.setdefault("LD_LIBRARY_PATH", str((ROOT / "lib").resolve()))

    # Ensure the imported module can be reloaded fresh for each backend mode.
    module_name = f"pdockerd_test_{name}"
    for key in [k for k in sys.modules if k.startswith("pdockerd_test_")]:
        if k != "importlib.machinery" and k != "importlib.util":
            del sys.modules[k]

    loader = importlib.machinery.SourceFileLoader(module_name, str(PDOCKERD))
    spec = importlib.util.spec_from_loader(module_name, loader)
    if spec is None or spec.loader is None:
        fail("failed to create import spec for pdockerd")
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


def test_direct_backend_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("direct", "no-proot", home_path)

        if mod.runtime_backend_kind() != "direct":
            fail(f"backend kind must be direct, got {mod.runtime_backend_kind()}")
        ok("runtime backend kind selects direct")

        if mod.runtime_driver_name() != "pdocker-direct":
            fail(f"driver name mismatch: {mod.runtime_driver_name()}")
        ok("runtime driver name is pdocker-direct")

        if mod.runtime_info_id() != "PDOCKER:DIRECT":
            fail(f"info id mismatch: {mod.runtime_info_id()}")
        ok("runtime info id is PDOCKER:DIRECT")

        msg = mod.runtime_backend_unavailable_message()
        if msg:
            fail(f"direct backend should be available, got: {msg}")
        ok("runtime unavailable message is clear")

        process_msg = mod.runtime_process_unavailable_message()
        if "cannot execute container processes yet" not in process_msg:
            fail(f"direct process unavailable message mismatch: {process_msg!r}")
        ok("direct backend reports process execution gap")

        rootfs = home_path / "rootfs"
        (rootfs / "bin").mkdir(parents=True)
        (rootfs / "bin" / "sh").write_text("#!/bin/sh\n")
        pre = mod.runtime_preflight(str(rootfs), env={}, workdir="/", binds=None, cow_bind=None)
        if pre:
            fail(f"runtime_preflight should pass for direct backend: {pre!r}")
        ok("runtime_preflight accepts direct backend")

        try:
            mod.build_run_argv(str(rootfs), ["/bin/sh", "-c", "echo hi"], {}, "/", None, None)
        except RuntimeError as exc:
            if "cannot execute container processes yet" not in str(exc):
                fail(f"direct build_run_argv raised wrong error: {exc}")
        else:
            fail("direct build_run_argv should require a probed executor")
        ok("build_run_argv rejects direct execution without helper")


def test_direct_executor_probe_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        helper = home_path / "pdocker-direct-helper"
        helper.write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = \"--pdocker-direct-probe\" ]; then\n"
            "  echo pdocker-direct-executor:1\n"
            "  echo process-exec=0\n"
            "  exit 0\n"
            "fi\n"
            "exit 99\n"
        )
        helper.chmod(0o755)
        mod = load_pdockerd_with_env(
            "direct_helper",
            "no-proot",
            home_path,
            {"PDOCKER_DIRECT_EXECUTOR": str(helper)},
        )
        if not mod.direct_executor_available():
            fail("valid direct helper probe should be detected")
        process_msg = mod.runtime_process_unavailable_message()
        if "does not advertise process-exec=1" not in process_msg:
            fail(f"helper without process-exec=1 should remain blocked: {process_msg!r}")
        try:
            mod.build_run_argv(str(home_path / "rootfs"), ["/bin/echo"], {}, "/", None, None)
        except RuntimeError as exc:
            if "does not advertise process-exec=1" not in str(exc):
                fail(f"direct helper capability rejection mismatch: {exc}")
        else:
            fail("direct helper without process-exec=1 should not build argv")
        ok("direct executor probe alone does not enable process execution")


def test_direct_executor_process_capability_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        helper = home_path / "pdocker-direct-helper"
        helper.write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = \"--pdocker-direct-probe\" ]; then\n"
            "  echo pdocker-direct-executor:1\n"
            "  echo process-exec=1\n"
            "  exit 0\n"
            "fi\n"
            "exit 99\n"
        )
        helper.chmod(0o755)
        mod = load_pdockerd_with_env(
            "direct_helper_exec",
            "no-proot",
            home_path,
            {"PDOCKER_DIRECT_EXECUTOR": str(helper)},
        )
        if mod.runtime_process_unavailable_message():
            fail("direct helper with process-exec=1 should make process execution available")
        rootfs = home_path / "rootfs"
        rootfs.mkdir()
        argv = mod.build_run_argv(
            str(rootfs),
            ["/bin/echo", "hi"],
            {"A": "B"},
            "/work",
            binds=["/host:/guest:ro"],
            cow_bind={"upper": "/upper", "lower": "/lower", "guest_path": "/"},
            mode="exec",
        )
        joined = " ".join(argv)
        for token in (str(helper), "--rootfs", str(rootfs), "--workdir", "/work", "--env", "A=B", "--bind", "/host:/guest:ro", "--cow-upper", "/upper", "--", "/bin/echo"):
            if token not in joined:
                fail(f"direct helper argv missing {token!r}: {argv!r}")
        ok("direct executor process capability enables structured helper argv")


def test_direct_backend_rejects_fake_container_start() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("direct_start", "no-proot", home_path)
        image = "ubuntu:22.04"
        img_dir = Path(mod.image_dir(mod.normalize_image(image)))
        rootfs = img_dir / "rootfs"
        (rootfs / "bin").mkdir(parents=True)
        (rootfs / "usr/local/bin").mkdir(parents=True)
        (rootfs / "workspace").mkdir(parents=True)
        (rootfs / "bin" / "sh").write_text("#!/bin/sh\n")
        (rootfs / "usr/local/bin" / "start-code-server").write_text("#!/bin/sh\n")
        (img_dir / "config.json").write_text(
            '{"config":{"Cmd":["/usr/local/bin/start-code-server"],"Env":[]}}'
        )
        state = mod.create_container(
            {
                "Image": image,
                "Cmd": ["/usr/local/bin/start-code-server"],
                "WorkingDir": "/workspace",
                "Env": ["CODE_SERVER_PORT=18080"],
            },
            name="direct-dev",
        )
        try:
            mod.start_container(state["Id"])
        except RuntimeError as exc:
            message = str(exc)
        else:
            fail("direct backend started a fake container instead of rejecting execution")
        if "cannot execute container processes yet" not in message:
            fail(f"direct start rejection did not explain executor gap: {message!r}")
        saved = mod.load_container_state(state["Id"])
        if saved["State"]["Running"]:
            fail("direct backend marked rejected container as running")
        if saved["State"]["ExitCode"] != 126:
            fail(f"direct backend exit code mismatch: {saved['State']['ExitCode']!r}")
        log_path = Path(mod.LOGS_DIR) / f"{state['Id']}.log"
        text = log_path.read_text(errors="replace") if log_path.exists() else ""
        if "fake listener" not in text:
            fail("direct backend log did not state that no fake listener was started")
        ok("direct backend rejects fake service start honestly")


def test_default_no_proot_runtime_path() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("default", "no-proot", home_path)

        if not mod.RUNTIME_BACKEND or mod.RUNTIME_BACKEND.kind != "direct":
            fail("requested no-proot backend was not selected")
        ok("no-proot request creates direct backend instance")


def seed_legacy_image(mod, image: str) -> None:
    img_dir = Path(mod.image_dir(mod.normalize_image(image)))
    rootfs = img_dir / "rootfs"
    (rootfs / "bin").mkdir(parents=True)
    (rootfs / "bin" / "sh").write_text("#!/bin/sh\n")
    (img_dir / "config.json").write_text('{"config":{"Env":[]}}')


def seed_layered_image(mod, image: str, diff_ids: list[str], config: dict | None = None) -> None:
    img_dir = Path(mod.image_dir(mod.normalize_image(image)))
    rootfs = img_dir / "rootfs"
    rootfs.mkdir(parents=True, exist_ok=True)
    for did in diff_ids:
        tree = Path(mod.LAYERS_DIR) / did / "tree"
        tree.mkdir(parents=True, exist_ok=True)
    mod._save_image_manifest(str(img_dir), diff_ids, config or {"config": {"Env": []}})
    (img_dir / "config.json").write_text(json.dumps(config or {"config": {"Env": []}}))


def test_dockerfile_unknown_instruction_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("dockerfile_unknown", "no-proot", home_path)
        seed_legacy_image(mod, "ubuntu:22.04")
        ctx = home_path / "ctx"
        ctx.mkdir()
        dockerfile = ctx / "Dockerfile"
        dockerfile.write_text("FROM ubuntu:22.04\nPDOCKER_MAGIC true\n")
        output: list[str] = []
        result = mod.execute_dockerfile_build(
            str(dockerfile), str(ctx), "local/unknown:latest", {}, output.append
        )
        if result is not None:
            fail("unknown Dockerfile instruction should fail the build")
        if not any("unknown Dockerfile instruction" in line for line in output):
            fail(f"unknown instruction diagnostic missing: {output!r}")
        ok("Dockerfile parser rejects non-standard instructions")


def test_direct_run_requires_real_executor() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("dockerfile_run", "no-proot", home_path)
        seed_legacy_image(mod, "ubuntu:22.04")
        ctx = home_path / "ctx"
        ctx.mkdir()
        dockerfile = ctx / "Dockerfile"
        dockerfile.write_text("FROM ubuntu:22.04\nRUN echo should-not-be-faked\n")
        output: list[str] = []
        result = mod.execute_dockerfile_build(
            str(dockerfile), str(ctx), "local/run:latest", {}, output.append
        )
        if result is not None:
            fail("direct backend should not build fake RUN layers")
        joined = "\n".join(output)
        if "RUN requires a real container process executor" not in joined:
            fail(f"direct RUN diagnostic missing: {joined!r}")
        ok("direct Dockerfile RUN fails instead of recording fake layers")


def test_existing_tag_inline_run_cache() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("inline_cache", "no-proot", home_path)
        base = "a" * 64
        run = "b" * 64
        base_tree = Path(mod.LAYERS_DIR) / base / "tree"
        (base_tree / "bin").mkdir(parents=True)
        (base_tree / "bin" / "sh").write_text("#!/bin/sh\n")
        run_tree = Path(mod.LAYERS_DIR) / run / "tree"
        (run_tree / "cached").mkdir(parents=True)
        (run_tree / "cached" / "marker").write_text("reused\n")
        seed_layered_image(mod, "ubuntu:22.04", [base])
        cfg = {
            "config": {"Env": []},
            "rootfs": {"type": "layers", "diff_ids": [f"sha256:{base}", f"sha256:{run}"]},
            "history": [{"created_by": "RUN echo cached"}],
        }
        seed_layered_image(mod, "local/cached:latest", [base, run], cfg)
        ctx = home_path / "ctx"
        ctx.mkdir()
        dockerfile = ctx / "Dockerfile"
        dockerfile.write_text("FROM ubuntu:22.04\nRUN echo cached\n")
        output: list[str] = []
        result = mod.execute_dockerfile_build(
            str(dockerfile), str(ctx), "local/cached:latest", {}, output.append
        )
        if result != mod.normalize_image("local/cached:latest"):
            fail(f"inline cache rebuild failed: {result!r}, output={output!r}")
        joined = "\n".join(output)
        if "Using inline cache" not in joined:
            fail(f"existing tag inline cache was not used: {joined!r}")
        final_diff_ids = mod._load_image_manifest(Path(mod.image_dir(result)))
        if final_diff_ids != [base, run]:
            fail(f"inline cache rebuilt unexpected layer stack: {final_diff_ids!r}")
        ok("existing tagged image can seed Dockerfile RUN cache")


def test_existing_tag_full_image_cache() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("image_cache", "no-proot", home_path)
        base = "a" * 64
        run = "b" * 64
        copy = "c" * 64
        seed_layered_image(mod, "ubuntu:22.04", [base])
        dest = Path(mod.image_dir(mod.normalize_image("local/full-cache:latest")))
        rootfs = dest / "rootfs"
        (rootfs / "bin").mkdir(parents=True)
        (rootfs / "bin" / "sh").write_text("#!/bin/sh\n")
        (rootfs / "app").mkdir(parents=True)
        (rootfs / "app" / "hello.txt").write_text("hello\n")
        for did in (run, copy):
            (Path(mod.LAYERS_DIR) / did / "tree").mkdir(parents=True, exist_ok=True)
        cfg = {
            "config": {"Env": []},
            "rootfs": {"type": "layers", "diff_ids": [f"sha256:{base}", f"sha256:{run}", f"sha256:{copy}"]},
            "history": [
                {"created_by": "FROM ubuntu:22.04", "empty_layer": True},
                {"created_by": "RUN echo prepared"},
                {"created_by": "COPY hello.txt /app/hello.txt"},
            ],
        }
        mod._save_image_manifest(str(dest), [base, run, copy], cfg)
        (dest / "config.json").write_text(json.dumps(cfg))
        ctx = home_path / "ctx"
        ctx.mkdir()
        (ctx / "hello.txt").write_text("hello\n")
        dockerfile = ctx / "Dockerfile"
        dockerfile.write_text("FROM ubuntu:22.04\nRUN echo prepared\nCOPY hello.txt /app/hello.txt\n")
        output: list[str] = []
        result = mod.execute_dockerfile_build(
            str(dockerfile), str(ctx), "local/full-cache:latest", {}, output.append
        )
        if result != mod.normalize_image("local/full-cache:latest"):
            fail(f"full image cache rebuild failed: {result!r}, output={output!r}")
        if not any("Using image cache" in line for line in output):
            fail(f"full image cache was not used: {output!r}")
        ok("unchanged existing image skips full Dockerfile rebuild")


def test_build_cache_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        mod = load_pdockerd_with_env("build_cache", "no-proot", home_path)
        parent = ["a" * 64]
        state = {
            "env": {"A": "B"},
            "workdir": "/work",
            "shell": ["/bin/sh", "-c"],
            "user": "",
            "platform": "linux/arm64",
        }
        key1 = mod.build_cache_key("RUN", "echo hi", parent, state)
        key2 = mod.build_cache_key("RUN", "echo hi", parent, dict(reversed(state.items())))
        if key1 != key2:
            fail("build cache key should be stable for equivalent state")
        key3 = mod.build_cache_key("RUN", "echo hi", ["b" * 64], state)
        if key1 == key3:
            fail("build cache key must include parent layer stack")
        did = "c" * 64
        layer_tree = Path(mod.LAYERS_DIR) / did / "tree"
        layer_tree.mkdir(parents=True)
        (layer_tree / "marker").write_text("cached\n")
        mod.store_build_cache_entry(key1, {"diff_id": did, "size": 7})
        entry = mod.load_build_cache_entry(key1)
        if not entry or entry.get("diff_id") != did:
            fail("stored build cache entry was not reusable")
        shutil.rmtree(layer_tree.parent)
        if mod.load_build_cache_entry(key1) is not None:
            fail("build cache must ignore entries whose layer was pruned")
        ok("Dockerfile RUN cache keys and layer validation work")


def test_active_operations_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        mod = load_pdockerd_with_env("active_ops", "no-proot", Path(home))
        op_id = mod.start_operation("build", "build local/test:latest", "receiving context")
        mod.update_operation(op_id, "Step: FROM ubuntu:22.04")
        ops = mod.list_active_operations()
        if len(ops) != 1 or ops[0].get("Detail") != "Step: FROM ubuntu:22.04":
            fail(f"active operation not visible: {ops!r}")
        mod.finish_operation(op_id, "done", "Successfully tagged local/test:latest")
        if mod.list_active_operations():
            fail("finished operation should not remain active")
        ok("daemon active operations are listed independently of UI jobs")


def test_host_environment_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        direct = home_path / "pdocker-direct"
        direct.write_text("#!/bin/sh\nexit 0\n")
        direct.chmod(0o755)
        gpu = home_path / "pdocker-gpu-executor"
        gpu.write_text("#!/bin/sh\nexit 0\n")
        gpu.chmod(0o755)
        mod = load_pdockerd_with_env(
            "host_environment",
            "no-proot",
            home_path,
            {
                "PDOCKER_DIRECT_EXECUTOR": str(direct),
                "PDOCKER_GPU_EXECUTOR": str(gpu),
                "PDOCKER_GPU_EXECUTOR_AVAILABLE": "1",
                "PDOCKER_GPU_COMMAND_API": "pdocker-gpu-command-v1",
                "PDOCKER_VULKAN_ICD_KIND": "pdocker-bridge-minimal",
                "PDOCKER_VULKAN_ICD_READY": "0",
            },
        )
        env = mod.collect_host_environment("1.43")
        if env.get("Runtime", {}).get("DockerApiVersion") != "1.43":
            fail(f"host environment API version missing: {env!r}")
        if env.get("Gpu", {}).get("CommandApi") != "pdocker-gpu-command-v1":
            fail(f"host environment GPU command api missing: {env!r}")
        if env.get("Frameworks", {}).get("Vulkan", {}).get("ApiVersion") != "1.1.0":
            fail(f"host environment Vulkan API version missing: {env!r}")
        if "OpenCL" not in env.get("Frameworks", {}):
            fail(f"host environment OpenCL diagnostic missing: {env!r}")
        if "NnApi" not in env.get("Frameworks", {}):
            fail(f"host environment NNAPI diagnostic missing: {env!r}")
        if "OpenCVPython" in env.get("Frameworks", {}):
            fail(f"host environment should stay focused on GPU/NPU, not OpenCV: {env!r}")
        if not env.get("Paths", {}).get("DirectExecutor", {}).get("Exists"):
            fail(f"host environment direct executor path missing: {env!r}")
        if "PATH" in env.get("Environment", {}):
            fail(f"host environment must not dump broad process environment: {env!r}")
        ok("host environment contract exposes bounded runtime diagnostics")


def test_gpu_shim_contract() -> None:
    with tempfile.TemporaryDirectory(prefix="pdocker-test-") as home:
        home_path = Path(home)
        shim = home_path / "pdocker-gpu-shim"
        shim.write_text("#!/bin/sh\nexit 0\n")
        shim.chmod(0o755)
        vulkan_icd = home_path / "pdocker-vulkan-icd.so"
        vulkan_icd.write_text("icd")
        vulkan_icd.chmod(0o755)
        opencl_icd = home_path / "pdocker-opencl-icd.so"
        opencl_icd.write_text("opencl")
        opencl_icd.chmod(0o755)
        mod = load_pdockerd_with_env(
            "gpu_shim",
            "no-proot",
            home_path,
            {
                "PDOCKER_GPU_SHIM_HOST_PATH": str(shim),
                "PDOCKER_GPU_SHIM_CONTAINER_PATH": "/usr/local/bin/pdocker-gpu-shim",
                "PDOCKER_VULKAN_ICD_HOST_PATH": str(vulkan_icd),
                "PDOCKER_VULKAN_ICD_CONTAINER_PATH": "/usr/local/lib/pdocker-vulkan-icd.so",
                "PDOCKER_OPENCL_ICD_HOST_PATH": str(opencl_icd),
                "PDOCKER_OPENCL_ICD_CONTAINER_PATH": "/usr/local/lib/pdocker-opencl-icd.so",
                "PDOCKER_GPU_EXECUTOR": str(home_path / "pdocker-gpu-executor"),
                "PDOCKER_GPU_HOST_DIR": str(home_path),
                "PDOCKER_GPU_CONTAINER_DIR": "/run/pdocker-gpu",
                "PDOCKER_GPU_QUEUE_SOCKET": "/run/pdocker-gpu/pdocker-gpu.sock",
                "PDOCKER_GPU_SHARED_DIR": "/run/pdocker-gpu",
                "PDOCKER_GPU_COMMAND_API": "pdocker-gpu-command-v1",
                "PDOCKER_GPU_ABI_VERSION": "0.1",
            },
        )
        state = {
            "HostConfig": {
                "DeviceRequests": [
                    {
                        "Driver": "pdocker-gpu",
                        "Count": -1,
                        "Capabilities": [["gpu"]],
                        "Options": {"pdocker.opencl": "opencl"},
                    }
                ]
            }
        }
        env = mod._gpu_env(state)
        binds = mod._gpu_binds(state)
        if env.get("PDOCKER_GPU_SHIM") != "/usr/local/bin/pdocker-gpu-shim":
            fail(f"gpu shim env missing: {env!r}")
        if env.get("PDOCKER_GPU_COMMAND_API") != "pdocker-gpu-command-v1":
            fail(f"gpu command api missing: {env!r}")
        if env.get("PDOCKER_GPU_LLM_ENGINE_LOCATION") != "container":
            fail(f"gpu engine location must stay container: {env!r}")
        if env.get("PDOCKER_GPU_QUEUE_SOCKET") != "/run/pdocker-gpu/pdocker-gpu.sock":
            fail(f"gpu queue socket env missing: {env!r}")
        if env.get("PDOCKER_GPU_SHARED_DIR") != "/run/pdocker-gpu":
            fail(f"gpu shared dir env missing: {env!r}")
        if env.get("PDOCKER_VULKAN_ICD") != "/usr/local/lib/pdocker-vulkan-icd.so":
            fail(f"pdocker Vulkan ICD env missing: {env!r}")
        if env.get("PDOCKER_VULKAN_ICD_KIND") != "pdocker-bridge-minimal":
            fail(f"pdocker Vulkan ICD kind missing: {env!r}")
        if env.get("PDOCKER_VULKAN_ICD_READY") != "0":
            fail(f"pdocker Vulkan ICD must not claim compute readiness yet: {env!r}")
        if env.get("PDOCKER_OPENCL_ICD") != "/usr/local/lib/pdocker-opencl-icd.so":
            fail(f"pdocker OpenCL ICD env missing: {env!r}")
        if env.get("PDOCKER_OPENCL_ICD_KIND") != "pdocker-bridge-minimal":
            fail(f"pdocker OpenCL ICD kind missing: {env!r}")
        if env.get("PDOCKER_OPENCL_API_VERSION") != "1.2":
            fail(f"pdocker OpenCL API version missing: {env!r}")
        expected_bind = f"{shim}:/usr/local/bin/pdocker-gpu-shim:ro"
        if expected_bind not in binds:
            fail(f"gpu shim bind missing {expected_bind!r}: {binds!r}")
        expected_icd_bind = f"{vulkan_icd}:/usr/local/lib/pdocker-vulkan-icd.so:ro"
        if expected_icd_bind not in binds:
            fail(f"gpu Vulkan ICD bind missing {expected_icd_bind!r}: {binds!r}")
        expected_opencl_bind = f"{opencl_icd}:/usr/local/lib/pdocker-opencl-icd.so:ro"
        if expected_opencl_bind not in binds:
            fail(f"gpu OpenCL ICD bind missing {expected_opencl_bind!r}: {binds!r}")
        expected_opencl_lib_bind = f"{opencl_icd}:/usr/local/lib/libOpenCL.so.1:ro"
        if expected_opencl_lib_bind not in binds:
            fail(f"gpu OpenCL lib bind missing {expected_opencl_lib_bind!r}: {binds!r}")
        expected_gpu_dir_bind = f"{home_path}:/run/pdocker-gpu"
        if expected_gpu_dir_bind not in binds:
            fail(f"gpu runtime dir bind missing {expected_gpu_dir_bind!r}: {binds!r}")
        ok("GPU shim contract injects device-independent container ABI")


def main() -> int:
    test_direct_backend_contract()
    test_direct_executor_probe_contract()
    test_direct_executor_process_capability_contract()
    test_direct_backend_rejects_fake_container_start()
    test_default_no_proot_runtime_path()
    test_dockerfile_unknown_instruction_rejected()
    test_direct_run_requires_real_executor()
    test_existing_tag_inline_run_cache()
    test_existing_tag_full_image_cache()
    test_build_cache_contract()
    test_active_operations_contract()
    test_host_environment_contract()
    test_gpu_shim_contract()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
