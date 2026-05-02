# pdocker default dev workspace

This project builds the default development container for pdocker.

It includes:

- code-server for browser-based VS Code sessions.
- Continue VS Code extension.
- OpenAI Codex CLI through `npm install -g @openai/codex`.
- Common development tools: git, Python, Node/npm, ripgrep, jq, vim, nano.
- Common editor extensions for Python, YAML, Docker, formatting, linting, and Git.

Run from the pdocker UI:

1. Open `Dockerfile` tab.
2. Build the default project.
3. Open `Compose` tab and run the default compose project.
4. Open logs or shell from the `Containers` tab.

The app's current runtime uses host-style networking, so the code-server process
binds to `0.0.0.0:18080` inside the container by default. Container cards expose
the local browser URL when this service is present.
