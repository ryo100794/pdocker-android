FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    SHELL=/bin/bash \
    DISPLAY=:1 \
    VNC_DISPLAY=:1 \
    VNC_PORT=5901 \
    NOVNC_PORT=6080 \
    VNC_GEOMETRY=1280x800 \
    VNC_DEPTH=24 \
    PDOCKER_GL_BACKEND=llvmpipe \
    PDOCKER_GRAPHICS_MODE=software \
    PDOCKER_ZINK_EXPERIMENTAL=0 \
    LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    QT_X11_NO_MITSHM=1 \
    XDG_RUNTIME_DIR=/tmp/runtime-root

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash \
      blender \
      ca-certificates \
      curl \
      dbus-x11 \
      libgl1 \
      libglu1-mesa \
      matchbox-window-manager \
      mesa-utils \
      mesa-vulkan-drivers \
      procps \
      tigervnc-common \
      tigervnc-standalone-server \
      vulkan-tools \
      websockify \
      x11-xserver-utils \
      xterm \
    && tmp="$(mktemp -d)" \
    && (cd "$tmp" && apt-get download novnc && dpkg-deb -x novnc_*.deb /) \
    && rm -rf "$tmp" /var/lib/apt/lists/*

COPY scripts/start-blender-xvnc-novnc.sh /usr/local/bin/start-blender-xvnc-novnc
RUN chmod +x /usr/local/bin/start-blender-xvnc-novnc \
    && mkdir -p /workspace /workspace/logs /tmp/runtime-root /root/.vnc \
    && chmod 700 /tmp/runtime-root /root/.vnc

WORKDIR /workspace
EXPOSE 6080 5901
HEALTHCHECK --interval=15s --timeout=5s --start-period=30s --retries=8 CMD curl -fsS http://127.0.0.1:6080/vnc.html >/dev/null && pgrep -x Xvnc >/dev/null && pgrep -x blender >/dev/null

CMD ["/usr/local/bin/start-blender-xvnc-novnc"]
