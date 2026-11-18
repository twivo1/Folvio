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
class PdfTextLayout {
public:
    // 'rules' are thin horizontal lines (e.g. table borders); lines separated by a rule
    // are never merged into one paragraph.
    static QList<TextBlock> build(const QList<PdfTextRun>& runs, int page, const QList<QRectF>& rules = {});
};

} // namespace folvio
