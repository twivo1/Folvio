#!/usr/bin/env bash
# Builds, tests and packages Folvio for Linux x86_64 (AppImage + .deb).
# Runs inside the image from packaging/linux/Dockerfile (Ubuntu 24.04, Qt from aqtinstall).
#
#   packaging/linux/build_linux.sh [--skip-tests]
#
# Outputs in dist/linux/: Folvio-<ver>-Linux-x86_64.AppImage, Folvio-<ver>-Ubuntu-amd64.deb
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
SKIP_TESTS=0
[[ "${1:-}" == "--skip-tests" ]] && SKIP_TESTS=1
: "${QT_ROOT:?QT_ROOT must point to the Qt installation (gcc_64)}"
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' CMakeLists.txt | head -1)"
BUILD="$ROOT/build/linux-release"
DIST="$ROOT/dist/linux"
APPDIR="$BUILD/AppDir"
TOOLS="$ROOT/build/linux-tools"
export LANG=C.UTF-8 APPIMAGE_EXTRACT_AND_RUN=1

step() { echo; echo "== $*"; }

# --- pinned packaging tools (SHA-256 verified) ----------------------------------------
fetch() { # url sha256 dest
  if [[ ! -f "$3" ]] || ! echo "$2  $3" | sha256sum -c - >/dev/null 2>&1; then
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
  fi
  chmod +x "$3"
}
mkdir -p "$TOOLS" "$DIST"
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
      c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d "$TOOLS/linuxdeploy-x86_64.AppImage"
fetch https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage \
      15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724 "$TOOLS/linuxdeploy-plugin-qt-x86_64.AppImage"
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
      ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 "$TOOLS/appimagetool-x86_64.AppImage"
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
      2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d "$TOOLS/runtime-x86_64"

# --- dependencies -----------------------------------------------------------------------
step "Dependencies"
tools/fetch_pdfium.sh
tools/build_opencv.sh > "$BUILD.opencv.log" 2>&1 || { tail -40 "$BUILD.opencv.log"; exit 1; }

# --- build --------------------------------------------------------------------------------
step "Build $VERSION"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT" \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DFOLVIO_BUILD_TOOLS=OFF
cmake --build "$BUILD" --parallel

# --- tests (as an unprivileged user, real X11 server) ------------------------------------
if [[ $SKIP_TESTS == 0 ]]; then
  step "Tests"
  TEST_USER="${FOLVIO_TEST_USER:-ubuntu}"
  chown -R "$TEST_USER" "$BUILD/tests"
  su "$TEST_USER" -c "cd '$BUILD/tests' && HOME=\$(mktemp -d) QT_QPA_PLATFORM=xcb LANG=C.UTF-8 \
      xvfb-run -a -s '-screen 0 1920x1080x24' ./folvio_tests --gtest_brief=1" | tee "$DIST/linux-tests.log"
  grep -q "^\[  PASSED  \]" "$DIST/linux-tests.log"
  ! grep -q "^\[  FAILED  \]" "$DIST/linux-tests.log"
fi

# --- AppDir -------------------------------------------------------------------------------
step "AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"
# Fonts for deterministic PDF editing on every distribution (OFL-1.1 / Apache-2.0 / Bitstream Vera).
FONTS="$APPDIR/usr/share/folvio/fonts"
FLIC="$APPDIR/usr/share/doc/folvio/licenses/fonts"
mkdir -p "$FONTS" "$FLIC"
cp /usr/share/fonts/truetype/liberation/Liberation{Sans,Serif,Mono}-*.ttf "$FONTS/"
cp /usr/share/fonts/truetype/crosextra/Carlito-*.ttf /usr/share/fonts/truetype/crosextra/Caladea-*.ttf "$FONTS/"
cp /usr/share/fonts/truetype/dejavu/DejaVu{Sans,Serif,SansMono}*.ttf "$FONTS/"
cp /usr/share/doc/fonts-liberation/copyright "$FLIC/liberation-copyright"
cp /usr/share/doc/fonts-crosextra-carlito/copyright "$FLIC/carlito-copyright"
cp /usr/share/doc/fonts-crosextra-caladea/copyright "$FLIC/caladea-copyright"
cp /usr/share/doc/fonts-dejavu-core/copyright "$FLIC/dejavu-copyright"
cp "$QT_ROOT/../../Licenses/LICENSE" "$APPDIR/usr/share/doc/folvio/licenses/qt-LICENSE" 2>/dev/null || true
desktop-file-validate "$APPDIR/usr/share/applications/io.github.twivo1.Folvio.desktop"

