#include "editor/pdf_native/PdfTextLayout.h"

#include <QHash>
#include <QRegularExpression>

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

bool PdfTextLayout::isBulletText(const QString& text)
{
    static const QRegularExpression bullet(
        QStringLiteral("^\\s*([\\x{2022}\\x{25CF}\\x{25E6}\\x{25AA}\\x{25AB}\\x{25A0}\\x{25A1}\\x{2023}\\x{2043}\\x{2219}"
                       "\\x{00B7}\\x{2013}\\x{2014}\\-\\*\\+>]|\\(?\\d{1,3}[.)]|\\(?[a-zA-Z][.)])(\\s|$)"));
    return bullet.match(text).hasMatch();
}

namespace {

bool sameStyle(const PdfTextRun& a, const PdfTextRun& b)
{
    return a.fontName == b.fontName && std::abs(a.effectiveSize - b.effectiveSize) < 0.6 && a.color == b.color
           && a.bold == b.bold && a.italic == b.italic;
}

double baselineOf(const PdfTextRun& r) { return r.matrix.dy(); }

// Runs written one after another in the content stream (a few non-text objects,
// e.g. underlines, may sit in between).
bool streamAdjacent(const PdfTextRun& prev, const PdfTextRun& next)
{
    const int d = next.objectIndex - prev.objectIndex;
    return d >= 1 && d <= 4;
}

// Font family without style suffix: "SegoeUI-Bold" -> "segoeui".
QString familyKey(const QString& fontName)
{
    QString f = fontName.toLower();
    const int dash = f.indexOf(QLatin1Char('-'));
    if (dash > 0)
        f = f.left(dash);
    for (const char* suffix : {"bolditalic", "bold", "italic", "oblique", "semibold", "medium", "regular", "mt", "ps"})
        if (f.endsWith(QLatin1String(suffix)) && f.size() > int(qstrlen(suffix)) + 2)
            f.chop(int(qstrlen(suffix)));
    return f;
}

bool isMarkerSymbol(const QString& text)
{
    static const QRegularExpression symbol(
        QStringLiteral("^\\s*([\\x{2022}\\x{25CF}\\x{25E6}\\x{25AA}\\x{25AB}\\x{25A0}\\x{25A1}\\x{25B8}\\x{25B9}\\x{25BA}"
                       "\\x{25B6}\\x{2023}\\x{2043}\\x{2219}\\x{00B7}\\x{2013}\\x{2014}\\x{2192}\\x{27A2}\\x{2713}\\x{2714}"
                       "\\-\\*\\+>]|\\(?\\d{1,3}[.)]|\\(?[a-zA-Z][.)])\\s*$"));
    return symbol.match(text).hasMatch();
}

// Joins runs into line text, inserting spaces at PDFium word boundaries or wide gaps.
QString joinRuns(const QList<PdfTextRun>& runs, const QList<int>& indices)
{
    QString text;
    for (int k = 0; k < indices.size(); ++k) {
        const PdfTextRun& r = runs[indices[k]];
        if (k > 0) {
            const PdfTextRun& prev = runs[indices[k - 1]];
            const double size = std::max(1.0, std::min(r.effectiveSize, prev.effectiveSize));
            const double gap = r.bounds.left() - prev.bounds.right();
            if ((prev.spaceAfter || gap > 0.8 * size) && !text.endsWith(QLatin1Char(' '))
                && !r.text.startsWith(QLatin1Char(' ')))
                text += QLatin1Char(' ');
        }
        text += r.text;
    }
    while (!text.isEmpty() && text.back().isSpace())
        text.chop(1);
    return text;
}

bool isMarkerShape(const QRectF& r)
{
    const double w = r.width(), h = r.height();
    return w >= 1.0 && h >= 1.0 && w <= 9.0 && h <= 9.0 && std::max(w, h) <= 2.2 * std::min(w, h);
}

} // namespace

