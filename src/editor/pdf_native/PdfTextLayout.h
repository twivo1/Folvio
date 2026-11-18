#pragma once

#include "engines/pdf/PdfDocument.h"

#include <QList>
#include <QRectF>
#include <QString>

namespace folvio {

struct TextLine {
    QList<int> runs;        // indices into the page's run list
    QString text;
    QRectF bounds;          // PDF space
    double baseline = 0;    // y of the baseline (PDF space)
    double left = 0;        // x where the line starts (first run origin)
    bool startsItem = false; // first line of a list item (bullet/number marker)
    QList<int> markerRuns;   // list marker runs (bullet symbols, "1.") - never part of the editable text
};

struct TextBlock {
    int id = -1;            // stable id within a page (order of appearance)
    int page = -1;
    QList<TextLine> lines;
    QRectF bounds;          // PDF space
    double lineSpacing = 0; // baseline-to-baseline distance (0 for single line)
    PdfTextRun style;       // dominant run style (longest text)
    bool mixedStyle = false;
    bool editable = true;   // false for rotated / invisible text
    QList<int> objectIndices; // page object indices belonging to this block

    QString text() const;   // lines joined with '\n'
    int lineCount() const { return int(lines.size()); }
};

// Groups PDFium text objects into lines and paragraphs.
//
// * Lines never span text columns: runs that are not written consecutively in
//   the content stream and are separated by a visible gap belong to different
//   flows (e.g. a date in the left column and a skill list in the right one).
// * Each list item (bullet/number marker drawn as text or as a small graphic)
//   starts its own block, so editing one item never reflows the next.
// * Lines separated by a horizontal rule (table borders) are never merged.
class PdfTextLayout {
public:
    static QList<TextBlock> build(const QList<PdfTextRun>& runs, int page, const QList<QRectF>& rules = {},
                                  const QList<QRectF>& graphics = {});

    static bool isBulletText(const QString& text);
};

} // namespace folvio
