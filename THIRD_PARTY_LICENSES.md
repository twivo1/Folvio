# Folvio — third-party components

Folvio is a C++/Qt application. It bundles the components below and can use
separately installed engines. This document is informational and is not legal advice.

## Bundled with Folvio

| Component | Version | Licence | Use |
|---|---|---|---|
| Qt 6 (Core, Gui, Widgets, Sql, Svg, Concurrent, image format plugins) | 6.10 | LGPL-3.0 (dynamically linked; Qt libraries are replaceable DLLs) | User interface, imaging, SQLite access |
| PDFium (binaries by bblanchon/pdfium-binaries) | chromium/8165 | BSD-3-Clause; bundled third-party code under licences listed in `licenses/pdfium/` (FreeType, ICU, libjpeg-turbo, OpenJPEG, LCMS, zlib, abseil, …) | PDF rendering, analysis and native editing |
| SQLite (via Qt SQL plugin) | — | Public domain | Settings and history |
| Tesseract `tessdata_fast` language data (`pol`, `eng`) | 4.1 | Apache-2.0 | OCR language models |
| MinGW-w64 runtime (libstdc++, libgcc, winpthreads) | GCC 13.1 | GPL-3.0 with GCC Runtime Library Exception / MIT-style | C++ runtime |

Qt source code is available from https://download.qt.io/ and the Qt Company. To
comply with the LGPL, Folvio links Qt dynamically; users may replace the Qt DLLs.

## External engines (not bundled, detected at run time)

| Engine | Licence | Notes |
|---|---|---|
| LibreOffice | MPL-2.0 | Office document conversion; runs as a separate process with macros disabled |
| FFmpeg | LGPL-2.1+ or GPL (depends on the build) | Audio/video, HEIC and AVIF; separate process |
| Tesseract OCR | Apache-2.0 | OCR; separate process |
| Microsoft Word | Proprietary (user's own licence) | Optional; PDF→DOCX reconstruction via COM automation |

Because these engines run as separate programs and are not distributed with
Folvio, their licences do not apply to Folvio's own code. Commercial
redistribution of Folvio *together with* any of these engines must follow the
respective licences (e.g. GPL FFmpeg builds require GPL-compatible distribution;
use an LGPL build when bundling).

## Considered and not used

* **MuPDF / PyMuPDF** — AGPL-3.0 or commercial licence; would force AGPL or a paid licence.
* **ONLYOFFICE** — AGPL-3.0; requires a document server.
* **Qt WebEngine** — unavailable for the MinGW toolchain.
