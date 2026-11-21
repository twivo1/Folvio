#!/usr/bin/env bash
# Smoke test of the release packages inside a clean distribution container (no Qt, no build tools).
# Usage (artifacts mounted at /out): clean_test.sh appimage|deb yes|no   (yes = install FFmpeg, LibreOffice, Tesseract)
set -uo pipefail
MODE="$1"; ENGINES="$2"
. /etc/os-release; echo "### $PRETTY_NAME - $MODE - engines=$ENGINES"
if command -v apt-get >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  # What a desktop installation already has (display server for the test: Xvfb).
  apt-get install -y -qq --no-install-recommends xvfb xauth libgl1 libegl1 libfontconfig1 libx11-xcb1 libdbus-1-3 \
      libglib2.0-0t64 libwayland-client0 fonts-dejavu-core ca-certificates >/dev/null 2>&1 \
   || apt-get install -y -qq --no-install-recommends xvfb xauth libgl1 libegl1 libfontconfig1 libx11-xcb1 libdbus-1-3 \
      libglib2.0-0 libwayland-client0 fonts-dejavu-core ca-certificates >/dev/null
  if [[ $MODE == deb ]]; then
    if [[ $ENGINES == yes ]]; then apt-get install -y -qq /out/Folvio-*-Ubuntu-amd64.deb >/dev/null
    else apt-get install -y -qq --no-install-recommends /out/Folvio-*-Ubuntu-amd64.deb >/dev/null; fi || exit 2
  elif [[ $ENGINES == yes ]]; then
    apt-get install -y -qq --no-install-recommends ffmpeg tesseract-ocr libreoffice-writer-nogui libreoffice-calc-nogui \
        libreoffice-impress-nogui >/dev/null
  fi
else
  dnf install -y -q xorg-x11-server-Xvfb xauth mesa-libGL mesa-libEGL fontconfig libX11-xcb dbus-libs glib2 \
      libwayland-client dejavu-sans-fonts which >/dev/null
  [[ $ENGINES == yes ]] && dnf install -y -q ffmpeg-free tesseract libreoffice-writer libreoffice-calc libreoffice-impress >/dev/null
fi
useradd -m tester 2>/dev/null
if [[ $MODE == deb ]]; then
  CMD="folvio"
  echo "installed: $(dpkg -s folvio | grep Version)"
else
  cp /out/Folvio-*-Linux-x86_64.AppImage /home/tester/Folvio.AppImage && chmod +x /home/tester/Folvio.AppImage
  CMD="/home/tester/Folvio.AppImage --appimage-extract-and-run"
fi
REQ=""; [[ $ENGINES == yes ]] && REQ="--selftest-require ffmpeg,soffice,tesseract"
su tester -c "cd ~ && xvfb-run -a $CMD --selftest $REQ" ; RC=$?
echo "selftest exit: $RC"
su tester -c "cd ~ && xvfb-run -a -s '-screen 0 1600x1000x24' $CMD --page 6 --screenshot /tmp/shot.png --screenshot-delay 2500 >/dev/null 2>&1"; echo "gui screenshot exit: $? size: $(stat -c %s /tmp/shot.png 2>/dev/null)"
cp /tmp/shot.png "/out/shot-${ID}${VERSION_ID}-${MODE}-${ENGINES}.png" 2>/dev/null
if [[ $MODE == deb ]]; then
  su tester -c "mkdir -p ~/.local/share/Folvio && echo keep > ~/.local/share/Folvio/user-data.txt"
  apt-get remove -y -qq folvio >/dev/null; echo "after remove: binary=$(ls /opt/folvio/bin/folvio 2>&1 | head -1) desktop=$(ls /usr/share/applications/io.github.twivo1.Folvio.desktop 2>&1 | head -1) userdata=$(cat /home/tester/.local/share/Folvio/user-data.txt)"
fi
exit $RC
