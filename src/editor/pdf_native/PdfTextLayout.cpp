#include "editor/pdf_native/PdfTextLayout.h"

#include <algorithm>
#include <cmath>

namespace folvio {

QString TextBlock::text() const
{
    QStringList l;
    for (const auto& line : lines)
        l << line.text;
    return l.join(QLatin1Char('\n'));
}

namespace {

bool sameStyle(const PdfTextRun& a, const PdfTextRun& b)
{
    return a.fontName == b.fontName && std::abs(a.effectiveSize - b.effectiveSize) < 0.6 && a.color == b.color
           && a.bold == b.bold && a.italic == b.italic;
}

double baselineOf(const PdfTextRun& r) { return r.matrix.dy(); }

} // namespace

QList<TextBlock> PdfTextLayout::build(const QList<PdfTextRun>& runs, int page, const QList<QRectF>& rules)
{
    auto ruleBetween = [&rules](const TextLine& upper, const TextLine& lower) {
        const double yTop = upper.baseline, yBottom = lower.baseline;
        const double left = std::max(upper.bounds.left(), lower.bounds.left());
        const double right = std::min(upper.bounds.right(), lower.bounds.right());
        for (const QRectF& r : rules) {
            const double y = r.center().y();
            if (y < yTop && y > yBottom && r.left() < right && r.right() > left)
                return true;
        }
        return false;
    };
    QList<TextBlock> blocks;
    // 1. Non-editable runs (rotated or invisible) each become their own block.
    QList<int> flow;
    for (int i = 0; i < runs.size(); ++i) {
        const PdfTextRun& r = runs[i];
        if (r.text.trimmed().isEmpty() && r.text.isEmpty())
            continue;
        if (!r.axisAligned || r.renderMode == 3 /* invisible (OCR layers) */) {
            TextBlock b;
            b.page = page;
            TextLine line;
            line.runs = {i};
            line.text = r.text;
            line.bounds = r.bounds;
            line.baseline = baselineOf(r);
            line.left = r.matrix.dx();
            b.lines << line;
            b.bounds = r.bounds;
            b.style = r;
            b.editable = r.axisAligned && r.renderMode != 3;
            b.objectIndices = {r.objectIndex};
            blocks << b;
            continue;
        }
        flow << i;
    }

    // 2. Lines. Sort strictly by baseline, cluster baselines within a tolerance,
    //    then order each cluster by x and split it at column-sized gaps.
    std::sort(flow.begin(), flow.end(), [&](int a, int b) {
        const double ya = baselineOf(runs[a]), yb = baselineOf(runs[b]);
        if (ya != yb)
            return ya > yb;
        return runs[a].bounds.left() < runs[b].bounds.left();
    });
    QList<QList<int>> clusters;
    double clusterBase = 0;
    for (int idx : flow) {
        const double y = baselineOf(runs[idx]);
        const double tol = 0.3 * std::max(1.0, runs[idx].effectiveSize);
        if (clusters.isEmpty() || clusterBase - y > tol) {
            clusters.append(QList<int>{idx});
            clusterBase = y;
        } else {
            clusters.last().append(idx);
        }
    }
    QList<TextLine> lines;
    for (QList<int>& cluster : clusters) {
        std::sort(cluster.begin(), cluster.end(), [&](int a, int b) {
            if (runs[a].bounds.left() != runs[b].bounds.left())
                return runs[a].bounds.left() < runs[b].bounds.left();
            return a < b;
        });
        bool lineOpen = false;
        for (int idx : cluster) {
            const PdfTextRun& r = runs[idx];
            const double size = std::max(1.0, r.effectiveSize);
            if (lineOpen) {
                TextLine& l = lines.last();
                const PdfTextRun& prev = runs[l.runs.last()];
                const double gap = r.bounds.left() - l.bounds.right();
                // Gaps wider than ~2.5 em indicate separate columns / table cells.
                if (gap < 2.5 * size) {
                    // Word boundaries come from PDFium's text page (advance-width based);
                    // very large gaps are spaces regardless.
                    const bool needSpace = (prev.spaceAfter || gap > 0.8 * std::min(size, std::max(1.0, prev.effectiveSize)))
                                           && !l.text.endsWith(QLatin1Char(' ')) && !r.text.startsWith(QLatin1Char(' '));
                    if (needSpace)
                        l.text += QLatin1Char(' ');
                    l.text += r.text;
                    l.runs << idx;
                    l.bounds = l.bounds.united(r.bounds);
                    continue;
                }
            }
            TextLine l;
            l.runs = {idx};
            l.text = r.text;
            l.bounds = r.bounds;
            l.baseline = baselineOf(r);
            l.left = r.matrix.dx();
            lines << l;
            lineOpen = true;
        }
    }

    // 3. Paragraph blocks: consecutive lines with the same size, alignment and regular spacing.
    auto dominantRun = [&](const TextLine& l) -> const PdfTextRun& {
        int best = l.runs.first();
        for (int i : l.runs)
            if (runs[i].text.size() > runs[best].text.size())
                best = i;
        return runs[best];
    };
    QList<TextBlock> flowBlocks;
    for (const TextLine& l : lines) {
        const PdfTextRun& s = dominantRun(l);
        bool merged = false;
        // Look back through recent blocks so multi-column pages still form paragraphs.
        for (int bi = int(flowBlocks.size()) - 1; bi >= 0 && bi >= int(flowBlocks.size()) - 24 && !merged; --bi) {
            TextBlock& b = flowBlocks[bi];
            const TextLine& last = b.lines.last();
            const double size = std::max(1.0, b.style.effectiveSize);
            const double pitch = last.baseline - l.baseline;
            if (pitch > 2.3 * size)
                continue;
            const bool sizeOk = std::abs(s.effectiveSize - b.style.effectiveSize) < 0.15 * size;
            const bool pitchOk = pitch > 0.8 * size && pitch < 2.25 * size
                                 && (b.lineSpacing <= 0 || std::abs(pitch - b.lineSpacing) < 0.25 * b.lineSpacing);
            const bool leftAligned = std::abs(l.bounds.left() - b.bounds.left()) < 1.2 * size;
            const bool overlap = l.bounds.left() < b.bounds.right() && l.bounds.right() > b.bounds.left();
            const bool centered = std::abs(l.bounds.center().x() - b.bounds.center().x()) < 1.2 * size;
            if (sizeOk && pitchOk && overlap && (leftAligned || centered) && s.fontName == b.style.fontName
                && s.bold == b.style.bold && !ruleBetween(last, l)) {
                if (b.lineSpacing <= 0)
                    b.lineSpacing = pitch;
                b.lines << l;
                b.bounds = b.bounds.united(l.bounds);
                merged = true;
            }
        }
        if (!merged) {
            TextBlock b;
            b.page = page;
            b.lines << l;
            b.bounds = l.bounds;
            b.style = s;
            flowBlocks << b;
        }
    }

    for (TextBlock& b : flowBlocks) {
        // PDFium appends generated spaces to the last glyph before a gap; drop them at line ends.
        for (TextLine& l : b.lines)
            while (!l.text.isEmpty() && l.text.back().isSpace())
                l.text.chop(1);
        for (const TextLine& l : b.lines)
            for (int i : l.runs) {
                b.objectIndices << runs[i].objectIndex;
                if (!sameStyle(runs[i], b.style) && !runs[i].text.trimmed().isEmpty())
                    b.mixedStyle = true;
            }
        blocks << b;
    }

    // Stable ids: reading order (top of page = larger y in PDF space).
    std::stable_sort(blocks.begin(), blocks.end(), [](const TextBlock& a, const TextBlock& b) {
        const double ya = a.bounds.bottom(), yb = b.bounds.bottom(); // bottom() == max y for y-up rects
        if (ya != yb)
            return ya > yb;
        return a.bounds.left() < b.bounds.left();
    });
    for (int i = 0; i < blocks.size(); ++i)
        blocks[i].id = i;
    return blocks;
}

} // namespace folvio
