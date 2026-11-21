# Combine

Combine turns a set of images (photos of paper pages, screenshots, scans) into one multi-page PDF. The work
happens in one workspace with three steps: **1 Arrange → 2 Edit → 3 Export**.

## Modules (`src/combine/`)

| File | Layer | Responsibility |
|---|---|---|
| `CombineModel.*` | Domain | `CombinePage` (source, rotation, corners, crop, annotations), `Annotation`, `ExportSettings`, `CombineDocument` (ordered pages, snapshot undo/redo up to 200 steps, macros), `PageListModel` (thumbnails) |
| `ImageGeometry.*` | Image processing | `Geometry` (corner validation, ordering, output size, aspect estimation, homography, rotation of corners) and `ImageProcessor` (EXIF-aware loading, rectification with OpenCV `warpPerspective`, page detection, crop, rendering the processing pipeline) |
| `AnnotationRender.*` | Image processing | Painting, hit testing, erasing and text layout of annotations — shared by the editor, the thumbnails and the export check |
| `CombinePdfExporter.*` | Export | Page plan (size, orientation, margins, image placement), PDF generation with PDFium, verification after writing, safe write |
| `CombineSession.*` | Persistence | Import (files, folders, clipboard bitmaps, copied files), autosave/recovery, `.folvio` project files |
| `PageGridView.*` | GUI | Thumbnail grid (Arrange) and page list (Edit): drag & drop reordering with an insertion line |
| `PageCanvas.*` | GUI | Interactive editor for one page: perspective handles, crop frame, drawing tools, selection & transform, inline text editor |
| `CombineWorkspace.*` | GUI | The three-step workspace, options panel, shortcuts, background thumbnails, background export |

The domain, image processing, export and persistence layers have no widget dependencies and are tested
directly; the GUI is tested end to end through Qt events.

## Reordering pages

The document (`CombineDocument::movePages`) is the only owner of the page order; the views never position
cards themselves (`QListView::Static`). `PageGridView` handles drag & drop: while dragging it computes the
insertion index — left/right half of a card in the grid, top/bottom half in the vertical page list, below the
last row = end, above the first = beginning — and draws a line there. On drop it asks the workspace to move the
selected rows, which is one undo step (a drop in place changes nothing and is not recorded). Only
`Qt::MoveAction` is offered, so a drag never duplicates pages, and nothing is removed after the drag loop.
Buttons and `Ctrl+↑/↓` use the same path. Leaving the view or pressing Escape cancels.

## Non-destructive processing pipeline

Source files are never modified. Every page stores only parameters; pixels are produced on demand in this
order:

1. **Source** decoded at full resolution (`QImageReader`, allocation limit 2 GB; HEIC/AVIF through FFmpeg into
   a PNG session asset).
2. **EXIF orientation** applied (`autoTransform`).
3. **Rotation** by the user (90° steps).
4. **Perspective correction** — the four corners (TL, TR, BR, BL) are stored in full-resolution pixel
   coordinates of step 3. A homography maps the quadrilateral onto a rectangle (`cv::warpPerspective`, bicubic,
   white outside). An optional safety margin keeps a band of the photo around the page.
5. **Crop** — a normalised rectangle of the straightened image (so changing the corners and cropping are
   independent, and neither "copies" pixels from the other).
6. **Annotations** — vector objects in page units (x / width, y / width), so they keep their place and size at
   any zoom level and at any export resolution.

Rotating a page that already has corners rotates the corners with it and re-labels them for the new
orientation, so the straightened result turns by 90° as well.

### Output proportions

* *Auto* estimates the real height/width ratio of the photographed page from the corners with a pinhole camera
  model (principal point at the photo centre, square pixels; Zhang & He, *Whiteboard scanning and image
  enhancement*, 2007). When one pair of edges is parallel in the photo the focal length cannot be measured and
  a typical phone camera focal length (0.7 × image diagonal) is assumed; when the corners are inconsistent with
  a camera the longest opposite edges are used. The detail level follows the longest edges (no upscaling of a
  small page, no loss of detail on a large one).
* *A4*, *Letter*, *Custom* force the ratio; portrait/landscape is chosen with the same camera model.

### Corner validation

Rejected with a message (and the *Straighten* button disabled): crossed or mirrored order, concave shapes,
interior angles outside 12°–168°, area under 0.5 % of the photo, edges shorter than 8 px, corners more than 2 %
outside the photo. *Fix corner order* sorts shuffled points.

### Page detection

Downscaled grayscale → Gaussian blur → Canny at three threshold pairs → dilation → external contours →
polygon approximation (several tolerances) → the largest convex quadrilateral covering 15–99.5 % of the photo;
Otsu thresholding as a fallback. The result is only a proposal — the handles stay editable. If nothing
convincing is found, Folvio says so and leaves the corners where they were.

## Coordinate systems

