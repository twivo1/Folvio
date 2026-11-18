# Build environment for Folvio on Linux (Ubuntu 24.04 LTS = oldest supported glibc, 2.39).
# Qt 6.10.0 comes from the official Qt online repository through aqtinstall (pinned).
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
ARG QT_VERSION=6.10.0
ARG AQT_VERSION=3.3.0
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++ cmake ninja-build git curl ca-certificates file patchelf desktop-file-utils \
      python3 python3-venv python3-pypdf python3-pil python3-numpy \
      libgl-dev libegl-dev libglx-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev libfreetype-dev \
      libdbus-1-3 libxcb1 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
      libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxcb-xfixes0 libx11-xcb1 libxrender1 \
      libxi6 libsm6 libice6 libwayland-client0 libwayland-cursor0 libwayland-egl1 libglib2.0-0t64 \
      xvfb xauth dbus-x11 \
      fonts-liberation fonts-crosextra-carlito fonts-crosextra-caladea fonts-dejavu-core \
      ffmpeg tesseract-ocr libreoffice-writer-nogui libreoffice-calc-nogui libreoffice-impress-nogui \
      dpkg-dev fakeroot \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/aqt && /opt/aqt/bin/pip install --no-cache-dir "aqtinstall==${AQT_VERSION}" \
    && /opt/aqt/bin/aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -m qtimageformats -O /opt/Qt \
    && rm -rf /root/.cache
ENV QT_ROOT=/opt/Qt/${QT_VERSION}/gcc_64
ENV PATH=/opt/Qt/${QT_VERSION}/gcc_64/bin:$PATH
WORKDIR /src
