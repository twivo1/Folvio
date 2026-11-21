#!/usr/bin/env bash
# Downloads the prebuilt PDFium (BSD-3-Clause, bblanchon/pdfium-binaries) for Linux x86_64 into
# third_party/pdfium-linux. Same Chromium release as the Windows package in third_party/pdfium.
set -euo pipefail
TAG="chromium/8165"
SHA256="ba7865e16c0271daab75256790be06c26272190afe6e2a70eae0bb39bb99089c"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/pdfium-linux"
if [[ -f "$DEST/lib/libpdfium.so" ]]; then
  echo "PDFium already present in $DEST"; exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/pdfium.tgz" "https://github.com/bblanchon/pdfium-binaries/releases/download/${TAG/\//%2F}/pdfium-linux-x64.tgz"
echo "$SHA256  $TMP/pdfium.tgz" | sha256sum -c -
mkdir -p "$DEST"
tar -xzf "$TMP/pdfium.tgz" -C "$DEST"
echo "PDFium $TAG extracted to $DEST"
