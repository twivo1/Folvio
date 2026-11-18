#pragma once

#include "editor/pdf_native/PdfEditSession.h"

#include <QHash>
#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>

namespace folvio {

// Continuous page view for a PdfDocument. Renders visible pages lazily and
// caches them per zoom level. In interactive mode it highlights text blocks of
// a PdfEditSession and reports clicks in PDF coordinates.
class PdfCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PdfCanvas(QWidget* parent = nullptr);

    using DocProvider = std::function<PdfDocument*()>;
    void setDocumentProvider(DocProvider provider);
    void setSession(PdfEditSession* session) { m_session = session; }
    void setInteractive(bool on) { m_interactive = on; setMouseTracking(on); }
    // Pixel differences against this document are tinted (Compare -> Differences).
    void setReference(DocProvider reference) { m_reference = std::move(reference); invalidateAll(); }
    void setShowDifferences(bool on);

    void invalidatePages(const QSet<int>& pages);
    void invalidateAll();
    void relayout();

    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    double scale() const; // logical pixels per PDF point
    int pageCount() const;
    QRect pageRect(int page) const; // widget coordinates
    int pageAt(const QPoint& p) const;
    int currentPage(const QRect& visible) const;

    QPointF widgetToPdf(int page, const QPointF& p) const;
    QPointF pdfToWidget(int page, const QPointF& p) const;
    QRectF pdfRectToWidget(int page, const QRectF& r) const;

    // Overlays
    void setHighlights(const QHash<int, QList<QRectF>>& pdfRects) { m_highlights = pdfRects; update(); }
    void setLowConfidence(const QHash<int, QList<QRectF>>& pdfRects) { m_lowConf = pdfRects; update(); }
    void setSelectedBlock(int page, int blockId) { m_selPage = page; m_selBlock = blockId; update(); }
    void setHoverEnabled(bool on) { m_hoverEnabled = on; update(); }
    void setCrosshair(bool on) { m_crosshair = on; setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor); }

    QSize sizeHint() const override;

Q_SIGNALS:
    void clicked(int page, const QPointF& pdfPoint, PdfEditSession::Hit hit, Qt::MouseButton button, const QPoint& globalPos);
    void doubleClicked(int page, const QPointF& pdfPoint, PdfEditSession::Hit hit);
    void zoomRequested(int steps);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    QImage pageImage(int page);
    QImage diffImage(int page, const QImage& current);
    QTransform transformFor(int page) const; // PDF -> widget

    DocProvider m_doc;
    DocProvider m_reference;
    QPointer<PdfEditSession> m_session;
    bool m_interactive = false;
    bool m_showDiff = false;
    bool m_hoverEnabled = true;
    bool m_crosshair = false;
    double m_zoom = 1.0;
    QList<QRect> m_rects;               // page rects
    mutable QHash<int, QTransform> m_transforms;
    QHash<int, QImage> m_cache;
    QHash<int, QImage> m_diffCache;
    QList<int> m_lru;
    PdfEditSession::Hit m_hover;
    int m_hoverPage = -1;
    int m_selPage = -1;
    int m_selBlock = -1;
    QHash<int, QList<QRectF>> m_highlights;
    QHash<int, QList<QRectF>> m_lowConf;
};

} // namespace folvio
