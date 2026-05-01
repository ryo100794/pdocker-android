# Default development workspace

Snapshot date: 2026-05-01.

pdocker ships a default project template under
`app/src/main/assets/default-project/`. On first app launch it is copied to:

```text
filesDir/pdocker/projects/default/
```

Existing user-edited files are not overwritten.

## Included container stack

The default Dockerfile builds `pdocker/dev-workspace:latest` from Ubuntu 22.04
and installs:

- code-server, used as a VS Code-compatible browser IDE server.
- Continue VS Code extension (`Continue.continue`).
- OpenAI Codex CLI (`npm install -g @openai/codex`).
- Common development tools: git, Python, Node/npm, ripgrep, jq, SSH client,
  vim, nano, Vulkan tools, and shell utilities.
- Common IDE extensions for Python, YAML, Docker, formatting, linting, Jupyter,
  and Git inspection. Extension install failures are non-fatal because
  Open VSX / marketplace availability can vary by architecture and date.

## Files

```text
default-project/
├── Dockerfile
├── compose.yaml
├── README.md
├── continue/config.yaml
├── scripts/start-code-server.sh
└── vscode/settings.json
```

## Runtime notes

- `compose.yaml` starts code-server on `0.0.0.0:8080`.
- `compose.yaml` requests `gpus: all`, which maps to pdocker's experimental
  Vulkan passthrough / CUDA-compatible API negotiation.
- `OPENAI_API_KEY`, `GITHUB_TOKEN`, and `CODE_SERVER_PASSWORD` are passed
  through from the environment when provided.
- If `CODE_SERVER_PASSWORD` is empty, code-server starts with `--auth none` for
  local-only development convenience. Set `CODE_SERVER_PASSWORD` before exposing
  the service outside the device.
- pdocker's current networking model is host-style, so the UI still needs a
  service widget/webview affordance to expose the code-server URL cleanly.

## Source references

- Codex CLI install path: `npm install -g @openai/codex`.
- code-server supports command-line extension installation with
  `code-server --install-extension <extension id>`.
- Continue extension marketplace ID: `Continue.continue`.
