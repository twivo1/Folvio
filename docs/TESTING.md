# Testing

`build/release/tests/folvio_tests.exe` (GoogleTest). Tests generate their own fixtures at run time (PDFs with
`QPdfWriter`, media with FFmpeg `lavfi`, DOCX/encrypted PDFs with Python's standard library + `pypdf`) and run
real engines. Tests that need a missing engine are reported as *skipped*, never as passed.

Last full run (2026-11-18, Windows 11, all engines present): **49 tests, 49 passed, 0 skipped**.

| Suite | Covers |
|---|---|
| `AutoChange.EndToEndOriginalToEditedDocument` | The acceptance scenario: create PDF with "Original document" → open → change to "Edited document" → save → close → reopen → new text present, other content intact, verified backup exists, no temp leftovers |
| `AutoChange.EditingOnePageLeavesOtherPagesPixelIdentical` | Untouched pages render pixel-identically; on the edited page only the edited line band differs |
| `AutoChange.PolishCharactersRoundTrip` | Polish characters in edited text, path with Polish characters |
| `AutoChange.UndoRedoAndDelete`, `AddTextBoxAndStyleChange`, `MultiLineParagraphReflowsWithinBlock` | Editing operations |
| `AutoChange.FailedSaveKeepsOriginalIntact` | Injected failure before swap and corruption after swap: original preserved / restored |
| `AutoChange.DetectsExternalModificationAndReadOnly` | File changed by another program; read-only file never overwritten |
| `AutoChange.SaveAsCreatesNewFileAndLeavesOriginal`, `RecoveryRestoresUnsavedEdits`, `PasswordProtectedPdf` | Save As, crash recovery, encrypted PDFs |
| `Pdf.*` | Simple, multi-page, Polish text, tables + images, scanned (no text layer), corrupted/truncated, unusual fonts, layout |
| `Convert.*` | PNG→JPG (alpha), name conflicts, long/Polish paths, images↔PDF, Markdown→HTML/PDF, HTML→DOCX→PDF, PDF→DOCX, CSV→XLSX→JSON, MP4→MP3, MP4→MKV (remux), →H.265, FLAC→MP3, video without audio, MP4→GIF, frames, cancel, AVIF, repeated import |
| `Docx.HeadersAndFootersSurviveConversionToPdf`, `RichEditor.*` | DOCX header/footer, editor round trip DOCX→edit→DOCX/PDF, Markdown save-back |
| `Stability.ShutdownDuringProcessingStopsQuickly`, `ProcessRunner.*` | Closing while converting, timeouts, process-tree kill, no shell interpretation of arguments |
| `FormatDetector.*`, `PathUtils.*`, `SafeFileWriter.*`, `TextData.*`, `Planner.*`, `OutputValidator.*` | Units: magic bytes, sanitizing, CSV/JSON rules, route planning, missing dependencies, truncated outputs |

The release script (`packaging/build_release.ps1`) runs the suite and additionally smoke-tests the deployed
application with a minimal `PATH` (no Qt SDK) before building the installer.

UI checks: `Folvio.exe --screenshot out.png [--page N] [--theme light|dark] [--demo-edit] [file]` renders the
window to an image and exits (used to review layouts in both themes).