# Plugins Folvio does not use and whose client libraries are absent (database drivers, GTK3 theme).
# They are removed from the Qt copy inside the disposable container only.
if [[ "${FOLVIO_CONTAINER:-}" != 1 ]]; then
  echo "Packaging must run in the build container (packaging/linux/Dockerfile)." >&2; exit 1
fi
( cd "$QT_ROOT/plugins" && rm -f sqldrivers/libqsqlibase.so sqldrivers/libqsqlmimer.so sqldrivers/libqsqlmysql.so     sqldrivers/libqsqloci.so sqldrivers/libqsqlodbc.so sqldrivers/libqsqlpsql.so platformthemes/libqgtk3.so )
export QMAKE="$QT_ROOT/bin/qmake" LD_LIBRARY_PATH="$QT_ROOT/lib:$APPDIR/usr/lib/folvio"
# Wayland and X11 platform plugins; only the SQLite SQL driver (others need client libraries).
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
export EXTRA_QT_PLUGINS="wayland-shell-integration;wayland-decoration-client;wayland-graphics-integration-client;svg"
[[ -f "$QT_ROOT/plugins/platforms/libqwayland.so" ]] || export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
# Base system libraries every desktop has are taken from the host, not bundled. This also keeps
# copyleft libraries out of the image (libjbig is GPL-2.0+, glib/systemd/gcrypt/dbus are LGPL),
# so the AppImage only redistributes permissively licensed code plus Qt (LGPL, replaceable .so files).
EXCLUDES=()
for lib in 'libtiff.so*' 'libjbig.so*' 'libLerc.so*' 'libdeflate.so*' 'libwebp.so*' 'libsharpyuv.so*' 'libjpeg.so*' \
           'libglib-2.0.so*' 'libgthread-2.0.so*' 'libdbus-1.so*' 'libsystemd.so*' 'libgcrypt.so*' 'libgpg-error.so*' \
           'libcap.so*' 'libpcre2-8.so*' 'liblzma.so*' 'libzstd.so*' 'liblz4.so*' 'libbsd.so*' 'libmd.so*' 'libbz2.so*' \
           'libffi.so*' 'libbrotli*.so*'; do
  EXCLUDES+=(--exclude-library "$lib")
done
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/folvio" \
    --library "$APPDIR/usr/lib/folvio/libpdfium.so" \
    --desktop-file "$APPDIR/usr/share/applications/io.github.twivo1.Folvio.desktop" \
    --icon-file "$ROOT/assets/branding/png/folvio-icon-256.png" --icon-filename io.github.twivo1.Folvio \
    "${EXCLUDES[@]}" \
    --plugin qt > "$BUILD.linuxdeploy.log" 2>&1 || { tail -60 "$BUILD.linuxdeploy.log"; exit 1; }
# Wayland needs the shell/graphics/decoration integration plugins, which linuxdeploy-plugin-qt does not
# deploy ("Loading shell integration failed" without them). Copy them and deploy their dependencies.
WAYLAND_PLUGINS=()
for kind in wayland-shell-integration wayland-graphics-integration-client wayland-decoration-client; do
  mkdir -p "$APPDIR/usr/plugins/$kind"
  cp "$QT_ROOT/plugins/$kind/"*.so "$APPDIR/usr/plugins/$kind/"
  for f in "$APPDIR/usr/plugins/$kind/"*.so; do WAYLAND_PLUGINS+=(--deploy-deps-only "$f"); done
done
"$TOOLS/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" "${EXCLUDES[@]}" "${WAYLAND_PLUGINS[@]}" \
    >> "$BUILD.linuxdeploy.log" 2>&1 || { tail -40 "$BUILD.linuxdeploy.log"; exit 1; }
test -f "$APPDIR/usr/plugins/wayland-shell-integration/libxdg-shell.so"

# linuxdeploy-plugin-qt deploys plugin dependencies itself and ignores --exclude-library, so remove the
# host-provided libraries explicitly (the dynamic loader then uses the system copies).
for pattern in "${EXCLUDES[@]}"; do
  [[ "$pattern" == --exclude-library ]] && continue
  find "$APPDIR/usr/lib" -maxdepth 1 -name "$pattern" -print -delete
