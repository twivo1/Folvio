# Technology audit (November 2026)

## Toolchain

| Choice | Reason |
|---|---|
| C++20, Qt 6.10 Widgets (MinGW 13.1 x64) | Native look, per-monitor DPI, mature widgets; installed toolchain on the target machine. Qt under LGPLv3, linked dynamically (commercial distribution allowed if Qt DLLs stay replaceable). |
| CMake + Ninja | Standard, fast, works with Qt's CMake packages. |
| SQLite (Qt SQL) | Local history/settings, zero administration. |
| GoogleTest | Requested; integrates with CTest. |
| Inno Setup 6 | Free incl. commercial use, per-user installs, known-folder aware shortcuts. |

## PDF editing engine – comparison

| Engine | Edits existing PDF objects? | Formatting fidelity | Integration | Offline | Licence / commercial use | Maintained |
|---|---|---|---|---|---|---|
| **PDFium** (chosen) | Yes: enumerate/remove/insert page objects, set text, embed TrueType/CID fonts, regenerate one page's content, incremental save | Unchanged objects are untouched; new text uses matched fonts | C API, prebuilt Windows DLLs | Yes | BSD-3-Clause / Apache-2.0 – **free for commercial use** | Google/Chromium, active |
| MuPDF | Yes (strong editing, redaction) | Very good | C API | Yes | **AGPL-3.0** or paid licence – blocks closed-source distribution without purchase | Artifex, active |
| Qt PDF | **No** – rendering/search only | n/a | Trivial | Yes | LGPL/GPL | Qt |
| Poppler | Mostly rendering; limited annotation editing | n/a | C++ API, GPL | Yes | **GPL** | Active |
| qpdf | Structural edits (pages, encryption), no text layout | n/a | C++ | Yes | Apache-2.0 | Active |

**Decision:** PDFium for rendering, analysis and native editing. MuPDF would be technically excellent but its AGPL
licence conflicts with future commercial distribution unless a licence is purchased (a decision for the
product owner, not made here).

## Document (reflow) editing engine – comparison

| Option | Real editor? | Fidelity | Integration on this toolchain | Licence |
|---|---|---|---|---|
| ONLYOFFICE Document Server | Yes, excellent | High | Requires Node-based server + embedded browser; **Qt WebEngine is not available for MinGW** | AGPL-3.0 |
| LibreOfficeKit (embedded) | Yes | High | Not supported for embedding on Windows | MPL-2.0 |
| Qt WebEngine + HTML editor (e.g. CKEditor) | Yes | Medium | Not available for MinGW; CKEditor 5 is GPL/commercial | GPL / commercial |
| **QTextDocument editor + LibreOffice/Word import-export** (chosen) | Yes: paragraphs, character formats, lists, tables, images, pagination | Medium (round-trip through HTML/ODT) | Native, no extra runtime | LGPL (Qt) + external MPL engine |

**Decision:** native QTextDocument-based editor with true pagination and zoom, used for DOCX/ODT/RTF/MD/HTML/TXT
and for PDFs the user explicitly chooses to *reflow*. Users are always warned that this path is not lossless. For
PDFs the default is native in-place editing, which preserves the original appearance.

## PDF → DOCX

| Engine | Quality | Notes |
|---|---|---|
| Microsoft Word (PDF Reflow, via COM) | Best available offline | Only when the user has Office; macros force-disabled |
| LibreOffice `writer_pdf_import` | Low (positioned frames) | Fallback; labelled as approximate |

PDF → DOCX is never presented as lossless.

## Other engines

| Area | Engine | Licence |
|---|---|---|
| Office conversions | LibreOffice headless | MPL-2.0 (separate process) |
| Audio/video, HEIC, AVIF | FFmpeg | LGPL/GPL depending on build (separate process) |
| OCR | Tesseract 5 + `tessdata_fast` pol/eng | Apache-2.0 |
| Images | Qt image plugins (PNG, JPEG, WebP, TIFF, GIF, BMP, SVG) | LGPL |

## Name

Candidates: **Folvio**, Refold, Formaft, Morvia, Pivotra, Alterio, Kilnform, Shiftlo. Chosen: **Folvio**
(*folio* + *via*). A quick web search found no software product with this name; "Refold" is an established
language-learning brand. This is not a trademark search.
