FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    SHELL=/bin/bash \
    ROS_DISTRO=humble \
    DISPLAY=:1 \
    VNC_DISPLAY=:1 \
    VNC_PORT=5900 \
    NOVNC_PORT=6080 \
    VNC_GEOMETRY=1280x800 \
    VNC_DEPTH=24 \
    PDOCKER_GL_BACKEND=llvmpipe \
    LIBGL_ALWAYS_SOFTWARE=1 \
    GALLIUM_DRIVER=llvmpipe \
    QT_X11_NO_MITSHM=1 \
    XDG_RUNTIME_DIR=/tmp/runtime-root

RUN apt-get update && apt-get install -y --no-install-recommends \
      bash \
      ca-certificates \
      curl \
      dbus-x11 \
      gnupg \
      locales \
      lsb-release \
      mesa-utils \
      procps \
      software-properties-common \
      tigervnc-standalone-server \
      tigervnc-common \
      websockify \
      xfce4 \
      xfce4-terminal \
      x11-xserver-utils \
    && locale-gen en_US.UTF-8 \
    && add-apt-repository universe \
    && curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key \
      | gpg --dearmor -o /usr/share/keyrings/ros-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" \
      > /etc/apt/sources.list.d/ros2.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
      ros-humble-desktop \
      ros-humble-rviz2 \
    && tmp="$(mktemp -d)" \
    && (cd "$tmp" && apt-get download novnc && dpkg-deb -x novnc_*.deb /) \
    && rm -rf "$tmp" \
    && rm -rf /var/lib/apt/lists/*

COPY scripts/start-ros2-rviz-novnc.sh /usr/local/bin/start-ros2-rviz-novnc
RUN chmod +x /usr/local/bin/start-ros2-rviz-novnc \
    && mkdir -p /workspace /workspace/logs /tmp/runtime-root \
    && chmod 700 /tmp/runtime-root

WORKDIR /workspace
EXPOSE 6080 5900
HEALTHCHECK --interval=15s --timeout=5s --start-period=30s --retries=8 CMD curl -fsS http://127.0.0.1:6080/vnc.html >/dev/null && pgrep -x Xvnc >/dev/null && pgrep -x rviz2 >/dev/null

CMD ["/usr/local/bin/start-ros2-rviz-novnc"]
