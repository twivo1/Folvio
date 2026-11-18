# Folvio

**Folvio** is a native Windows desktop workspace for converting files and editing documents in place.

* **Convert** – documents, spreadsheets, presentations, images, audio and video. Only conversions that the
  installed engines can really perform are offered; every output is verified (opened/decoded/probed), not just
  checked for an exit code.
* **Auto Change** – open a PDF, click a paragraph, change the text, press `Ctrl+S`. Folvio rewrites only the
  changed text objects of that page; everything else in the file stays byte-for-byte as it was. The original is
  replaced safely (backup → temp file → verification → atomic swap → restore on failure).
* **Reflow editing** – Word-like editing of DOCX/ODT/RTF/Markdown/HTML/TXT, and of PDFs reconstructed into an
  editable document (clearly marked as not lossless).
* **Combine** – turn photos of paper pages, screenshots and scans into one PDF: arrange pages by drag & drop,
  paste screenshots with `Ctrl+V`, straighten photographed pages with four corner handles (real perspective
  correction, automatic page detection), crop, draw, highlight, add arrows, shapes and text, export with
  chosen page size, orientation, margins and quality. See [docs/COMBINE.md](docs/COMBINE.md).
* **Batch** – many files, one target format, parallel jobs with progress, cancel and retry.

Works offline. No account, no telemetry, no uploads.

## Build

Requirements (Windows 11 x64): Qt 6.10 for MinGW 64-bit (with CMake and Ninja from Qt Tools), Inno Setup 6 for
the installer. Optional engines: LibreOffice, FFmpeg, Tesseract, Microsoft Word.

```powershell
# OpenCV (core + imgproc, static) is built once from source into third_party/opencv:
#   powershell -ExecutionPolicy Bypass -File tools/build_opencv.ps1   (the release script does this when missing)
# PDFium binaries are committed in third_party/pdfium; to refresh them:
powershell -ExecutionPolicy Bypass -File tools\fetch_pdfium.ps1

# Release build + tests + deployment + installer (+ install for the current user)
powershell -ExecutionPolicy Bypass -File packaging\build_release.ps1 -Install
```

Development build from Git Bash:

```bash
source build.env.sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.0/mingw_64
cmake --build build/release
build/release/tests/folvio_tests.exe
```

Output: `dist/Folvio/` (portable, deployed app) and `dist/Folvio-Setup-<version>-x64.exe`.
The installer creates a Start menu entry, an optional desktop shortcut (real Desktop known folder, OneDrive
redirection included) and an uninstaller. Per-user install by default, no administrator rights required.

## Documentation

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) – modules, data flow, safety model
* [docs/COMBINE.md](docs/COMBINE.md) – Combine: processing pipeline, perspective correction, export
* [docs/TECHNOLOGY_AUDIT.md](docs/TECHNOLOGY_AUDIT.md) – engine comparison, licences, decisions
* [docs/TESTING.md](docs/TESTING.md) – test suite and how to run it
* [docs/LIMITATIONS.md](docs/LIMITATIONS.md) – known limitations and roadmap
* [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
