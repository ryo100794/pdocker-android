# llama.cpp Runtime Benchmarks

This document records repeatable llama.cpp measurements from pdocker Android
runtime runs. Keep the latest machine-readable result in
`docs/test/llama-bench-latest.json` and copy it to the device bench directory
with `scripts/android-llama-bench.sh`.

## How To Run

1. Start the llama.cpp project from the app or Engine compose path.
2. Wait until `http://127.0.0.1:18081/` returns HTTP 200.
3. Run:

```sh
bash scripts/android-llama-bench.sh --predict 8 --repeat 1
```

Use the same prompt, token count, and model when comparing CPU fallback with
future Vulkan/CUDA-compatible runs.

## Latest HTTP API Result

- Date: 2026-05-03 UTC.
- Local path: `docs/test/llama-bench-cpu-repeat3.json`.
- Latest alias: `docs/test/llama-bench-latest.json`.
- Device path: `files/pdocker/bench/llama-bench-cpu-repeat3.json`.
- Mode: CPU fallback baseline.
- Model: Qwen3 8B GGUF, Q4_K_M, 8.19B parameters.
- Prompt tokens: 6.
- Generated tokens: 8.
- Repetitions: 3.
- Mean wall time: 37.32s.
- Generation speed: 0.260 tokens/s mean, 0.239 min, 0.286 max.

Per-run generation speeds:

- Run 1: 0.255 tokens/s, 39.26s wall.
- Run 2: 0.239 tokens/s, 38.58s wall.
- Run 3: 0.286 tokens/s, 34.11s wall.

## Latest llama-bench Tool Result

The official llama.cpp `llama-bench` tool was built inside the existing
container with:

```sh
cmake --build /opt/llama.cpp/build --target llama-bench --parallel 2
```

The repeatable wrapper is:

```sh
bash scripts/android-llama-tool-bench.sh
```

Latest local path: `docs/test/llama-bench-tool-cpu-p16-n8-r3.json`.

Parameters:

- `-m /models/model.gguf`
- `-p 16`
- `-n 8`
- `-r 3`
- `-ngl 0`
- `-t 8`

Results:

- Prompt processing: 2.40 tokens/s average, samples 2.11, 2.58, 2.51.
- Token generation: 0.228 tokens/s average, samples 0.210, 0.260, 0.215.
- Backend reported by llama-bench: BLAS/OpenBLAS, CPU.

GPU status for this run: CPU fallback. The container diagnostics reported no
Vulkan passthrough, no CUDA-compatible signal, no `VK_ICD_FILENAMES`, and no
usable GPU device inside the container. Future GPU-enabled runs should preserve
this same benchmark shape and record the GPU diagnostic evidence next to the
tokens/s result.
