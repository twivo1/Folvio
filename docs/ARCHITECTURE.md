# Architecture

```
src/
├── app/                 Application bootstrap (QApplication, theme, DB, command line, logging)
├── core/
│   ├── files/           FormatRegistry/FormatDetector (magic bytes), PathUtils, SafeFileWriter
│   ├── process/         ProcessRunner: external processes in Windows Job Objects
│   ├── deps/            DependencyManager: locates engines (override → bundled → known paths → PATH)
│   ├── conversions/     IConverter, ConversionRegistry, ConversionPlanner, OutputValidator, Builtins
│   ├── jobs/            ConversionExecutor, ConversionJob, JobQueue (bounded QThreadPool)
│   └── persistence/     Database (SQLite: history, recent files, settings – metadata only)
├── engines/
│   ├── pdf/             PdfDocument (PDFium wrapper), PDF→image, images→PDF
│   ├── documents/       LibreOfficeConverter (headless, hardened profile), WordConverter (COM, optional)
│   ├── media/           FfmpegConverter (remux when possible, real progress)
│   ├── images/          QtImageConverter, FfmpegImageConverter (HEIC/AVIF)
│   ├── data/            TextDataConverter (CSV↔JSON, Markdown↔HTML, Markdown→PDF)
│   └── ocr/             TesseractEngine (TSV with per-word confidence)
├── editor/
│   ├── pdf_native/      Auto Change: PdfTextLayout, PdfEditSession, FontMatcher, PdfCanvas, PdfWorkspace
│   └── rich/            Reflow editing: RichDocumentIO, PagedDocumentView, RichWorkspace
├── combine/             Combine: model + undo, ImageGeometry (homography, detection), AnnotationRender,
│                        CombinePdfExporter, CombineSession (import, autosave, .folvio), PageCanvas, CombineWorkspace
└── ui/                  MainWindow, pages, widgets, Theme (light/dark tokens), Icons
engines/                 Runtime data shipped next to Folvio.exe (Word helper script, OCR language data)
packaging/               Release script, Inno Setup script, Windows resources/manifest
tools/brand/             Generates PNG/ICO/wordmarks/installer art from the master SVG logo
tests/                   GoogleTest suite
```

## Conversion system

* Each `IConverter` declares **capabilities** (`from → to`, fidelity, lossy flag, user-facing note), the
  external **tools** it needs and the **options** it accepts. A converter whose tool is missing contributes no
  routes, so the UI never shows impossible conversions.
* `ConversionPlanner` prefers the best direct route; one intermediate hop is allowed only through a small
  whitelist of hubs (`pdf` for office→image, `png` for image bridges, `html` for Markdown↔office, `csv` for
  spreadsheet↔JSON) and never chains two lossy steps.
* `ConversionExecutor` runs each step in a private temp folder, validates every produced file with
  `OutputValidator` (PDF opens in PDFium, images decode, ZIP containers have their main part, media has the
  expected streams and a duration) and finally moves results to the destination without overwriting anything
  (`name (1).ext`). Multi-file outputs (pages, frames) go into a new folder.
* `JobQueue` runs jobs on a bounded thread pool; progress is marshalled to the GUI thread. Cancel sets an atomic
  flag; `ProcessRunner` then terminates the whole **Job Object** (process tree, e.g. `soffice.com` →
  `soffice.bin`). Job Objects also cap memory per conversion.
* External programs are always started with an **argv array** (`QProcess::setArguments`), never through a
  shell. Inputs are copied to neutral names for LibreOffice/Word; FFmpeg inputs use the `file:` protocol and a
  protocol whitelist.

## Auto Change (native PDF editing)

1. `PdfDocument::textRuns` lists text objects with geometry, font, colour, and – from PDFium's text page – the
   characters and word boundaries of each object (advance-based, so narrow glyphs do not create fake spaces).
2. `PdfTextLayout` clusters runs into lines (baseline clustering, column gaps) and lines into paragraph blocks
   (size, spacing, alignment; table rules separate blocks).
