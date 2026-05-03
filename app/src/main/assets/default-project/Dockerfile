FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG CODE_SERVER_VERSION=latest
ARG CODEX_NPM_PACKAGE=@openai/codex
ARG NODE_MAJOR=22

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    SHELL=/bin/bash \
    CODE_SERVER_USER_DATA_DIR=/workspace/.vscode-server/data \
    CODE_SERVER_EXTENSIONS_DIR=/workspace/.vscode-server/extensions

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash \
      ca-certificates \
      curl \
      dumb-init \
      git \
      gnupg \
      jq \
      openssh-client \
      procps \
      python3 \
      ripgrep \
      sudo \
      tar \
      unzip \
      libvulkan1 \
      wget \
      xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://deb.nodesource.com/setup_${NODE_MAJOR}.x" | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && node --version \
    && npm --version \
    && rm -rf /var/lib/apt/lists/*

RUN if [ "$CODE_SERVER_VERSION" = "latest" ]; then \
      curl -fsSL https://code-server.dev/install.sh | sh; \
    else \
      curl -fsSL https://code-server.dev/install.sh | sh -s -- --version "$CODE_SERVER_VERSION"; \
    fi

RUN npm install -g npm@latest \
    && npm install -g "$CODEX_NPM_PACKAGE"

RUN mkdir -p /workspace "$CODE_SERVER_USER_DATA_DIR/User" "$CODE_SERVER_EXTENSIONS_DIR" \
    && code-server --extensions-dir "$CODE_SERVER_EXTENSIONS_DIR" --install-extension Continue.continue || true \
    && code-server --extensions-dir "$CODE_SERVER_EXTENSIONS_DIR" --install-extension redhat.vscode-yaml || true \
    && code-server --extensions-dir "$CODE_SERVER_EXTENSIONS_DIR" --install-extension ms-azuretools.vscode-docker || true

COPY scripts/start-code-server.sh /usr/local/bin/start-code-server
COPY vscode/settings.json /workspace/.vscode-server/data/User/settings.json
COPY continue/config.yaml /workspace/.continue/config.yaml
RUN chmod +x /usr/local/bin/start-code-server

WORKDIR /workspace
EXPOSE 18080

ENTRYPOINT ["/usr/bin/dumb-init", "--"]
CMD ["/usr/local/bin/start-code-server"]
