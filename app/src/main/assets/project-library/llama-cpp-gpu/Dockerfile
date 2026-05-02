FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG LLAMA_CPP_REF=master

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
      vulkan-tools \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/ggml-org/llama.cpp /opt/llama.cpp \
    && cd /opt/llama.cpp \
    && if [ "$LLAMA_CPP_REF" != "master" ]; then git fetch --depth 1 origin "$LLAMA_CPP_REF" && git checkout "$LLAMA_CPP_REF"; fi \
    && cmake -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DGGML_NATIVE=OFF \
      -DGGML_VULKAN=ON \
      -DGGML_BLAS=ON \
      -DGGML_BLAS_VENDOR=OpenBLAS \
    && cmake --build build --config Release

COPY scripts/pdocker-gpu-profile.sh /usr/local/bin/pdocker-gpu-profile
COPY scripts/start-llama-server.sh /usr/local/bin/start-llama-server
RUN chmod +x /usr/local/bin/pdocker-gpu-profile /usr/local/bin/start-llama-server \
    && mkdir -p /models /workspace /profiles

WORKDIR /workspace
EXPOSE 18081

CMD ["/usr/local/bin/start-llama-server"]