3. Edits are **operations** (`EditOp`) replayed on the pristine original bytes for every state. Undo/redo is a
   cursor over the op list; batches (Replace all, OCR) undo as one step.
4. Replacing a block aligns old and new lines with an LCS diff. Unchanged lines keep their original objects
   (only translated if lines were added/removed above them). Changed lines become new text objects inserted at
   the **content-stream position** of the removed ones (reading order preserved), using the original text
   matrix, colour and size. Each word is a separate object positioned with the font's advance widths.
5. Fonts (`PdfTextWriter`): a changed line is written with the document's own embedded font when every
   character is known to exist in it (characters PDFium extracted from that font). Otherwise `FontMatcher`
   picks an installed TrueType family, embedded via `FPDFText_LoadCidType2Font` with a **Folvio-generated
   ToUnicode CMap** (PDFium's auto-generated map resolves '-' to U+00AD and ' ' to U+00A0, which corrupted
   copy/search). Every written object is read back; a mismatch retries with the system font or refuses the edit.
6. Safety: lines whose characters PDFium cannot map (U+FFFD) are never rewritten; a paragraph may only grow into
   free space (collision check against other blocks and graphics), otherwise the edit is refused and the
   document stays unchanged. Lines never span text columns (runs that are not consecutive in the content
   stream and separated by a gap belong to different flows); list items (text or graphic markers) are separate
   paragraphs and their markers are never rewritten.
7. Only modified pages get `FPDFPage_GenerateContent`; the rest of the file is untouched (verified by
   pixel-identical rendering in tests). Signed PDFs are saved incrementally.
8. Save = `serialize` → `verifyOutput` → `SafeFileWriter::replace`. `verifyOutput` re-opens the bytes and checks,
   object by object (independent of paragraph detection): untouched pages have identical text; on edited pages
   the multiset of text objects equals original-minus-intentionally-removed plus inserted; every new line is
   present in content order; the saved objects equal the in-memory edited document.

## Combine

Images → arranged, straightened, annotated, multi-page PDF. Non-destructive: pages store parameters (rotation,
four corners, crop, vector annotations) and pixels are produced on demand (preview at ≤2200 px, export at full
resolution). Perspective correction uses OpenCV (`warpPerspective`, static `core` + `imgproc`), PDF output uses
PDFium with vector annotations and real text. Details: [COMBINE.md](COMBINE.md).

## Safe saving (`SafeFileWriter`)

`isWritable` (read-only attribute, permissions, locked by another program) → external-change check (size +
SHA-256 fingerprint taken at open) → producer writes a hidden temp file in the same folder → validator →
verified backup copy (`%LOCALAPPDATA%\Folvio\backups` by default) → `ReplaceFileW` (keeps ACLs/attributes;
`MoveFileExW` fallback) → hash check of the result → automatic restore from backup on any mismatch. Temp files
are removed in all paths. Failure injection hooks exist for tests.

## Reflow editing

`RichDocumentIO` imports DOCX/DOC/ODT/RTF via LibreOffice → HTML (images embedded), PDFs via Word (or
LibreOffice) → DOCX → HTML. `PagedDocumentView` hosts a `QTextEdit` in a `QGraphicsView`: true A4 pagination by
`QTextDocumentLayout`, real zoom by view transform, page breaks drawn in the margins. Saving writes ODT with
`QTextDocumentWriter` and converts it back with LibreOffice, or prints PDF from the same paginated layout.
Every save goes through `SafeFileWriter` with format validation, and the user is told what may not survive.

## Privacy and robustness

* Database stores paths, formats, statuses and settings – never document content.
* Crash recovery stores only pending edit operations for the open document, deleted on save/close.
* LibreOffice runs in Folvio-owned profiles with macros disabled; Word with `AutomationSecurity = ForceDisable`.
* Limits: PDFs > 2 GB rejected, render size capped, image decoder allocation limit, batch size capped, per-process
  memory cap, timeouts on all external processes. Stale temp folders are cleaned at start-up.
