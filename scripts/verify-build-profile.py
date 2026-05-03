#!/usr/bin/env python3
"""Lightweight regression for pdockerd build profiling and COPY snapshot cost."""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import tarfile
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PDOCKERD = ROOT / "docker-proot-setup" / "bin" / "pdockerd"


def load_pdockerd(tmp: Path):
    os.environ["PDOCKER_HOME"] = str(tmp / "home")
    os.environ["PDOCKER_TMP_DIR"] = str(tmp / "tmp")
    os.environ["PDOCKER_RUNTIME_BACKEND"] = "direct"
    loader = importlib.machinery.SourceFileLoader("pdockerd_build_profile", str(PDOCKERD))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    for dirname in (
        mod.IMAGES_DIR,
        mod.LAYERS_DIR,
        mod.CONTAINERS_DIR,
        mod.META_DIR,
        mod.LOGS_DIR,
        mod.VOLUMES_DIR,
        mod.NETWORKS_DIR,
        mod.PDOCKER_TMP,
    ):
        Path(dirname).mkdir(parents=True, exist_ok=True)
    return mod


def seed_base_image(mod, tmp: Path) -> None:
    stage = tmp / "base-stage"
    for idx in range(300):
        path = stage / "usr" / "share" / "pdocker-test" / f"f{idx:04d}.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("x" * 128)
    tar_path = tmp / "base.tar"
    with tarfile.open(tar_path, "w") as tf:
        tf.add(stage, arcname=".")
    diff_id = mod._sha256_file(str(tar_path))
    mod._extract_layer_tar(str(tar_path), diff_id)

    image_dir = Path(mod.image_dir(mod.normalize_image("local/base:latest")))
    (image_dir / "rootfs").mkdir(parents=True, exist_ok=True)
    config = {
        "config": {"Env": [], "Cmd": None},
        "rootfs": {"type": "layers", "diff_ids": [f"sha256:{diff_id}"]},
    }
    mod._save_image_manifest(str(image_dir), [diff_id], config)
    (image_dir / "config.json").write_text(json.dumps(config))


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pdocker-build-profile-") as td:
        tmp = Path(td)
        mod = load_pdockerd(tmp)
        seed_base_image(mod, tmp)
        ctx = tmp / "context"
        ctx.mkdir()
        (ctx / "Dockerfile").write_text(
            "FROM local/base:latest\n"
            "COPY a.txt /opt/a.txt\n"
            "COPY b.txt /opt/b.txt\n"
        )
        (ctx / "a.txt").write_text("a")
        (ctx / "b.txt").write_text("b")
        logs: list[str] = []
        result = mod.execute_dockerfile_build(
            str(ctx / "Dockerfile"),
            str(ctx),
            "local/out:latest",
            {},
            logs.append,
        )
        text = "\n".join(logs)
        if not result:
            raise SystemExit("build did not produce an image")
        if "build-profile snapshot COPY:" not in text:
            raise SystemExit("missing build-profile snapshot output")
        if "mode=paths" not in text:
            raise SystemExit("COPY did not use path-scoped snapshot mode")
        if "prev-index=0.000s" not in text:
            raise SystemExit("COPY snapshot still builds a full previous-layer index")
    print("verify-build-profile: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
