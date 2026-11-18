#include "combine/AnnotationRender.h"

#include "editor/pdf_native/FontMatcher.h"

#include <QFont>
#include <QHash>
#include <QPainter>
#include <QRawFont>

#include <cmath>

namespace folvio::combine {

namespace {

const QRawFont& rawFontFor(const QString& file)
{
    // QRawFont is not shared between threads: one cache per thread (GUI and export worker).
    thread_local QHash<QString, QRawFont> cache;
    auto it = cache.find(file);
    if (it == cache.end())
        it = cache.insert(file, QRawFont(file, 100.0));
    return *it;
}

double distToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    double t = len2 > 0 ? ((p - a).x() * ab.x() + (p - a).y() * ab.y()) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + t * ab;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

QPen penFor(const Annotation& a, double widthScale = 1.0)
{
    QColor c = a.stroke;
    c.setAlphaF(c.alphaF() * a.opacity);
    QPen pen(c, std::max(1e-5, a.width * widthScale));
    pen.setCapStyle(a.type == Annotation::Type::Highlighter ? Qt::SquareCap : Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

} // namespace

QPainterPath AnnotationRender::centerLine(const Annotation& a)
{
    QPainterPath path;
    const QList<QPointF>& pts = a.points;
    if (pts.isEmpty())
        return path;
    path.moveTo(pts.first());
    if (pts.size() == 1) {
        path.lineTo(pts.first() + QPointF(1e-5, 0));
        return path;
    }
    if (a.type == Annotation::Type::Line || a.type == Annotation::Type::Arrow) {
        QPointF end = pts.last();
        if (a.type == Annotation::Type::Arrow) {
            // Stop the shaft inside the head so the line cap does not poke through the tip.
            const QPointF d = pts.last() - pts.first();
            const double len = std::hypot(d.x(), d.y());
            if (len > 1e-9)
                end = pts.last() - d / len * (arrowHeadLength(a) * 0.6);
        }
        path.lineTo(end);
        return path;
    }
    const bool smooth = a.smooth && pts.size() > 2 && a.type != Annotation::Type::Polyline;
    if (!smooth) {
        for (int i = 1; i < pts.size(); ++i)
            path.lineTo(pts[i]);
        return path;
    }
    // Quadratic Bezier through midpoints: removes mouse jitter, keeps the stroke on its points.
    for (int i = 1; i < pts.size() - 1; ++i) {
        const QPointF mid = (pts[i] + pts[i + 1]) / 2.0;
        path.quadTo(pts[i], mid);
    }
    path.lineTo(pts.last());
    return path;
}

double AnnotationRender::arrowHeadLength(const Annotation& a)
{
    return std::max(a.width * 4.0, 0.012);
}

QPainterPath AnnotationRender::arrowHead(const Annotation& a)
{
    QPainterPath head;
    if (a.type != Annotation::Type::Arrow || a.points.size() < 2)
        return head;
    const QPointF tip = a.points.last();
    const QPointF d = a.points.last() - a.points.first();
    const double len = std::hypot(d.x(), d.y());
    if (len < 1e-9)
        return head;
    const QPointF u = d / len, n(-u.y(), u.x());
    const double L = arrowHeadLength(a), W = L * 0.55;
    head.moveTo(tip);
    head.lineTo(tip - u * L + n * W);
    head.lineTo(tip - u * L - n * W);
    head.closeSubpath();
    return head;
}

QPainterPath AnnotationRender::outline(const Annotation& a)
{
    QPainterPath path;
    if (a.type == Annotation::Type::Rect)
        path.addRect(a.rect.normalized());
    else if (a.type == Annotation::Type::Ellipse)
        path.addEllipse(a.rect.normalized());
    return path;
}

QString AnnotationRender::fontFile(const Annotation& a)
{
    FontMatch m = FontMatcher::byFamily(a.fontFamily, a.bold, a.italic);
    if (!m.isValid())
        m = FontMatcher::byFamily(QStringLiteral("Arial"), a.bold, a.italic);
    return m.filePath;
}

double AnnotationRender::textAdvance(const Annotation& a, const QString& text)
{
    const QRawFont& f = rawFontFor(fontFile(a));
    if (!f.isValid())
        return text.size() * a.fontSize * 0.5;
    double w = 0;
    for (const QPointF& adv : f.advancesForGlyphIndexes(f.glyphIndexesForString(text)))
        w += adv.x();
    return w / 100.0 * a.fontSize;
}

QList<TextLineLayout> AnnotationRender::layoutText(const Annotation& a)
{
    QList<TextLineLayout> lines;
    const QRectF box = a.rect.normalized();
    const double size = std::max(1e-4, a.fontSize);
    const double lineHeight = size * 1.25;
    double baseline = box.top() + size * 0.95;
    auto push = [&](const QString& t) {
        TextLineLayout l;
        l.text = t;
        l.width = textAdvance(a, t);
        l.baseline = baseline;
        switch (a.align) {
        case 1: l.x = box.left() + (box.width() - l.width) / 2; break;
        case 2: l.x = box.right() - l.width; break;
        default: l.x = box.left(); break;
        }
        lines << l;
        baseline += lineHeight;
    };
    for (const QString& para : a.text.split(QLatin1Char('\n'))) {
        if (box.width() <= 0) {
            push(para);
            continue;
        }
        QString cur;
        for (const QString& word : para.split(QLatin1Char(' '))) {
            const QString cand = cur.isEmpty() ? word : cur + QLatin1Char(' ') + word;
            if (!cur.isEmpty() && textAdvance(a, cand) > box.width() * 1.001) {
                push(cur);
                cur = word;
            } else {
                cur = cand;
            }
        }
        push(cur);
    }
    return lines;
}

void AnnotationRender::paint(QPainter& p, const Annotation& a)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    switch (a.type) {
    case Annotation::Type::Pen:
    case Annotation::Type::Highlighter:
    case Annotation::Type::Line:
    case Annotation::Type::Polyline:
        if (!a.pressure.isEmpty() && a.pressure.size() == a.points.size() && a.type == Annotation::Type::Pen) {
            // Pressure-sensitive pen: segment widths follow the recorded pressure.
            for (int i = 1; i < a.points.size(); ++i) {
                p.setPen(penFor(a, 0.3 + 0.7 * (a.pressure[i - 1] + a.pressure[i]) / 2.0));
                p.drawLine(a.points[i - 1], a.points[i]);
            }
        } else {
            p.setPen(penFor(a));
            p.setBrush(Qt::NoBrush);
            p.drawPath(centerLine(a));
        }
        break;
    case Annotation::Type::Arrow: {
        p.setPen(penFor(a));
        p.setBrush(Qt::NoBrush);
        p.drawPath(centerLine(a));
        QColor c = a.stroke;
        c.setAlphaF(c.alphaF() * a.opacity);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(arrowHead(a));
        break;
    }
    case Annotation::Type::Rect:
    case Annotation::Type::Ellipse: {
        if (a.hasFill) {
            QColor f = a.fill;
            f.setAlphaF(f.alphaF() * a.opacity);
            p.setBrush(f);
        } else {
            p.setBrush(Qt::NoBrush);
        }
        if (a.width > 0) {
            QPen pen = penFor(a);
            pen.setCapStyle(Qt::SquareCap);
            pen.setJoinStyle(Qt::MiterJoin);
            p.setPen(pen);
        } else {
            p.setPen(Qt::NoPen);
        }
        p.drawPath(outline(a));
        break;
    }
    case Annotation::Type::Text: {
        // Painter units are page units; QFont needs a sensible pixel size, so draw scaled.
        const double unit = 1000.0; // render at 1000 px per page width, then scale down
        p.scale(1.0 / unit, 1.0 / unit);
        QFont f(a.fontFamily);
        f.setPixelSize(std::max(1, int(std::lround(a.fontSize * unit))));
        f.setBold(a.bold);
        f.setItalic(a.italic);
        f.setHintingPreference(QFont::PreferNoHinting);
        p.setFont(f);
        QColor c = a.stroke;
        c.setAlphaF(c.alphaF() * a.opacity);
        p.setPen(c);
        for (const TextLineLayout& l : layoutText(a)) {
            p.drawText(QPointF(l.x * unit, l.baseline * unit), l.text);
            if (a.underline && !l.text.trimmed().isEmpty()) {
                const double y = (l.baseline + a.fontSize * 0.12) * unit;
                p.fillRect(QRectF(l.x * unit, y, l.width * unit, std::max(1.0, a.fontSize * 0.06 * unit)), c);
            }
        }
        break;
    }
    }
    p.restore();
}

void AnnotationRender::paintAll(QPainter& p, const QList<Annotation>& list)
{
    for (const Annotation& a : list)
        paint(p, a);
}

bool AnnotationRender::hitTest(const Annotation& a, const QPointF& pt, double tol)
{
    switch (a.type) {
    case Annotation::Type::Text: return a.rect.normalized().adjusted(-tol, -tol, tol, tol).contains(pt);
    case Annotation::Type::Rect:
    case Annotation::Type::Ellipse: {
        const QRectF r = a.rect.normalized();
        if (a.hasFill) {
            if (a.type == Annotation::Type::Rect)
                return r.adjusted(-tol, -tol, tol, tol).contains(pt);
            return outline(a).contains(pt) || QPainterPathStroker(QPen(Qt::black, a.width + 2 * tol)).createStroke(outline(a)).contains(pt);
        }
        QPainterPathStroker stroker;
        stroker.setWidth(a.width + 2 * tol);
        return stroker.createStroke(outline(a)).contains(pt);
    }
    default: {
        const double reach = a.width / 2 + tol;
        if (a.points.size() == 1)
            return std::hypot(pt.x() - a.points[0].x(), pt.y() - a.points[0].y()) <= reach;
        for (int i = 1; i < a.points.size(); ++i)
            if (distToSegment(pt, a.points[i - 1], a.points[i]) <= reach)
                return true;
        return a.type == Annotation::Type::Arrow && arrowHead(a).contains(pt);
    }
    }
}

QList<Annotation> AnnotationRender::erase(const Annotation& a, const QPointF& pt, double radius, bool* changed)
{
    *changed = false;
    if (!a.isStroke()) {
        if (hitTest(a, pt, radius)) {
            *changed = true;
            return {};
        }
        return {a};
    }
    // Split the stroke where points fall inside the eraser.
    QList<Annotation> parts;
    Annotation cur = a;
    cur.points.clear();
    cur.pressure.clear();
    const double reach = radius + a.width / 2;
    auto flush = [&] {
        if (cur.points.size() >= 2) {
            cur.id = QUuid::createUuid();
            parts << cur;
        }
        cur.points.clear();
        cur.pressure.clear();
    };
    for (int i = 0; i < a.points.size(); ++i) {
        const QPointF& p = a.points[i];
        if (std::hypot(p.x() - pt.x(), p.y() - pt.y()) <= reach) {
            *changed = true;
            flush();
            continue;
        }
        cur.points << p;
        if (i < a.pressure.size())
            cur.pressure << a.pressure[i];
    }
    flush();
    if (!*changed)
        return {a};
    if (parts.size() == 1)
        parts[0].id = a.id;
    return parts;
}

} // namespace folvio::combine
