# Folvio — third-party components

Folvio's own source code is licensed under the MIT licence (see `LICENSE`, © 2026 Bartosz Skrzypiec).
The release packages also contain the third-party components below, each under its own licence.
This document lists what is actually inside the packages; it is informational and not legal advice.

## In every package

| Component | Version | Licence | Linking | Use |
|---|---|---|---|---|
| Qt (Core, Gui, Widgets, Sql, Svg, Concurrent, Network + plugins) | 6.10.0 | LGPL-3.0-only | Dynamic (replaceable `.dll` / `.so` files) | User interface, imaging, SQLite access; Network only for the optional model download |
| ONNX Runtime (Microsoft, NuGet `Microsoft.ML.OnnxRuntime`) | 1.31.1 | MIT; its bundled components are listed in `licenses/onnxruntime/ThirdPartyNotices.txt` | Dynamic, loaded at run time | Handwriting recognition (optional module) |
| PDFium (prebuilt by bblanchon/pdfium-binaries) | chromium/8165 | BSD-3-Clause; bundled code (FreeType, ICU, libjpeg-turbo, OpenJPEG, LCMS, zlib, abseil, …) under the licences in `licenses/pdfium/` | Dynamic | PDF rendering, analysis and native editing |
| OpenCV (`core`, `imgproc`) | 4.10.0 | Apache-2.0 (includes zlib, zlib licence) | Static | Combine: perspective correction, page detection |
| SQLite (inside the Qt SQL plugin) | — | Public domain | Dynamic (plugin) | Settings and history |
| Tesseract `tessdata_fast` language data (`pol`, `eng`) | 4.1 | Apache-2.0 | Data files | OCR language models |

**Qt and the LGPL.** Folvio uses Qt under the LGPL-3.0. Qt is linked dynamically and the Qt libraries can be
replaced by the user. Folvio uses only unmodified Qt 6.10.0 from the Qt Project; its complete source code is
available at <https://download.qt.io/official_releases/qt/6.10/6.10.0/>. On Linux the TIFF image plugin is
rebuilt from that same `qtimageformats` source (unmodified) against the distribution's libtiff
(`packaging/linux/Dockerfile`). Qt's own bundled third-party code (libpng, libjpeg, libwebp, FreeType,
HarfBuzz, PCRE2, md4c, …) is documented at <https://doc.qt.io/qt-6/licenses-used-in-qt.html>.

## Windows installer only

| Component | Version | Licence | Use |
|---|---|---|---|
| MinGW-w64 runtime (libstdc++, libgcc, winpthreads) | GCC 13.1 | GPL-3.0 with the GCC Runtime Library Exception; winpthreads MIT/BSD | C++ runtime |
| Inno Setup (the installer program inside `Folvio-Setup-*.exe`) | 6 | Inno Setup licence (free for any use, including commercial) | Installation and uninstallation |

## Linux packages only (AppImage and .deb)

| Component | Licence | Use |
|---|---|---|
| ICU 73 (shipped with Qt) | Unicode License v3 / ICU | Unicode support in Qt |
| libxcb utility libraries (`xcb-cursor`, `xcb-icccm`, `xcb-image`, `xcb-keysyms`, `xcb-render-util`, …), libxkbcommon, libXau, libXdmcp, libwayland-cursor | MIT / X11 | X11 and Wayland integration of Qt |
| Liberation fonts | 2.1.5 | SIL Open Font License 1.1 | Metric-compatible substitutes for Arial, Times New Roman, Courier New when editing PDFs |
| Carlito, Caladea | — | SIL OFL 1.1 (Carlito), Apache-2.0 (Caladea) | Substitutes for Calibri and Cambria |
| DejaVu fonts | 2.37 | Bitstream Vera licence / public domain changes | Fallback fonts |
| AppImage type 2 runtime (AppImage only) | 20251108 | MIT; statically includes squashfuse (BSD-2-Clause) and libfuse (LGPL-2.1, source: <https://github.com/AppImage/type2-runtime/tree/20251108> and <https://github.com/libfuse/libfuse>) | Mounting the AppImage |

The licence texts of the bundled libraries and fonts are installed under `share/doc/folvio/` (Linux) or the
`licenses\` folder (Windows). Libraries that every Linux desktop provides (glibc, OpenGL/EGL, fontconfig,
FreeType, GLib, D-Bus, systemd, libtiff, X11, Wayland, …) are **not** bundled; the system copies are used.

## Downloaded on request (not in the packages)

| Component | Licence | Notes |
|---|---|---|
| PaddleOCR PP-OCRv6 small text detection and recognition models (PaddlePaddle, official ONNX exports) | Apache-2.0 | ~31 MB, downloaded only after the user agrees, from `huggingface.co/PaddlePaddle` at pinned commits, SHA-256 verified; removable in the Recognize Text panel. See `docs/HANDWRITING_OCR.md`. |

The GNHK handwriting dataset (CC-BY-4.0, Lee, Chung and Lee, ICDAR 2021) was used only to evaluate the
handwriting module during development; none of it is included in the repository or the packages.

## External engines (not bundled)

These run as separate programs. Folvio does not redistribute them; they are installed from their publishers
(Windows: through the Windows package manager `winget` when the user chooses *Settings → Install missing
engines*; Linux: through the distribution's packages, which the `.deb` lists as *Recommends*).

| Engine | Licence | Notes |
|---|---|---|
| FFmpeg | LGPL-2.1+ or GPL-2.0+ (depends on the build) | Audio/video conversion, HEIC and AVIF |
| LibreOffice | MPL-2.0 | Office document conversion; runs with macros disabled in a separate profile |
| Tesseract OCR | Apache-2.0 | OCR engine (the Polish and English data above are bundled) |
| Microsoft Word (Windows, optional) | Proprietary, the user's own licence | Higher-quality PDF→DOCX reconstruction through COM automation |

Bundling FFmpeg was considered and rejected for this release: the available Windows builds contain a dozen
LGPL/GPL libraries whose exact sources would all have to be shipped with every release.

## Build and packaging tools (not distributed)

CMake, Ninja, GCC/MinGW-w64, GoogleTest (BSD-3-Clause, tests only), aqtinstall (MIT), linuxdeploy and
linuxdeploy-plugin-qt (MIT), appimagetool (MIT). All downloads in the build scripts are pinned to a version
and verified with SHA-256.

## Considered and not used

* **MuPDF / PyMuPDF** — AGPL-3.0 or commercial licence.
* **ONLYOFFICE** — AGPL-3.0; requires a document server.
* **Qt WebEngine** — unavailable for the MinGW toolchain; not needed.
* **Qt PDF** — not used (Folvio uses PDFium directly); excluded from the packages.
