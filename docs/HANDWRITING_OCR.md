# Recognize Text and handwriting recognition

Folvio's **Recognize Text** tool (Combine editor and the Auto Change image editor) turns a selected area of a
page into editable text. Printed text is read by Tesseract; handwriting by an optional, **experimental** module.
Everything runs offline on the user's computer.

## User flow

1. Choose **Recognize** (toolbar) or **Recognize Text** (image editor header) and drag a rectangle.
2. The area is cut from the *final* page (rotation, perspective and crop applied) at **full resolution**, and
   recognised in the background. *Text type*: **Auto**, **Printed text**, **Handwriting (experimental)**.
   Auto keeps Tesseract's result when it is confident (≥ 80 %), otherwise uses the handwriting module when installed.
3. The **Recognized Text** panel shows the editable result with the engine and the mean confidence, plus
   warnings: low confidence (< 70 %), and *formulas or a drawing* (many symbols/digits relative to letters).
4. Actions: **Copy**, **Insert as Text** (a normal text annotation below the selection), **Replace Selection…**,
   **Cancel**. Replace always opens a dialog with **Before / After** previews and four variants:
   * *Insert as overlay* - text over the original, which stays visible;
   * *Keep the original and add the text below*;
   * *Replace on a clean background* - the area is covered with the paper colour measured on the selection border,
     and the text (in the measured ink colour) is fitted into it;
   * *Create a clean note* - a white note with the text over the area.
   When the border of the selection is not plain (grid, ruled lines, texture), the dialog warns and preselects the
   overlay. Nothing changes before **Apply**; everything is an annotation, the photo itself is never modified,
   and one **Undo** restores the area.

## Engine choice (research, November 2026)

| Option | Code / weights licence | Polish | Size | CPU speed | Offline, no Python | Verdict |
|---|---|---|---|---|---|---|
| **PaddleOCR PP-OCRv6 small** det + rec (official ONNX exports) | Apache-2.0 / Apache-2.0 | yes (50 languages, all Polish letters in the dictionary) | 9.9 MB + 21.2 MB | ~0.5 s per phone photo of a page | yes, via ONNX Runtime C API | **chosen** |
| PaddleOCR PP-OCRv6 medium rec | Apache-2.0 | yes | 76.6 MB | ~4× slower | yes | better WER, worse CER on our set; too large for an optional download |
| PaddleOCR PP-OCRv5 latin mobile rec | Apache-2.0 | yes | 8 MB | similar | yes | weaker than v6 small |
| Microsoft TrOCR (base/large handwritten) | MIT | **no** (English IAM only) | 330 MB - 2.2 GB | seconds per line on CPU | needs a converted encoder/decoder | rejected |
| Tesseract 5 (already used for print) | Apache-2.0 | yes | - | - | yes | not able to read handwriting (see below) |
| Cloud OCR APIs | proprietary, paid | yes | - | - | no | rejected: uploads documents, needs keys |

Runtime: **ONNX Runtime 1.31.1** (MIT, Microsoft), from the official NuGet package `Microsoft.ML.OnnxRuntime`
(pinned version, SHA-256 checked by `tools/fetch_onnxruntime.*`). It is shipped with Folvio (17 MB on Windows,
29 MB on Linux) and loaded at run time through its C API (`QLibrary` + `OrtGetApiBase`), so Folvio starts and works
without it. Runtime telemetry is switched off (`DisableTelemetryEvents`). On Windows the DLL needs the Microsoft
Visual C++ runtime, which practically every Windows 11 has; if it is missing the panel says so and names the
`winget` package.

## Model distribution

The models are **not** in the installer. When handwriting is first needed, Folvio asks for consent (size, source,
"nothing is uploaded") and downloads three files from PaddlePaddle's own Hugging Face repositories, pinned to
commits and verified with SHA-256 (`HandwritingOcr::modelFiles()`):

