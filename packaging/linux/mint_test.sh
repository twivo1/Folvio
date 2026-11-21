#!/usr/bin/env bash
# Installation test on Linux Mint containers (official linuxmintd images), artifacts mounted at /out.
#   mint_test.sh deb-engines | deb-plain | appimage | unsupported
# Runs as an ordinary user with sudo, the way the README tells people to install Folvio.
set -uo pipefail
MODE="$1"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
# The linuxmintd images are Mint's build images: make the system identify as Linux Mint (base-files,
# mint-common from the Mint repository) so that tests see what a Mint desktop sees.
apt-get install -y -qq --allow-downgrades base-files mint-common >/dev/null 2>&1 || true
apt-get install -y -qq sudo wget xvfb xauth file >/dev/null 2>&1
. /etc/os-release
echo "### $PRETTY_NAME | $(ldd --version | head -1) | mode=$MODE"
useradd -m mintuser 2>/dev/null && echo "mintuser ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/mintuser
DEB=$(ls /out/Folvio-*-Ubuntu-amd64.deb | head -1)
APPIMAGE=$(ls /out/Folvio-*-Linux-x86_64.AppImage | head -1)
run_user() { su mintuser -c "cd ~ && $1"; }

case "$MODE" in
deb-engines)
  # The README command (with a local file instead of the download, which only exists after publishing).
  run_user "sudo DEBIAN_FRONTEND=noninteractive apt install -y $DEB ffmpeg tesseract-ocr libreoffice-writer libreoffice-calc libreoffice-impress > /tmp/apt.log 2>&1; echo apt-exit=\$?"
  echo "installed: $(dpkg -s folvio | grep '^Version')"
  run_user "folvio --version"
  run_user "xvfb-run -a folvio --selftest --selftest-require ffmpeg,soffice,tesseract" | grep -E "System|Engine|PASS|FAIL|SKIP|RESULT"
  echo "selftest exit: ${PIPESTATUS[0]}"
  run_user "xvfb-run -a -s '-screen 0 1600x1000x24' folvio --page 6 --screenshot /tmp/mint.png --screenshot-delay 2500 >/dev/null 2>&1"
  echo "gui screenshot: $(stat -c %s /tmp/mint.png 2>/dev/null) bytes"; cp /tmp/mint.png /out/mint-${VERSION_ID}-deb.png 2>/dev/null
  echo "menu entry: $(grep -E '^(Name|Exec|Icon)=' /usr/share/applications/io.github.twivo1.Folvio.desktop | tr '\n' ' ')"
  echo "icon: $(ls /usr/share/icons/hicolor/256x256/apps/io.github.twivo1.Folvio.png)"
  run_user "mkdir -p ~/.local/share/Folvio && echo keep > ~/.local/share/Folvio/user-data.txt"
  run_user "sudo apt remove -y folvio > /dev/null 2>&1"
  echo "after remove: binary=$(ls /opt/folvio/bin/folvio 2>/dev/null || echo gone) menu=$(ls /usr/share/applications/io.github.twivo1.Folvio.desktop 2>/dev/null || echo gone) userdata=$(cat /home/mintuser/.local/share/Folvio/user-data.txt)"
  ;;
deb-plain)
  run_user "sudo DEBIAN_FRONTEND=noninteractive apt install -y --no-install-recommends $DEB > /tmp/apt.log 2>&1; echo apt-exit=\$?"
  run_user "xvfb-run -a folvio --selftest" | grep -E "PASS|FAIL|SKIP|RESULT"
  echo "selftest exit: ${PIPESTATUS[0]}"
  ;;
appimage)
  cp "$APPIMAGE" /home/mintuser/Folvio.AppImage && chown mintuser /home/mintuser/Folvio.AppImage
  run_user "chmod +x Folvio.AppImage && file Folvio.AppImage | cut -c1-80"
  echo "-- direct start (needs FUSE; containers have no /dev/fuse):"
  run_user "xvfb-run -a ./Folvio.AppImage --version 2>&1 | head -3"
  echo "-- without FUSE:"
  run_user "xvfb-run -a ./Folvio.AppImage --appimage-extract-and-run --selftest" | grep -E "System|PASS|FAIL|SKIP|RESULT"
  echo "selftest exit: ${PIPESTATUS[0]}"
  echo "libfuse2 installed: $(dpkg -l | grep -cE '^ii +libfuse2') ; fuse3: $(dpkg -l | grep -cE '^ii +fuse3')"
  ;;
unsupported)
  echo "-- .deb:"
  run_user "sudo DEBIAN_FRONTEND=noninteractive apt install -y $DEB 2>&1 | grep -E 'Depends|unmet|E:' | head -4"
  echo "-- AppImage:"
  cp "$APPIMAGE" /home/mintuser/Folvio.AppImage && chown mintuser /home/mintuser/Folvio.AppImage
  run_user "chmod +x Folvio.AppImage && ./Folvio.AppImage --appimage-extract-and-run --version 2>&1 | head -3"
  ;;
esac