done
# SQL drivers other than SQLite pull in database client libraries; Folvio only uses SQLite.
find "$APPDIR/usr/plugins/sqldrivers" -type f ! -name 'libqsqlite.so' -delete 2>/dev/null || true
test -f "$APPDIR/usr/plugins/sqldrivers/libqsqlite.so"
test -f "$APPDIR/usr/plugins/platforms/libqxcb.so"
if ls "$APPDIR/usr/lib" | grep -E '^(libjbig|libglib-2|libsystemd|libgcrypt|libdbus-1|libtiff)\.'; then
  echo "A library that must come from the host system was bundled." >&2; exit 1
fi

# --- AppImage ------------------------------------------------------------------------------
step "AppImage"
APPIMAGE="$DIST/Folvio-$VERSION-Linux-x86_64.AppImage"
rm -f "$APPIMAGE"
ARCH=x86_64 VERSION="$VERSION" "$TOOLS/appimagetool-x86_64.AppImage" --runtime-file "$TOOLS/runtime-x86_64" \
    --no-appstream "$APPDIR" "$APPIMAGE" > "$BUILD.appimagetool.log" 2>&1 || { tail -40 "$BUILD.appimagetool.log"; exit 1; }
chmod +x "$APPIMAGE"

# --- .deb (same AppDir under /opt/folvio, launcher and desktop integration in /usr) ---------
step "Debian package"
DEBROOT="$BUILD/deb"
rm -rf "$DEBROOT"
mkdir -p "$DEBROOT/opt/folvio" "$DEBROOT/usr/bin" "$DEBROOT/DEBIAN"
cp -a "$APPDIR/usr/." "$DEBROOT/opt/folvio/"
ln -s /opt/folvio/bin/folvio "$DEBROOT/usr/bin/folvio"
mkdir -p "$DEBROOT/usr/share"
mv "$DEBROOT/opt/folvio/share/applications" "$DEBROOT/opt/folvio/share/icons" "$DEBROOT/opt/folvio/share/metainfo" "$DEBROOT/usr/share/"
mkdir -p "$DEBROOT/usr/share/doc/folvio"
cp "$ROOT/packaging/linux/debian-copyright" "$DEBROOT/usr/share/doc/folvio/copyright"
# Runtime dependencies of everything we ship, resolved to Ubuntu package names.
mkdir -p "$BUILD/debian-shlibs/debian" && cd "$BUILD/debian-shlibs"
printf 'Source: folvio\n\nPackage: folvio\nArchitecture: amd64\n' > debian/control
mapfile -t ELF < <(find "$DEBROOT/opt/folvio" -type f \( -name '*.so*' -o -path '*/bin/*' \) -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
DEPS="$(dpkg-shlibdeps -O -l"$DEBROOT/opt/folvio/lib" -l"$DEBROOT/opt/folvio/lib/folvio" --ignore-missing-info "${ELF[@]}" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')"
cd "$ROOT"
SIZE_KB="$(du -sk "$DEBROOT" | cut -f1)"
cat > "$DEBROOT/DEBIAN/control" <<CONTROL
Package: folvio
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Maintainer: Bartosz Skrzypiec <307302330+twivo1@users.noreply.github.com>
Installed-Size: $SIZE_KB
Depends: $DEPS
Recommends: ffmpeg, tesseract-ocr, libreoffice-writer, libreoffice-calc, libreoffice-impress, fonts-liberation (>= 1:2), fonts-crosextra-carlito
Homepage: https://github.com/twivo1/Folvio
Description: Convert, edit and combine files in one desktop workspace
 Folvio converts documents, images, audio and video, edits the text of existing
 PDFs in place, adds a searchable OCR layer to scans and combines photos of paper
 pages into one straightened PDF. Everything runs locally.
 .
 FFmpeg, LibreOffice and Tesseract (recommended packages) provide media, office
 and OCR conversions.
CONTROL
DEB="$DIST/Folvio-$VERSION-Ubuntu-amd64.deb"
rm -f "$DEB"
fakeroot dpkg-deb --build -Zxz "$DEBROOT" "$DEB" >/dev/null
dpkg-deb --info "$DEB" | sed -n '/Package:/,/Homepage:/p'

step "Artifacts"
( cd "$DIST" && sha256sum Folvio-*.AppImage Folvio-*.deb | tee SHA256SUMS-linux.txt )
ls -la "$DIST"
