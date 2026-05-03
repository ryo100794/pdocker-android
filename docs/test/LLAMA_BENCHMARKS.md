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

## Latest Result

- Date: 2026-05-03 UTC.
- Device path: `files/pdocker/bench/llama-bench-latest.json`.
- Mode: CPU fallback baseline.
- Model: Qwen3 8B GGUF, Q4_K_M, 8.19B parameters.
- Prompt tokens: 6.
- Generated tokens: 8.
- Wall time: 39.54s.
- Prompt speed: 1.11 tokens/s.
- Generation speed: 0.235 tokens/s.

GPU status for this run: CPU fallback. The container diagnostics reported no
Vulkan passthrough, no CUDA-compatible signal, no `VK_ICD_FILENAMES`, and no
usable GPU device inside the container. Future GPU-enabled runs should preserve
this same benchmark shape and record the GPU diagnostic evidence next to the
tokens/s result.