| System | Unit | Defined by |
|---|---|---|
| Source | pixels of the file (EXIF orientation applied) | `sourceSize` |
| Rotated | pixels after the user's 90° rotations | `Geometry::rotatedSize` |
| Rectified (perspective-corrected) | pixels of the straightened page incl. margin | `Geometry::rectifiedPixelSize` |
| Cropped / final | the crop rectangle, stored as fractions of the rectified image | `CombinePage::crop` |
| Annotation (page units) | final pixels divided by the final **width**, on both axes (x ∈ [0, 1], y ∈ [0, H/W]) | `Annotation` |
| Viewport | widget pixels: page units × displayed image width + offset (zoom, scrolling) | `PageCanvas::fromPage/toPage` |

Annotations live in page units of the **final** page, so they are remapped whenever the final frame changes:
applying or resetting a crop (`Geometry::remapAnnotationsForCrop`: scale = old crop width / new crop width,
translation by the crop offset) and rotating (`remapAnnotationsForRotation`, after leaving the crop). Stroke widths
and font sizes scale with the content, so an annotation stays on the same words. Zoom only changes the viewport
mapping and never touches stored coordinates.

Crop is non-destructive: the crop rectangle is a page property (Undo/Redo, `.folvio` projects, PDF export use it);
choosing Crop again shows the whole straightened page with the current frame. After **Apply crop** the canvas
returns to Select, shows only the cropped area, fits it into the view and the thumbnail is regenerated.

## Annotations

Pen (pressure from a tablet, optional smoothing), highlighter (translucent), line, arrow, rectangle, ellipse,
polyline (stroke, fill, opacity), text boxes (font, size, colour, bold, italic, underline, alignment, several
lines, Polish characters). Select tool: move, resize with 8 handles, delete, duplicate (Ctrl+D), bring forward /
send backward, edit properties in the panel, double-click text to edit. Eraser removes only annotations: strokes
are split where the eraser passes, shapes and text are removed whole.

Text boxes: the editor is transparent (only a thin dashed frame, a *Move* tab and three resize handles, none of
which is part of the page or the PDF). Font, size, colour, opacity, B/I/U and alignment apply to the box being
typed in immediately. Width changes re-wrap the text and never scale the font; *Auto size* keeps the height equal
to the text, dragging the bottom edge sets a fixed height that can never clip the text. Text tool: click empty
space to add, click a text to edit, drag a text to move it. Select tool: drag moves, a second click on the selected
text edits it. Ctrl+Enter or Esc finishes typing; Ctrl+Z / Ctrl+Y undo and redo inside the box while typing.

**Recognize** (also in the Auto Change image editor): drag around printed or handwritten text; see
`docs/HANDWRITING_OCR.md`.

## Export

* Page size *Original image size* (150 dpi, clamped 2"–20"), *A4*, *Letter*, *Custom*; orientation *Auto*
  (from the image), *Portrait*, *Landscape*; margins None / Small (6 mm) / Normal (13 mm) / Custom. The image is
  scaled to fit and centred, never distorted.
* Quality: *High* (lossless), *Balanced* (unchanged JPEG photos are embedded **byte-for-byte** without
  re-encoding; edited photos as JPEG q90; screenshots/PNG stay lossless), *Small file* (JPEG q72).
* Annotations are written as PDF vector paths and text objects (font embedded as CID with a verified ToUnicode
  map, so text is searchable and Polish characters extract correctly); transparency is real PDF transparency.
* The export runs on a worker thread with real per-page progress and can be cancelled. It writes to a
  temporary file in the target folder, reopens it with PDFium (page count, page sizes, text annotations
  present), then moves it into place. Existing files are replaced only after confirmation. A cancelled or
  failed export leaves no file behind. The session is kept after export.

## Session and projects

The current pages are autosaved (debounced, `QSaveFile`) to `%LOCALAPPDATA%\Folvio\combine\session`. Pasted
images are stored there as PNG. After a crash or restart Combine offers *Restore* / *Discard*. *Project → Save
project as…* writes a `.folvio` JSON file; pasted images are always embedded, other images optionally. Missing
or changed source images are reported (SHA-256 fingerprint), never silently skipped; export refuses to run while
a source is missing.

## Keyboard

| Keys | Action |
|---|---|
| Ctrl+O | Add images |
| Ctrl+V | Paste screenshot / copied image files (in text fields Ctrl+V pastes text) |
| Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z | Undo / redo |
| Delete | Remove selected pages (thumbnails) or the selected annotation (editor) |
| Ctrl+Up / Ctrl+Down | Move selected pages |
| Enter | Open the page in the editor / finish a polyline |
| PageUp / PageDown | Previous / next page in the editor |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0 | Zoom in, zoom out, fit page |
| Tab, arrows (+Shift) | Perspective: next corner, move the corner by one screen pixel (ten with Shift) |
| Ctrl+Enter, Esc | Text box: finish, cancel |
