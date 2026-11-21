#!/usr/bin/env bash
# Downloads ONNX Runtime (MIT, Microsoft) from the official NuGet package Microsoft.ML.OnnxRuntime
# (pinned version and SHA-256) and extracts the native runtime for this platform into
# third_party/onnxruntime/lib/<platform>. Folvio loads it at run time (C API) for handwriting
# recognition; the app works without it.
set -euo pipefail
VERSION="1.31.1"
SHA256="6871ccbc97f3d5bdb4095a1dcec33aa78d56f6ed8de5980ef5fa1d9f2e7c679a"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/onnxruntime/lib/linux-x64"
if [[ -f "$DEST/libonnxruntime.so" ]]; then
  echo "ONNX Runtime already present in $DEST"; exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/ort.nupkg" "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${VERSION}/microsoft.ml.onnxruntime.${VERSION}.nupkg"
echo "$SHA256  $TMP/ort.nupkg" | sha256sum -c -
mkdir -p "$DEST"
python3 - "$TMP/ort.nupkg" "$DEST" <<'PY'
import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1])
for name in ("runtimes/linux-x64/native/libonnxruntime.so",):
    with z.open(name) as src, open(os.path.join(sys.argv[2], os.path.basename(name)), "wb") as dst:
        dst.write(src.read())
PY
echo "ONNX Runtime $VERSION extracted to $DEST"