| File | Source | SHA-256 |
|---|---|---|
| `det.onnx` (9 880 512 B) | `PaddlePaddle/PP-OCRv6_small_det_onnx` @ `28fe5895…` | `d73e0058…10e` |
| `rec.onnx` (21 159 378 B) | `PaddlePaddle/PP-OCRv6_small_rec_onnx` @ `b8f84f0b…` | `5435fd74…634` |
| `rec.yml` (150 579 B, character list) | same commit | `ab078671…7d1` |

Files are written as `*.part`, hashed while downloading and only then renamed; a damaged or incomplete file is
deleted and fetched again. Location: `%LOCALAPPDATA%\Folvio\models\ppocrv6-small` (Windows),
`~/.local/share/Folvio/models/ppocrv6-small` (Linux). **Remove handwriting models** in the panel deletes the folder.
Downloads use Qt Network (Windows: the system's Schannel TLS; Linux: the system OpenSSL) - GET requests for
public files only, with a generic `Folvio` user agent.

## Pipeline (`src/engines/ocr/HandwritingOcr.cpp`)

* Detection (DB): longer side scaled to ≤ 1280 px, both sides multiples of 32 (round half to even, as PaddleOCR),
  ImageNet normalisation on BGR, probability > 0.3, box score ≥ 0.6, unclip ratio 1.5, rotated boxes.
* Recognition (CTC): each box is perspective-cropped, turned upright when tall, scaled to height 48, normalised to
  [-1, 1], greedy CTC decoding; confidence = mean of the chosen probabilities.
* Reading order: boxes whose centres lie within half a box height form one line, left to right.
* The C++ implementation was checked against a Python reference of the same pipeline (same ONNX Runtime and
  OpenCV versions) on lossless images of real handwriting: identical text on all six pages
  (`HandwritingOcr.RealHandwritingMatchesReferencePipeline`).

## Measured quality

Data: **GNHK** (GoodNotes Handwriting Kollection, ICDAR 2021, **CC-BY-4.0**): phone photos of real English
handwriting by many writers, used **only for evaluation** - not stored in the repository or distributed.
Mirror used: `radhakrishnadeshpande/gnhk-pagelevel` @ `0189a76a`, test split. Metrics on whole pages, lines in
reading order, against the human transcription (scribbles and unreadable words removed by the dataset).

| Engine | Pages | CER | WER | Words found (recall) | Time / page (CPU) |
|---|---|---|---|---|---|
| PP-OCRv6 small (Folvio) | 172 | **33.5 %** | 64.8 % | **57.4 %** | 0.49 s |
| Tesseract 5.4 `eng` | 172 | 84.1 % | 103.6 % | 7.2 % | 0.60 s |

Line-level (339 annotated lines of 30 pages; crops from the dataset polygons): PP-OCRv6 small CER 31.9 %,
medium 35.7 % (WER 57.8 %), tiny 36.0 %, PP-OCRv5 latin 40.8 %, Tesseract 76.6 %.
Clean, upright notes do much better than the average: e.g. a short quote on ruled paper is recognised
perfectly (`RecognizeText.HandwritingAreaOnRealSample`), while slanted photos with mixed scripts and
tiny writing pull the average down. Memory: peak ~310 MB for a 9.6-megapixel page (models included).

**Polish handwriting has not been measured on real samples**: no openly licensed dataset of modern Polish
handwriting was found, and private notes were deliberately not used. The model's dictionary contains all
Polish letters and synthetic Polish text is read correctly, but real Polish handwriting quality is unknown.
For this reason the module is labelled **experimental** in the UI, results are always shown for review before
use, and replacements keep the original.

Reproduce: `tools/handwriting_eval/` (Python, development only; see its README).

## Known limitations

* Experimental quality (see above): expect errors, especially with cursive, slanted lines, small writing.
* Formulas and drawings are not understood - the panel warns, and the overlay keeps the original visible.
* Tesseract often reads nothing through grid/ruled lines on printed text; the handwriting module (Auto) or a
  manual correction helps.
* Vertical text and text rotated by more than ~45° are not supported.
