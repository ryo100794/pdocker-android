# pdocker default dev workspace

This project builds the default development container for pdocker.

It includes:

- code-server for browser-based VS Code sessions.
- Continue VS Code extension.
- OpenAI Codex CLI through `npm install -g @openai/codex`.
- OpenAI Codex VS Code extension (`OpenAI.chatgpt`; the Marketplace ID keeps
  the historical `chatgpt` suffix).
- Claude Code CLI through `npm install -g @anthropic-ai/claude-code`.
- Claude Code VS Code extension (`Anthropic.claude-code`).
- Common development tools: git, Python, Node/npm, ripgrep, jq, vim, nano.
- Common editor extensions for Python, YAML, Docker, formatting, linting, and Git.

Run from the pdocker UI:

1. Open `Dockerfile` tab.
2. Build the default project.
3. Open `Compose` tab and run the default compose project.
4. Open logs or shell from the `Containers` tab.

The app's current runtime uses host-style networking, so the code-server process
binds to `0.0.0.0:18080` inside the container by default. The compose header
comment `# pdocker.service-url: 18080=VS Code` labels the local browser
shortcut without changing standard Compose behavior. `# pdocker.auto-open: VS Code`
marks that declared service as the one pdocker may open automatically after
compose up.

If the Codex side panel is not obvious in code-server, open the command
palette and run `Tasks: Run Task`, then choose `Codex: start`. That task starts
the Codex CLI in a VS Code terminal inside `/workspace`.
