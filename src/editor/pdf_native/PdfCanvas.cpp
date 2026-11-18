#include "editor/pdf_native/PdfCanvas.h"

#include "ui/themes/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QWheelEvent>

#include <cmath>

namespace folvio {

namespace {
constexpr int kGap = 18;
constexpr int kMargin = 24;
constexpr int kCachedPages = 24;
} // namespace

PdfCanvas::PdfCanvas(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    connect(&Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void PdfCanvas::setDocumentProvider(DocProvider provider)
{
    m_doc = std::move(provider);
    invalidateAll();
}

void PdfCanvas::setShowDifferences(bool on)
{
    m_showDiff = on;
    m_diffCache.clear();
    update();
}

int PdfCanvas::pageCount() const
{
    PdfDocument* d = m_doc ? m_doc() : nullptr;
    return d ? d->pageCount() : 0;
}

double PdfCanvas::scale() const
{
    return m_zoom * 96.0 / 72.0;
}

void PdfCanvas::setZoom(double zoom)
{
    zoom = std::clamp(zoom, 0.1, 6.0);
    if (std::abs(zoom - m_zoom) < 1e-4)
        return;
    m_zoom = zoom;
    m_cache.clear();
    m_diffCache.clear();
    m_lru.clear();
    relayout();
}

void PdfCanvas::invalidatePages(const QSet<int>& pages)
{
    for (int p : pages) {
        m_cache.remove(p);
        m_diffCache.remove(p);
        m_lru.removeAll(p);
    }
    update();
}

void PdfCanvas::invalidateAll()
{
    m_cache.clear();
    m_diffCache.clear();
    m_lru.clear();
    relayout();
}

void PdfCanvas::relayout()
{
    m_rects.clear();
    m_transforms.clear();
    PdfDocument* d = m_doc ? m_doc() : nullptr;
    const int n = d ? d->pageCount() : 0;
    int y = kMargin;
    int maxW = 0;
    const double s = scale();
    QList<QSize> sizes;
    for (int i = 0; i < n; ++i) {
        const QSizeF pt = d->pageSize(i);
        const QSize sz(int(std::lround(pt.width() * s)), int(std::lround(pt.height() * s)));
        sizes << sz;
        maxW = std::max(maxW, sz.width());
    }
    const int totalW = maxW + 2 * kMargin;
    const int avail = parentWidget() ? std::max(totalW, parentWidget()->width()) : totalW;
    for (int i = 0; i < n; ++i) {
        const QSize sz = sizes[i];
        m_rects << QRect((avail - sz.width()) / 2, y, sz.width(), sz.height());
        y += sz.height() + kGap;
    }
    const QSize total(avail, n ? y - kGap + kMargin : 200);
    setMinimumSize(totalW, total.height());
    resize(std::max(avail, totalW), total.height());
    update();
}

QSize PdfCanvas::sizeHint() const
{
    return minimumSize();
}

QRect PdfCanvas::pageRect(int page) const
{
    return page >= 0 && page < m_rects.size() ? m_rects[page] : QRect();
}

int PdfCanvas::pageAt(const QPoint& p) const
{
    for (int i = 0; i < m_rects.size(); ++i)
        if (m_rects[i].adjusted(-4, -kGap / 2, 4, kGap / 2).contains(p))
            return i;
    return -1;
}

int PdfCanvas::currentPage(const QRect& visible) const
{
    int best = 0, bestArea = -1;
    for (int i = 0; i < m_rects.size(); ++i) {
        const QRect inter = m_rects[i].intersected(visible);
        const int area = inter.isEmpty() ? 0 : inter.width() * inter.height();
        if (area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

QTransform PdfCanvas::transformFor(int page) const
{
    auto it = m_transforms.find(page);
    if (it != m_transforms.end())
        return *it;
    PdfDocument* d = m_doc ? m_doc() : nullptr;
    const QRect r = pageRect(page);
    QTransform t;
    if (d && !r.isEmpty())
        t = d->pageToDevice(page, r.size()) * QTransform::fromTranslate(r.x(), r.y());
    m_transforms.insert(page, t);
    return t;
}

QPointF PdfCanvas::pdfToWidget(int page, const QPointF& p) const
{
    return transformFor(page).map(p);
}

QPointF PdfCanvas::widgetToPdf(int page, const QPointF& p) const
{
    return transformFor(page).inverted().map(p);
}

QRectF PdfCanvas::pdfRectToWidget(int page, const QRectF& r) const
{
    return transformFor(page).mapRect(r);
}

QImage PdfCanvas::pageImage(int page)
{
    auto it = m_cache.find(page);
    if (it != m_cache.end()) {
        m_lru.removeAll(page);
        m_lru.append(page);
        return *it;
    }
    PdfDocument* d = m_doc ? m_doc() : nullptr;
    if (!d)
        return {};
    const qreal dpr = devicePixelRatioF();
    QImage img = d->render(page, scale() * dpr);
    img.setDevicePixelRatio(dpr);
    m_cache.insert(page, img);
    m_lru.append(page);
    while (m_lru.size() > kCachedPages) {
        const int evict = m_lru.takeFirst();
        m_cache.remove(evict);
        m_diffCache.remove(evict);
    }
    return img;
}

QImage PdfCanvas::diffImage(int page, const QImage& current)
{
    auto it = m_diffCache.find(page);
    if (it != m_diffCache.end())
        return *it;
    PdfDocument* ref = m_reference ? m_reference() : nullptr;
    QImage overlay;
    if (ref && page < ref->pageCount()) {
        const qreal dpr = devicePixelRatioF();
        const QImage a = ref->render(page, scale() * dpr).convertToFormat(QImage::Format_RGB32);
        const QImage b = current.convertToFormat(QImage::Format_RGB32);
        if (a.size() == b.size()) {
            overlay = QImage(b.size(), QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            const QColor tint = tc().danger;
            const QRgb mark = qPremultiply(qRgba(tint.red(), tint.green(), tint.blue(), 110));
            for (int y = 0; y < b.height(); ++y) {
                const QRgb* ra = reinterpret_cast<const QRgb*>(a.constScanLine(y));
                const QRgb* rb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
                QRgb* o = reinterpret_cast<QRgb*>(overlay.scanLine(y));
                for (int x = 0; x < b.width(); ++x) {
                    const int dlt = std::abs(qRed(ra[x]) - qRed(rb[x])) + std::abs(qGreen(ra[x]) - qGreen(rb[x]))
                                    + std::abs(qBlue(ra[x]) - qBlue(rb[x]));
                    if (dlt > 30)
                        o[x] = mark;
                }
            }
            overlay.setDevicePixelRatio(dpr);
        }
    }
    m_diffCache.insert(page, overlay);
    return overlay;
}

void PdfCanvas::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    const ThemeColors& c = tc();
    p.fillRect(e->rect(), c.sunken);
    if (m_rects.isEmpty())
        return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int i = 0; i < m_rects.size(); ++i) {
        const QRect r = m_rects[i];
        if (!r.adjusted(-6, -6, 6, 6).intersects(e->rect()))
            continue;
        // Subtle shadow: two offset rectangles, no blur effects.
        p.fillRect(r.translated(0, 2).adjusted(-1, 0, 1, 1), QColor(0, 0, 0, Theme::instance().isDark() ? 120 : 28));
        const QImage img = pageImage(i);
        if (img.isNull()) {
            p.fillRect(r, Qt::white);
            continue;
        }
        p.drawImage(r, img);
        if (m_showDiff) {
            const QImage d = diffImage(i, img);
            if (!d.isNull())
                p.drawImage(r, d);
        }
        p.setPen(QPen(c.border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));

        p.setRenderHint(QPainter::Antialiasing);
        // Search highlights
        for (const QRectF& hr : m_highlights.value(i)) {
            const QRectF w = pdfRectToWidget(i, hr).adjusted(-2, -2, 2, 2);
            p.fillRect(w, QColor(255, 196, 0, 70));
            p.setPen(QPen(QColor(214, 150, 0), 1));
            p.drawRect(w);
        }
        // OCR low-confidence words
        for (const QRectF& lr : m_lowConf.value(i)) {
            const QRectF w = pdfRectToWidget(i, lr).adjusted(-1, -1, 1, 1);
            p.fillRect(w, QColor(230, 120, 30, 50));
            p.setPen(QPen(QColor(220, 110, 20), 1, Qt::DashLine));
            p.drawRect(w);
        }
        if (m_session && m_interactive) {
            auto drawBlock = [&](const QRectF& pdfRect, bool selected) {
                const QRectF w = pdfRectToWidget(i, pdfRect).adjusted(-3, -2, 3, 2);
                QColor fill = c.accent;
                fill.setAlpha(selected ? 34 : 18);
                p.fillRect(w, fill);
                p.setPen(QPen(c.accent, selected ? 1.5 : 1.0));
                p.drawRect(w);
            };
            if (m_hoverEnabled && m_hoverPage == i && m_hover.type == PdfEditSession::Hit::Block) {
                const BlockState st = m_session->blockState(i, m_hover.id);
                drawBlock(st.bounds, false);
            } else if (m_hoverEnabled && m_hoverPage == i && m_hover.type == PdfEditSession::Hit::Added) {
                for (const AddedItem& a : m_session->addedItems(i))
                    if (a.addedId == m_hover.id)
                        drawBlock(a.bounds, false);
            }
            if (m_selPage == i && m_selBlock >= 0)
                drawBlock(m_session->blockState(i, m_selBlock).bounds, true);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }
}

void PdfCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_interactive || !m_session)
        return;
    const int page = pageAt(e->pos());
    PdfEditSession::Hit hit;
    if (page >= 0 && m_hoverEnabled)
        hit = m_session->hitTest(page, widgetToPdf(page, e->position()));
    if (hit.type != m_hover.type || hit.id != m_hover.id || page != m_hoverPage) {
        m_hover = hit;
        m_hoverPage = page;
        if (!m_crosshair)
            setCursor(hit.type != PdfEditSession::Hit::None ? Qt::IBeamCursor : Qt::ArrowCursor);
        update();
    }
}

void PdfCanvas::leaveEvent(QEvent*)
{
    if (m_hover.type != PdfEditSession::Hit::None) {
        m_hover = {};
        m_hoverPage = -1;
        update();
    }
}

void PdfCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    const int page = pageAt(e->pos());
    if (page < 0)
        return;
    const QPointF pdf = widgetToPdf(page, e->position());
    PdfEditSession::Hit hit;
    if (m_session && m_interactive)
        hit = m_session->hitTest(page, pdf);
    Q_EMIT clicked(page, pdf, hit, e->button(), e->globalPosition().toPoint());
}

void PdfCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    const int page = pageAt(e->pos());
    if (page < 0)
        return;
    const QPointF pdf = widgetToPdf(page, e->position());
    PdfEditSession::Hit hit;
    if (m_session && m_interactive)
        hit = m_session->hitTest(page, pdf);
    Q_EMIT doubleClicked(page, pdf, hit);
}

void PdfCanvas::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        Q_EMIT zoomRequested(e->angleDelta().y() > 0 ? 1 : -1);
        e->accept();
        return;
    }
    e->ignore(); // let the scroll area scroll
}

} // namespace folvio
