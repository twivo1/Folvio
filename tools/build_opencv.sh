#!/usr/bin/env bash
# Builds a minimal static OpenCV (core + imgproc, Apache-2.0) into third_party/opencv.
# Same version and options as tools/build_opencv.ps1 (Windows).
set -euo pipefail
VERSION="4.10.0"
SHA256="b2171af5be6b26f7a06b1229948bbb2bdaa74fcf5cd097e0af6378fce50a6eb9"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRCROOT="$ROOT/third_party/src"
SRC="$SRCROOT/opencv-$VERSION"
BUILD="$ROOT/build/opencv-$VERSION-linux"
INSTALL="$ROOT/third_party/opencv"
if [[ -f "$INSTALL/lib/cmake/opencv4/OpenCVConfig.cmake" ]]; then
  echo "OpenCV already installed in $INSTALL"; exit 0
fi
if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
  mkdir -p "$SRCROOT"
  curl -fsSL -o "$SRCROOT/opencv-$VERSION.tar.gz" "https://github.com/opencv/opencv/archive/refs/tags/$VERSION.tar.gz"
  echo "$SHA256  $SRCROOT/opencv-$VERSION.tar.gz" | sha256sum -c -
  tar -xzf "$SRCROOT/opencv-$VERSION.tar.gz" -C "$SRCROOT"
fi
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$INSTALL" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_LIST=core,imgproc -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF \
  -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF -DBUILD_opencv_apps=OFF -DBUILD_opencv_python3=OFF \
  -DBUILD_JAVA=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_EIGEN=OFF \
  -DWITH_LAPACK=OFF -DWITH_OPENMP=OFF -DWITH_PROTOBUF=OFF -DWITH_ADE=OFF -DWITH_QT=OFF \
  -DWITH_GTK=OFF -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF -DWITH_V4L=OFF -DWITH_1394=OFF \
  -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF -DWITH_OPENEXR=OFF \
  -DWITH_JASPER=OFF -DWITH_OPENJPEG=OFF -DWITH_IMGCODEC_HDR=OFF -DWITH_VTK=OFF -DWITH_TBB=OFF \
  -DWITH_VA=OFF -DWITH_VA_INTEL=OFF -DOPENCV_ENABLE_NONFREE=OFF -DBUILD_ZLIB=ON -DCV_TRACE=OFF \
  -DOPENCV_GENERATE_PKGCONFIG=OFF -DENABLE_PRECOMPILED_HEADERS=OFF
cmake --build "$BUILD" --parallel
cmake --install "$BUILD"
echo "OpenCV $VERSION installed to $INSTALL"
