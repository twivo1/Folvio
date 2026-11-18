# Build environment for Folvio on Linux (Ubuntu 24.04 LTS = oldest supported glibc, 2.39).
# Qt 6.10.0 comes from the official Qt online repository through aqtinstall (pinned).
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
ARG QT_VERSION=6.10.0
ARG AQT_VERSION=3.3.0
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++ cmake ninja-build git curl ca-certificates file patchelf desktop-file-utils xz-utils \
      python3 python3-venv python3-pypdf python3-pil python3-numpy \
      libgl-dev libegl-dev libglx-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1-dev libfreetype-dev libtiff-dev \
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

# The prebuilt TIFF image plugin links libtiff.so.5 (absent on Ubuntu 24.04 and newer). Rebuild only that
# plugin from the same Qt release against the distribution's maintained libtiff6.
ARG QTIF_SHA256=64450a52507c540de53616ed5e516df0e0905a99d3035ddfaa690f2b3f7c0cea
RUN cd /tmp \
    && curl -fsSL -o qtif.tar.xz "https://download.qt.io/official_releases/qt/6.10/${QT_VERSION}/submodules/qtimageformats-everywhere-src-${QT_VERSION}.tar.xz" \
    && echo "${QTIF_SHA256}  qtif.tar.xz" | sha256sum -c - \
    && tar -xf qtif.tar.xz \
    && cmake -S "qtimageformats-everywhere-src-${QT_VERSION}" -B qtif-build -G Ninja -DCMAKE_PREFIX_PATH="${QT_ROOT}" \
         -DFEATURE_system_tiff=ON -DCMAKE_BUILD_TYPE=Release > /dev/null \
    && cmake --build qtif-build --target QTiffPlugin \
    && cp "$(find qtif-build -name libqtiff.so | head -1)" "${QT_ROOT}/plugins/imageformats/libqtiff.so" \
    && ldd "${QT_ROOT}/plugins/imageformats/libqtiff.so" | grep -q 'libtiff.so.6' \
    && rm -rf /tmp/qtif*

# Marks the disposable build container (packaging may prune unused Qt plugins here, never on a host).
ENV FOLVIO_CONTAINER=1
WORKDIR /src