QList<TextBlock> PdfTextLayout::build(const QList<PdfTextRun>& runs, int page, const QList<QRectF>& rules,
                                      const QList<QRectF>& graphics)
{
    QList<QRectF> markers;
    for (const QRectF& g : graphics)
        if (isMarkerShape(g))
            markers << g;

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
        if (r.text.isEmpty())
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
            b.editable = false;
            b.objectIndices = {r.objectIndex};
            blocks << b;
            continue;
        }
        flow << i;
    }

    // 2. Lines: cluster by baseline, order each cluster by x, split at column boundaries.
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
                // A wide gap is always a column/cell boundary. A moderate gap between runs that are
                // not consecutive in the content stream means two independent text flows side by side.
                const bool columnBreak = gap > 2.5 * size || (gap > 0.6 * size && !streamAdjacent(prev, r));
                if (!columnBreak) {
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
    for (TextLine& l : lines) {
        // A leading marker run (bullet symbol or "1.") followed by a gap is kept out of the editable text.
        bool item = false;
        if (l.runs.size() > 1 && isMarkerSymbol(runs[l.runs.first()].text)) {
            const PdfTextRun& m = runs[l.runs.first()];
            const PdfTextRun& next = runs[l.runs[1]];
            if (next.bounds.left() - m.bounds.right() > 0.12 * std::max(1.0, next.effectiveSize)) {
                l.markerRuns << l.runs.takeFirst();
                l.left = runs[l.runs.first()].matrix.dx();
                l.bounds = runs[l.runs.first()].bounds;
                for (int i : l.runs)
                    l.bounds = l.bounds.united(runs[i].bounds);
                item = true;
            }
        }
        l.text = joinRuns(runs, l.runs);
        // List item: textual bullet/number, or a small marker shape just left of the line.
        item = item || isBulletText(l.text);
        const double size = std::max(1.0, runs[l.runs.first()].effectiveSize);
        for (const QRectF& m : markers) {
            if (item)
                break;
            const double cy = m.center().y();
            item = m.right() <= l.bounds.left() + 0.5 && m.right() > l.bounds.left() - 2.5 * size
                   && cy > l.baseline - 0.3 * size && cy < l.baseline + 0.9 * size;
        }
        l.startsItem = item;
    }

    // 3. Paragraph blocks: consecutive lines with the same style, alignment and regular spacing.
    // Dominant style of a line: the font/size carrying most characters (glyph-per-object
    // producers make "longest run" meaningless).
    auto styleKey = [&](int i) { return runs[i].fontName + QString::number(std::lround(runs[i].effectiveSize * 2)); };
    auto dominantRun = [&](const TextLine& l) -> const PdfTextRun& {
        QHash<QString, int> weight;
        for (int i : l.runs)
            weight[styleKey(i)] += int(runs[i].text.size());
        int best = l.runs.first();
        int bestWeight = -1;
        for (int i : l.runs) {
            const int w = weight.value(styleKey(i));
            if (w > bestWeight) {
                bestWeight = w;
                best = i;
            }
        }
        return runs[best];
    };
    QList<TextBlock> flowBlocks;
    for (const TextLine& l : lines) {
        const PdfTextRun& s = dominantRun(l);
        bool merged = false;
        // A list item always starts a new paragraph.
        for (int bi = int(flowBlocks.size()) - 1; !l.startsItem && bi >= 0 && bi >= int(flowBlocks.size()) - 24 && !merged; --bi) {
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
            // Same font, or another weight/style of the same family when the previous line was
            // wrapped (a bold word that starts the next line of the same paragraph).
            const bool lastWrapped = last.bounds.right() >= b.bounds.right() - 2.0 * size && b.bounds.width() > 8 * size;
            const bool sameFont = s.fontName == b.style.fontName && s.bold == b.style.bold;
            const bool sameFamily = familyKey(s.fontName) == familyKey(b.style.fontName);
            if (sizeOk && pitchOk && overlap && (leftAligned || centered) && (sameFont || (sameFamily && lastWrapped))
                && !ruleBetween(last, l)) {
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
        // Block style: the font/size with most characters across all lines.
        {
            QHash<QString, int> weight;
            for (const TextLine& l : b.lines)
                for (int i : l.runs)
                    weight[styleKey(i)] += int(runs[i].text.size());
            int bestWeight = -1;
            for (const TextLine& l : b.lines)
                for (int i : l.runs)
                    if (weight.value(styleKey(i)) > bestWeight) {
                        bestWeight = weight.value(styleKey(i));
                        b.style = runs[i];
                    }
        }
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
