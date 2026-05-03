FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG LLAMA_CPP_REF=master
ARG LLAMA_CPP_BUILD_JOBS=2

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    SHELL=/bin/bash

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      git \
      glslc \
      glslang-tools \
      jq \
      libopenblas-dev \
      libvulkan-dev \
      libvulkan1 \
      ninja-build \
      pkg-config \
      procps \
      python3 \
      python3-pip \
      spirv-headers \
      spirv-tools \
      vulkan-tools \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/ggml-org/llama.cpp /opt/llama.cpp \
    && cd /opt/llama.cpp \
    && git config --global --add safe.directory /opt/llama.cpp \
    && if [ "$LLAMA_CPP_REF" != "master" ]; then git fetch --depth 1 origin "$LLAMA_CPP_REF" && git checkout "$LLAMA_CPP_REF"; fi \
    && cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DGGML_NATIVE=OFF \
      -DGGML_VULKAN=ON \
      -DGGML_BLAS=ON \
      -DGGML_BLAS_VENDOR=OpenBLAS \
    && jobs="${LLAMA_CPP_BUILD_JOBS:-2}" \
    && cmake --build build --config Release --target llama-server --parallel "$jobs"

COPY scripts/pdocker-gpu-profile.sh /usr/local/bin/pdocker-gpu-profile
COPY scripts/start-llama-server.sh /usr/local/bin/start-llama-server
RUN chmod +x /usr/local/bin/pdocker-gpu-profile /usr/local/bin/start-llama-server \
    && mkdir -p /models /workspace /workspace/logs /profiles /var/log/pdocker

WORKDIR /workspace
EXPOSE 18081
HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=6 CMD curl -fsS http://127.0.0.1:18081/health >/dev/null || curl -fsS http://127.0.0.1:18081/v1/models >/dev/null

CMD ["/usr/local/bin/start-llama-server"]
