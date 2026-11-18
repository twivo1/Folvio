#include "combine/PageCanvas.h"

#include "combine/AnnotationRender.h"
#include "ui/themes/Theme.h"

#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTabletEvent>
#include <QTextBlock>
#include <QWheelEvent>

#include <cmath>

namespace folvio::combine {

namespace {
constexpr int kMargin = 28;
constexpr int kPreviewEdge = 2200;
constexpr int kMaxCache = 10;
constexpr double kHandle = 7.0;
const char* kCornerNames[] = {"TL", "TR", "BR", "BL"};

QString geometryKey(const CombinePage& p, ImageProcessor::Stage s)
{
    QJsonObject o = p.toJson();
    o.remove("annotations");
    o.remove("name");
    if (s == ImageProcessor::Stage::Rotated) {
        o.remove("quad");
        o.remove("crop");
        o.remove("aspect");
        o.remove("margin");
        o.remove("customRatio");
    } else if (s == ImageProcessor::Stage::Rectified) {
        o.remove("crop");
    }
    return QString::number(int(s)) + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QRectF handleRect(const QPointF& c, double r = kHandle) { return QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r); }

QList<QPointF> handlePoints(const QRectF& r)
{
    return {r.topLeft(), QPointF(r.center().x(), r.top()), r.topRight(), QPointF(r.right(), r.center().y()),
            r.bottomRight(), QPointF(r.center().x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), r.center().y())};
}

QRectF resizeRect(const QRectF& start, int handle, const QPointF& d)
{
    QRectF r = start;
    switch (handle) {
    case 0: r.setTopLeft(r.topLeft() + d); break;
    case 1: r.setTop(r.top() + d.y()); break;
    case 2: r.setTopRight(r.topRight() + d); break;
    case 3: r.setRight(r.right() + d.x()); break;
    case 4: r.setBottomRight(r.bottomRight() + d); break;
    case 5: r.setBottom(r.bottom() + d.y()); break;
    case 6: r.setBottomLeft(r.bottomLeft() + d); break;
    case 7: r.setLeft(r.left() + d.x()); break;
    default: r.translate(d); break;
    }
    return r.normalized();
}

} // namespace

// ---------------------------------------------------------------------------

CombineTextEditor::CombineTextEditor(QWidget* parent) : QPlainTextEdit(parent)
{
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    document()->setDocumentMargin(0);
}

void CombineTextEditor::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        Q_EMIT cancelRequested();
        return;
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier)) {
        Q_EMIT commitRequested();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

// ---------------------------------------------------------------------------

PageCanvas::PageCanvas(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TabletTracking);
    m_textEdit = new CombineTextEditor(this);
    m_textEdit->hide();
    connect(m_textEdit, &CombineTextEditor::commitRequested, this, &PageCanvas::commitText);
    connect(m_textEdit, &CombineTextEditor::cancelRequested, this, [this] {
        m_textTarget.reset();
        m_textEdit->hide();
        setFocus();
        update();
    });
    connect(m_textEdit, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_textTarget)
            return;
        const QFontMetrics fm(m_textEdit->font());
        const int lines = std::max(1, m_textEdit->document()->lineCount());
        m_textEdit->setFixedHeight(lines * fm.lineSpacing() + fm.descent() + 6);
    });
    connect(&Theme::instance(), &Theme::changed, this, qOverload<>(&QWidget::update));
}

void PageCanvas::setDocument(CombineDocument* doc)
{
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    connect(doc, &CombineDocument::pageChanged, this, [this](const QUuid& id) {
        if (id != m_pageId)
            return;
        if (m_selectedId != QUuid() && page()) {
            bool found = false;
            for (const Annotation& a : page()->annotations)
                found = found || a.id == m_selectedId;
            if (!found)
                m_selectedId = QUuid();
        }
        relayout();
        Q_EMIT selectionChanged();
    });
    connect(doc, &CombineDocument::pagesChanged, this, [this] {
        if (!page())
            setPage(m_doc->count() ? m_doc->page(0).id : QUuid());
        else
            relayout();
    });
}

const CombinePage* PageCanvas::page() const
{
    if (!m_doc)
        return nullptr;
    const int i = m_doc->indexOf(m_pageId);
    return i >= 0 ? &m_doc->page(i) : nullptr;
}

void PageCanvas::setPage(const QUuid& id)
{
    if (isEditingText())
        commitText();
    m_working.reset();
    m_workingListActive = false;
    m_pageId = id;
    m_selectedId = QUuid();
    m_correctedKey.clear();
    if (const CombinePage* p = page()) {
        m_quad = p->quad;
        m_perspective = p->perspective;
        m_crop = p->crop.value_or(QRectF(0, 0, 1, 1));
        m_compare = p->quad ? CompareMode::Corrected : CompareMode::Original;
    } else {
        m_quad.reset();
    }
    if (m_tool == Tool::Perspective)
        initPendingQuad();
    relayout();
    Q_EMIT selectionChanged();
    Q_EMIT perspectiveChanged();
    Q_EMIT cropChanged();
}

void PageCanvas::setTool(Tool t)
{
    if (isEditingText())
        commitText();
    if (m_working && m_working->type == Annotation::Type::Polyline && m_working->points.size() >= 2)
        commitWorking(tr("Draw polyline"));
    m_working.reset();
    m_tool = t;
    if (t == Tool::Perspective)
        initPendingQuad();
    if (t == Tool::Perspective && page() && page()->quad)
        m_compare = CompareMode::Original; // corners are adjusted on the original photo
    if (t == Tool::Crop && page())
        m_crop = page()->crop.value_or(QRectF(0, 0, 1, 1));
    if (t != Tool::Select)
        m_selectedId = QUuid();
    setCursor(t == Tool::Select ? Qt::ArrowCursor : t == Tool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
    relayout();
    Q_EMIT selectionChanged();
    Q_EMIT perspectiveChanged();
}

// ---- images ---------------------------------------------------------------------

QImage PageCanvas::stageImage(ImageProcessor::Stage stage) const
{
    const CombinePage* p = page();
    if (!p)
        return {};
    const QString key = geometryKey(*p, stage);
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return *it;
    QString err;
    QImage img = ImageProcessor::render(*p, stage, kPreviewEdge, &err);
    if (img.isNull())
        return {};
    m_cache.insert(key, img);
    m_cacheOrder << key;
    while (m_cacheOrder.size() > kMaxCache)
        m_cache.remove(m_cacheOrder.takeFirst());
    return img;
}

QSize PageCanvas::rotatedSourceSize() const
{
    const CombinePage* p = page();
    return p ? Geometry::rotatedSize(p->sourceSize, p->rotation) : QSize();
}

QImage PageCanvas::correctedPreview() const
{
    if (!m_quad || !page())
        return {};
    const QImage rotated = stageImage(ImageProcessor::Stage::Rotated);
    const QSize full = rotatedSourceSize();
    if (rotated.isNull() || full.width() <= 0)
        return {};
    const QString key = QString::fromUtf8(QJsonDocument(QJsonObject{{"q", m_quad->toJson()}, {"a", int(m_perspective.aspect)},
                                                                    {"m", m_perspective.marginPercent},
                                                                    {"r", m_perspective.customRatio},
                                                                    {"id", m_pageId.toString()}})
                                              .toJson(QJsonDocument::Compact));
    if (key == m_correctedKey)
        return m_correctedPreview;
    const double s = double(rotated.width()) / full.width();
    QString err;
    m_correctedPreview = ImageProcessor::rectify(rotated, Geometry::scaled(*m_quad, s), m_perspective, &err);
    m_correctedKey = key;
    return m_correctedPreview;
}

QImage PageCanvas::displayImage() const
{
    switch (m_tool) {
    case Tool::Perspective: return stageImage(ImageProcessor::Stage::Rotated);
    case Tool::Crop: return stageImage(ImageProcessor::Stage::Rectified);
    default: return stageImage(ImageProcessor::Stage::Final);
    }
}

void PageCanvas::invalidate()
{
    m_cache.clear();
    m_cacheOrder.clear();
    m_correctedKey.clear();
}

void PageCanvas::refreshPerspectivePreview()
{
    m_correctedKey.clear();
    update();
}

void PageCanvas::relayout()
{
    const QImage img = displayImage();
    m_imageSize = img.size();
    QSize total = m_imageSize * m_zoom;
    if (m_tool == Tool::Perspective && m_compare == CompareMode::Split) {
        const QImage c = correctedPreview();
        if (!c.isNull() && c.height() > 0)
            total.rwidth() += kMargin + int(c.width() * double(m_imageSize.height()) / c.height() * m_zoom);
    }
    if (m_tool == Tool::Perspective && m_compare == CompareMode::Corrected) {
        const QImage c = correctedPreview();
        if (!c.isNull())
            total = c.size() * m_zoom;
    }
    const QSize s(total.width() + 2 * kMargin, total.height() + 2 * kMargin);
    setMinimumSize(s);
    resize(s.expandedTo(parentWidget() ? parentWidget()->size() : s));
    update();
    if (m_textTarget && m_textEdit->isVisible()) {
        const QPointF tl = fromPage(m_textTarget->rect.topLeft());
        m_textEdit->move(tl.toPoint());
    }
}

QSize PageCanvas::sizeHint() const { return minimumSize(); }

void PageCanvas::setZoom(double z)
{
    z = std::clamp(z, 0.05, 8.0);
    if (std::abs(z - m_zoom) < 1e-4)
        return;
    if (isEditingText())
        commitText();
    m_zoom = z;
    relayout();
    Q_EMIT zoomChanged(m_zoom);
}

void PageCanvas::fitToView(const QSize& vp)
{
    QImage img = displayImage();
    if (m_tool == Tool::Perspective && m_compare == CompareMode::Corrected && !correctedPreview().isNull())
        img = correctedPreview();
    if (img.isNull())
        return;
    double w = img.width(), h = img.height();
    if (m_tool == Tool::Perspective && m_compare == CompareMode::Split) {
        const QImage c = correctedPreview();
        if (!c.isNull())
            w += c.width() * h / c.height() + kMargin;
    }
    const double z = std::min((vp.width() - 2.0 * kMargin - 4) / w, (vp.height() - 2.0 * kMargin - 4) / h);
    m_zoom = -1; // force update
    setZoom(std::max(0.05, z));
}

QRectF PageCanvas::imageRect() const
{
    const QRectF r(kMargin, kMargin, m_imageSize.width() * m_zoom, m_imageSize.height() * m_zoom);
    // Center horizontally when the widget is wider than the content.
    const double extra = width() - minimumWidth();
    return r.translated(std::max(0.0, extra / 2), 0);
}

QPointF PageCanvas::toPage(const QPointF& w) const
{
    const QRectF r = imageRect();
    return r.width() > 0 ? QPointF((w.x() - r.left()) / r.width(), (w.y() - r.top()) / r.width()) : QPointF();
}

QPointF PageCanvas::fromPage(const QPointF& u) const
{
    const QRectF r = imageRect();
    return QPointF(r.left() + u.x() * r.width(), r.top() + u.y() * r.width());
}

QPointF PageCanvas::fullToWidget(const QPointF& p) const
{
    const QSize full = rotatedSourceSize();
    const QRectF r = imageRect();
    if (full.width() <= 0)
        return r.topLeft();
    const double k = r.width() / full.width();
    return r.topLeft() + p * k;
}

QPointF PageCanvas::widgetToFull(const QPointF& w) const
{
    const QSize full = rotatedSourceSize();
    const QRectF r = imageRect();
    if (r.width() <= 0)
        return {};
    return (w - r.topLeft()) * (full.width() / r.width());
}

double PageCanvas::pageUnitsPerPixel() const
{
    const QRectF r = imageRect();
    return r.width() > 0 ? 1.0 / r.width() : 0.001;
}

// ---- perspective ----------------------------------------------------------------

QuadCheck PageCanvas::pendingQuadCheck() const
{
    if (!m_quad) {
        QuadCheck c;
        c.error = tr("No corners set.");
        return c;
    }
    return Geometry::validate(*m_quad, rotatedSourceSize());
}

void PageCanvas::selectCorner(int c)
{
    m_corner = std::clamp(c, 0, 3);
    update();
    Q_EMIT perspectiveChanged();
}

void PageCanvas::setCornerPosition(int c, const QPointF& p)
{
    if (!m_quad || c < 0 || c > 3)
        return;
    const QSize s = rotatedSourceSize();
    QPointF q(std::clamp(p.x(), -0.02 * s.width(), 1.02 * s.width()), std::clamp(p.y(), -0.02 * s.height(), 1.02 * s.height()));
    m_quad->pts[size_t(c)] = q;
    m_correctedKey.clear();
    update();
    Q_EMIT perspectiveChanged();
}

void PageCanvas::initPendingQuad()
{
    const CombinePage* p = page();
    if (!p || m_quad)
        return;
    if (p->quad) {
        m_quad = p->quad;
        return;
    }
    // First visit: propose the detected page outline; the corners stay editable.
    const QImage img = stageImage(ImageProcessor::Stage::Rotated);
    const QSize full = rotatedSourceSize();
    if (!img.isNull() && full.width() > 0) {
        if (const auto q = ImageProcessor::detectPage(img)) {
            m_quad = Geometry::scaled(*q, double(full.width()) / img.width());
            Q_EMIT message(tr("Page outline detected automatically. Check the corners, then choose Straighten."), false);
            return;
        }
    }
    m_quad = Geometry::defaultQuad(full);
}

bool PageCanvas::detectPage()
{
    const QImage img = stageImage(ImageProcessor::Stage::Rotated);
    const QSize full = rotatedSourceSize();
    if (img.isNull() || full.width() <= 0)
        return false;
    const auto q = ImageProcessor::detectPage(img);
    if (!q) {
        Q_EMIT message(tr("The page outline could not be detected. Drag the four corners onto the page corners."), true);
        return false;
    }
    m_quad = Geometry::scaled(*q, double(full.width()) / img.width());
    m_correctedKey.clear();
    update();
    Q_EMIT perspectiveChanged();
    Q_EMIT message(tr("Page outline detected. Check the corners, then choose Straighten."), false);
    return true;
}

void PageCanvas::resetQuad()
{
    if (!page())
        return;
    m_quad = Geometry::defaultQuad(rotatedSourceSize());
    m_correctedKey.clear();
    update();
    Q_EMIT perspectiveChanged();
}

void PageCanvas::autoOrderCorners()
{
    if (!m_quad)
        return;
    m_quad = Geometry::orderCorners(m_quad->pts);
    m_correctedKey.clear();
    update();
    Q_EMIT perspectiveChanged();
}

bool PageCanvas::applyPerspective(QString* error)
{
    const CombinePage* p = page();
    if (!p || !m_quad)
        return false;
    const QuadCheck c = pendingQuadCheck();
    if (!c.ok) {
        if (error)
            *error = c.error;
        return false;
    }
    CombinePage copy = *p;
    copy.quad = m_quad;
    copy.perspective = m_perspective;
    // A crop refers to the previous rectified image; it no longer applies.
    copy.crop.reset();
    m_doc->updatePage(copy, tr("Straighten page"));
    m_crop = QRectF(0, 0, 1, 1);
    m_compare = CompareMode::Corrected;
    relayout();
    Q_EMIT perspectiveChanged();
    return true;
}

void PageCanvas::removePerspective()
{
    const CombinePage* p = page();
    if (!p || !p->quad)
        return;
    CombinePage copy = *p;
    copy.quad.reset();
    copy.crop.reset();
    m_doc->updatePage(copy, tr("Remove perspective correction"));
    m_compare = CompareMode::Original;
    relayout();
    Q_EMIT perspectiveChanged();
}

void PageCanvas::setCompareMode(CompareMode m)
{
    m_compare = m;
    relayout();
}

// ---- crop -------------------------------------------------------------------------

void PageCanvas::applyCrop()
{
    const CombinePage* p = page();
    if (!p)
        return;
    CombinePage copy = *p;
    const QRectF unit(0, 0, 1, 1);
    const QRectF c = m_crop.normalized().intersected(unit);
    if (c.width() < 0.02 || c.height() < 0.02) {
        Q_EMIT message(tr("The crop area is too small."), true);
        return;
    }
    if (std::abs(c.width() - 1) < 1e-6 && std::abs(c.height() - 1) < 1e-6)
        copy.crop.reset();
    else
        copy.crop = c;
    m_doc->updatePage(copy, tr("Crop page"));
    Q_EMIT cropChanged();
    Q_EMIT message(tr("Crop applied. Annotations keep their position on the page."), false);
}

void PageCanvas::resetCrop()
{
    m_crop = QRectF(0, 0, 1, 1);
    const CombinePage* p = page();
    if (p && p->crop) {
        CombinePage copy = *p;
        copy.crop.reset();
        m_doc->updatePage(copy, tr("Reset crop"));
    }
    update();
    Q_EMIT cropChanged();
}

void PageCanvas::rotatePage(int degrees)
{
    const CombinePage* p = page();
    if (!p)
        return;
    CombinePage copy = *p;
    const QSize before = Geometry::rotatedSize(copy.sourceSize, copy.rotation);
    copy.rotation = ((copy.rotation + degrees) % 360 + 360) % 360;
    if (copy.quad)
        copy.quad = Geometry::rotated(*copy.quad, before, degrees);
    copy.crop.reset();
    m_doc->updatePage(copy, tr("Rotate page"));
    m_quad = copy.quad;
    if (m_tool == Tool::Perspective && !m_quad)
        m_quad = Geometry::defaultQuad(rotatedSourceSize());
    m_crop = QRectF(0, 0, 1, 1);
    relayout();
}

// ---- selection ------------------------------------------------------------------

std::optional<Annotation> PageCanvas::selected() const
{
    if (!page() || m_selectedId.isNull())
        return std::nullopt;
    for (const Annotation& a : page()->annotations)
        if (a.id == m_selectedId)
            return a;
    return std::nullopt;
}

void PageCanvas::updateSelected(const Annotation& a, const QString& description)
{
    const CombinePage* p = page();
    if (!p)
        return;
    CombinePage copy = *p;
    for (Annotation& x : copy.annotations)
        if (x.id == a.id)
            x = a;
    m_doc->updatePage(copy, description);
}

void PageCanvas::deleteSelection()
{
    const CombinePage* p = page();
    if (!p || m_selectedId.isNull())
        return;
    CombinePage copy = *p;
    copy.annotations.erase(std::remove_if(copy.annotations.begin(), copy.annotations.end(),
                                          [this](const Annotation& a) { return a.id == m_selectedId; }),
                           copy.annotations.end());
    m_selectedId = QUuid();
    m_doc->updatePage(copy, tr("Delete annotation"));
    Q_EMIT selectionChanged();
}

void PageCanvas::duplicateSelection()
{
    const auto sel = selected();
    if (!sel)
        return;
    CombinePage copy = *page();
    Annotation d = *sel;
    d.id = QUuid::createUuid();
    d.translate(QPointF(0.02, 0.02));
    copy.annotations << d;
    m_selectedId = d.id;
    m_doc->updatePage(copy, tr("Duplicate annotation"));
    Q_EMIT selectionChanged();
}

void PageCanvas::raiseSelection(int delta)
{
    const CombinePage* p = page();
    if (!p || m_selectedId.isNull())
        return;
    CombinePage copy = *p;
    for (int i = 0; i < copy.annotations.size(); ++i)
        if (copy.annotations[i].id == m_selectedId) {
            const int j = std::clamp(i + delta, 0, int(copy.annotations.size()) - 1);
            if (i != j) {
                copy.annotations.move(i, j);
                m_doc->updatePage(copy, delta > 0 ? tr("Bring forward") : tr("Send backward"));
            }
            return;
        }
}

void PageCanvas::clearSelection()
{
    m_selectedId = QUuid();
    update();
    Q_EMIT selectionChanged();
}

QRectF PageCanvas::selectionRectWidget() const
{
    const auto sel = selected();
    if (!sel)
        return {};
    const QRectF b = sel->bounds();
    return QRectF(fromPage(b.topLeft()), fromPage(b.bottomRight())).normalized().adjusted(-4, -4, 4, 4);
}

int PageCanvas::handleAt(const QPointF& w, const QRectF& rect) const
{
    const QList<QPointF> hs = handlePoints(rect);
    for (int i = 0; i < hs.size(); ++i)
        if (handleRect(hs[i], kHandle + 3).contains(w))
            return i;
    return -1;
}

// ---- text -----------------------------------------------------------------------

bool PageCanvas::isEditingText() const
{
    return m_textEdit && m_textEdit->isVisible() && m_textTarget.has_value();
}

void PageCanvas::startTextEdit(const std::optional<Annotation>& existing, const QPointF& pageTopLeft)
{
    Annotation a;
    if (existing) {
        a = *existing;
    } else {
        a.type = Annotation::Type::Text;
        a.stroke = m_style.stroke;
        a.opacity = m_style.opacity;
        a.fontFamily = m_style.fontFamily;
        a.fontSize = m_style.fontSize;
        a.bold = m_style.bold;
        a.italic = m_style.italic;
        a.underline = m_style.underline;
        a.align = m_style.align;
        const double maxW = std::max(0.1, 1.0 - pageTopLeft.x() - 0.02);
        a.rect = QRectF(pageTopLeft, QSizeF(std::min(0.45, maxW), a.fontSize * 1.4));
    }
    m_textTarget = a;
    const QRectF r = imageRect();
    QFont f(a.fontFamily);
    f.setPixelSize(std::max(4, int(std::lround(a.fontSize * r.width()))));
    f.setBold(a.bold);
    f.setItalic(a.italic);
    f.setUnderline(a.underline);
    m_textEdit->setFont(f);
    QColor c = a.stroke;
    m_textEdit->setStyleSheet(QStringLiteral("QPlainTextEdit { background: rgba(255,255,255,200); color: %1; border: 1px dashed %2;"
                                             " font-family: \"%3\"; font-size: %4px; font-weight: %5; font-style: %6; }")
                                  .arg(c.name(), tc().accent.name(), a.fontFamily)
                                  .arg(f.pixelSize())
                                  .arg(a.bold ? 700 : 400)
                                  .arg(a.italic ? "italic" : "normal"));
    m_textEdit->setPlainText(a.text);
    QTextOption opt = m_textEdit->document()->defaultTextOption();
    opt.setAlignment(a.align == 1 ? Qt::AlignHCenter : a.align == 2 ? Qt::AlignRight : Qt::AlignLeft);
    m_textEdit->document()->setDefaultTextOption(opt);
    const QPointF tl = fromPage(a.rect.topLeft());
    m_textEdit->setGeometry(QRect(tl.toPoint(), QSize(std::max(40, int(a.rect.width() * r.width())), 10)));
    Q_EMIT m_textEdit->textChanged();
    m_textEdit->show();
    m_textEdit->setFocus();
    m_textEdit->moveCursor(QTextCursor::End);
    m_selectedId = a.id;
    update();
}

void PageCanvas::commitText()
{
    if (!m_textTarget || !page()) {
        m_textEdit->hide();
        return;
    }
    Annotation a = *m_textTarget;
    m_textTarget.reset();
    m_textEdit->hide();
    a.text = m_textEdit->toPlainText();
    CombinePage copy = *page();
    const auto it = std::find_if(copy.annotations.begin(), copy.annotations.end(), [&](const Annotation& x) { return x.id == a.id; });
    if (a.text.trimmed().isEmpty()) {
        if (it != copy.annotations.end()) {
            copy.annotations.erase(it);
            m_selectedId = QUuid();
            m_doc->updatePage(copy, tr("Delete text"));
        }
        setFocus();
        return;
    }
    const int lines = int(AnnotationRender::layoutText(a).size());
    a.rect.setHeight(lines * a.fontSize * 1.25 + a.fontSize * 0.35);
    if (it != copy.annotations.end()) {
        if (it->text == a.text && it->rect == a.rect)
            return;
        *it = a;
        m_doc->updatePage(copy, tr("Edit text"));
    } else {
        copy.annotations << a;
        m_doc->updatePage(copy, tr("Add text"));
    }
    m_selectedId = a.id;
    setFocus();
    Q_EMIT selectionChanged();
}

// ---- painting -------------------------------------------------------------------

void PageCanvas::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    const ThemeColors& c = tc();
    p.fillRect(e->rect(), c.sunken);
    if (!page()) {
        p.setPen(c.textMuted);
        p.drawText(rect(), Qt::AlignCenter, tr("No page selected"));
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = imageRect();
    auto shadow = [&](const QRectF& rr) {
        p.fillRect(rr.translated(0, 2).adjusted(-1, 0, 1, 1), QColor(0, 0, 0, Theme::instance().isDark() ? 120 : 30));
    };

    if (m_tool == Tool::Perspective && m_compare == CompareMode::Corrected) {
        const QImage corr = correctedPreview();
        if (!corr.isNull()) {
            const QRectF cr(r.topLeft(), QSizeF(corr.width() * m_zoom, corr.height() * m_zoom));
            shadow(cr);
            p.drawImage(cr, corr);
            p.setPen(c.textMuted);
            p.drawText(QPointF(cr.left(), cr.top() - 8), tr("Corrected preview"));
            return;
        }
    }
    const QImage img = displayImage();
    shadow(r);
    p.drawImage(r, img);

    if (m_tool == Tool::Perspective) {
        if (m_compare == CompareMode::Split) {
            const QImage corr = correctedPreview();
            if (!corr.isNull()) {
                const double h = r.height();
                const QRectF cr(r.right() + kMargin, r.top(), corr.width() * h / corr.height(), h);
                shadow(cr);
                p.drawImage(cr, corr);
                p.setPen(c.textMuted);
                p.drawText(QPointF(r.left(), r.top() - 8), tr("Original"));
                p.drawText(QPointF(cr.left(), cr.top() - 8), tr("Corrected"));
            }
        }
        if (m_quad) {
            const bool ok = pendingQuadCheck().ok;
            const QColor line = ok ? c.accent : c.danger;
            QPolygonF poly;
            for (const QPointF& q : m_quad->pts)
                poly << fullToWidget(q);
            // Dim everything outside the page.
            QPainterPath outside;
            outside.addRect(r);
            QPainterPath inside;
            inside.addPolygon(poly);
            inside.closeSubpath();
            p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 70));
            p.setPen(QPen(line, 2));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(poly);
            QFont f = font();
            f.setBold(true);
            f.setPointSizeF(8);
            p.setFont(f);
            for (int i = 0; i < 4; ++i) {
                const QPointF w = poly[i];
                const bool sel = i == m_corner;
                p.setPen(QPen(Qt::white, 2));
                p.setBrush(sel ? line : QColor(line.red(), line.green(), line.blue(), 200));
                p.drawEllipse(w, sel ? 11.0 : 9.0, sel ? 11.0 : 9.0);
                p.setPen(Qt::white);
                p.drawText(QRectF(w.x() - 11, w.y() - 11, 22, 22), Qt::AlignCenter, QString::fromLatin1(kCornerNames[i]));
            }
        }
        return;
    }

    if (m_tool == Tool::Crop) {
        const QRectF cw(QPointF(r.left() + m_crop.left() * r.width(), r.top() + m_crop.top() * r.height()),
                        QPointF(r.left() + m_crop.right() * r.width(), r.top() + m_crop.bottom() * r.height()));
        QPainterPath outside;
        outside.addRect(r);
        QPainterPath inside;
        inside.addRect(cw);
        p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 120));
        p.setPen(QPen(c.accent, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cw);
        p.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DashLine));
        for (int k = 1; k < 3; ++k) {
            p.drawLine(QPointF(cw.left() + cw.width() * k / 3, cw.top()), QPointF(cw.left() + cw.width() * k / 3, cw.bottom()));
            p.drawLine(QPointF(cw.left(), cw.top() + cw.height() * k / 3), QPointF(cw.right(), cw.top() + cw.height() * k / 3));
        }
        p.setPen(QPen(Qt::white, 1.5));
        p.setBrush(c.accent);
        for (const QPointF& h : handlePoints(cw))
            p.drawRect(handleRect(h, 5));
        return;
    }

    // Annotations in page units.
    p.save();
    p.translate(r.topLeft());
    p.scale(r.width(), r.width());
    p.setClipRect(QRectF(0, 0, 1, r.height() / r.width()));
    const QList<Annotation>& list = m_workingListActive ? m_workingList : page()->annotations;
    for (const Annotation& a : list) {
        if (m_textTarget && a.id == m_textTarget->id && m_textEdit->isVisible())
            continue; // shown in the editor
        AnnotationRender::paint(p, a);
    }
    if (m_working)
        AnnotationRender::paint(p, *m_working);
    p.restore();

    if (m_tool == Tool::Select && !m_selectedId.isNull() && !isEditingText()) {
        QRectF sr;
        if (m_workingListActive) {
            for (const Annotation& a : m_workingList)
                if (a.id == m_selectedId)
                    sr = QRectF(fromPage(a.bounds().topLeft()), fromPage(a.bounds().bottomRight())).normalized().adjusted(-4, -4, 4, 4);
        } else {
            sr = selectionRectWidget();
        }
        if (!sr.isNull()) {
            p.setPen(QPen(c.accent, 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(sr);
            p.setPen(QPen(Qt::white, 1.2));
            p.setBrush(c.accent);
            for (const QPointF& h : handlePoints(sr))
                p.drawRect(handleRect(h, 4.5));
        }
    }
}

// ---- input ----------------------------------------------------------------------

void PageCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    if (m_tabletActive)
        return;
    pressAt(e->position(), 1.0, e->button(), e->modifiers());
}

void PageCanvas::mouseMoveEvent(QMouseEvent* e)
{
    if (m_tabletActive)
        return;
    if (m_tool == Tool::Perspective && m_quad && !(e->buttons() & Qt::LeftButton)) {
        bool over = false;
        for (const QPointF& q : m_quad->pts)
            over = over || handleRect(fullToWidget(q), 14).contains(e->position());
        setCursor(over ? Qt::SizeAllCursor : Qt::CrossCursor);
    }
    moveTo(e->position(), 1.0);
}

void PageCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_tabletActive)
        return;
    if (e->button() == Qt::LeftButton)
        releaseAt(e->position());
}

void PageCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (m_tool == Tool::Shape && m_working && m_working->type == Annotation::Type::Polyline) {
        if (m_working->points.size() > 2)
            m_working->points.removeLast(); // the double click added a duplicate point
        commitWorking(tr("Draw polyline"));
        return;
    }
    if (m_tool == Tool::Select || m_tool == Tool::Text) {
        const auto sel = selected();
        if (sel && sel->type == Annotation::Type::Text)
            startTextEdit(sel, sel->rect.topLeft());
    }
    QWidget::mouseDoubleClickEvent(e);
}

void PageCanvas::tabletEvent(QTabletEvent* e)
{
    // Pen pressure for the Pen tool; other tools use the synthesized mouse events.
    if (m_tool != Tool::Pen && m_tool != Tool::Highlighter) {
        e->ignore();
        return;
    }
    const double pressure = std::clamp(double(e->pressure()), 0.05, 1.0);
    switch (e->type()) {
    case QEvent::TabletPress:
        m_tabletActive = true;
        pressAt(e->position(), pressure, Qt::LeftButton, e->modifiers());
        break;
    case QEvent::TabletMove: moveTo(e->position(), pressure); break;
    case QEvent::TabletRelease:
        releaseAt(e->position());
        m_tabletActive = false;
        break;
    default: break;
    }
    e->accept();
}

void PageCanvas::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        setZoom(m_zoom * (e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15));
        e->accept();
        return;
    }
    e->ignore();
}

bool PageCanvas::focusNextPrevChild(bool next)
{
    // In the perspective tool Tab cycles through the four corners.
    if (m_tool == Tool::Perspective && m_quad && hasFocus()) {
        selectCorner((m_corner + (next ? 1 : 3)) % 4);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

void PageCanvas::pressAt(const QPointF& pos, double pressure, Qt::MouseButton button, Qt::KeyboardModifiers)
{
    const CombinePage* p = page();
    if (!p)
        return;
    if (isEditingText())
        commitText();
    const QPointF u = toPage(pos);

    switch (m_tool) {
    case Tool::Perspective: {
        if (!m_quad || button != Qt::LeftButton)
            return;
        int best = -1;
        double bestD = 1e9;
        for (int i = 0; i < 4; ++i) {
            const QPointF w = fullToWidget(m_quad->pts[size_t(i)]);
            const double d = std::hypot(w.x() - pos.x(), w.y() - pos.y());
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        if (best >= 0 && bestD < 24) {
            m_corner = best;
            m_draggingCorner = true;
            Q_EMIT perspectiveChanged();
        }
        update();
        return;
    }
    case Tool::Crop: {
        const QRectF r = imageRect();
        const QRectF cw(QPointF(r.left() + m_crop.left() * r.width(), r.top() + m_crop.top() * r.height()),
                        QPointF(r.left() + m_crop.right() * r.width(), r.top() + m_crop.bottom() * r.height()));
        m_cropHandle = handleAt(pos, cw);
        if (m_cropHandle < 0)
            m_cropHandle = cw.contains(pos) ? 8 : 9;
        m_cropAnchor = QPointF((pos.x() - r.left()) / r.width(), (pos.y() - r.top()) / r.height());
        m_cropStart = m_crop;
        if (m_cropHandle == 9)
            m_crop = QRectF(m_cropAnchor, QSizeF(0, 0));
        return;
    }
    case Tool::Pen:
    case Tool::Highlighter: {
        Annotation a;
        a.type = m_tool == Tool::Pen ? Annotation::Type::Pen : Annotation::Type::Highlighter;
        a.stroke = m_style.stroke;
        a.width = m_style.width;
        a.opacity = m_style.opacity;
        a.smooth = m_style.smooth;
        a.points << u;
        if (m_tabletActive && a.type == Annotation::Type::Pen)
            a.pressure << pressure;
        m_working = a;
        update();
        return;
    }
    case Tool::Shape: {
        if (m_working && m_working->type == Annotation::Type::Polyline) {
            if (button == Qt::RightButton) {
                m_working->points.removeLast();
                commitWorking(tr("Draw polyline"));
            } else {
                m_working->points << u; // next vertex (last point follows the mouse)
            }
            update();
            return;
        }
        if (button != Qt::LeftButton)
            return;
        Annotation a;
        a.type = m_shapeType;
        a.stroke = m_style.stroke;
        a.width = m_style.width;
        a.opacity = m_style.opacity;
        a.fill = m_style.fill;
        a.hasFill = m_style.hasFill && (m_shapeType == Annotation::Type::Rect || m_shapeType == Annotation::Type::Ellipse);
        a.smooth = false;
        if (m_shapeType == Annotation::Type::Rect || m_shapeType == Annotation::Type::Ellipse)
            a.rect = QRectF(u, QSizeF(0, 0));
        else
            a.points = {u, u};
        m_working = a;
        return;
    }
    case Tool::Text: {
        for (int i = int(p->annotations.size()) - 1; i >= 0; --i) {
            const Annotation& a = p->annotations[i];
            if (a.type == Annotation::Type::Text && AnnotationRender::hitTest(a, u, 0.004)) {
                startTextEdit(a, a.rect.topLeft());
                return;
            }
        }
        startTextEdit(std::nullopt, u);
        return;
    }
    case Tool::Eraser: {
        m_workingList = p->annotations;
        m_workingListActive = true;
        moveTo(pos, pressure);
        return;
    }
    case Tool::Select: {
        if (button != Qt::LeftButton)
            return;
        const QRectF sr = selectionRectWidget();
        int handle = sr.isNull() ? -1 : handleAt(pos, sr);
        if (handle < 0) {
            QUuid hit;
            const double tol = 6 * pageUnitsPerPixel();
            for (int i = int(p->annotations.size()) - 1; i >= 0 && hit.isNull(); --i)
                if (AnnotationRender::hitTest(p->annotations[i], u, tol))
                    hit = p->annotations[i].id;
            if (hit.isNull()) {
                clearSelection();
                return;
            }
            m_selectedId = hit;
            handle = 8;
            Q_EMIT selectionChanged();
        }
        m_selHandle = handle;
        m_dragStartPage = u;
        m_dragOriginal = selected();
        m_dragStartRect = m_dragOriginal ? m_dragOriginal->bounds() : QRectF();
        m_workingList = p->annotations;
        m_workingListActive = true;
        update();
        return;
    }
    }
}

void PageCanvas::moveTo(const QPointF& pos, double pressure)
{
    const QPointF u = toPage(pos);
    switch (m_tool) {
    case Tool::Perspective:
        if (m_draggingCorner)
            setCornerPosition(m_corner, widgetToFull(pos));
        return;
    case Tool::Crop: {
        if (m_cropHandle < 0)
            return;
        const QRectF r = imageRect();
        QPointF n((pos.x() - r.left()) / r.width(), (pos.y() - r.top()) / r.height());
        n = QPointF(std::clamp(n.x(), 0.0, 1.0), std::clamp(n.y(), 0.0, 1.0));
        if (m_cropHandle == 9)
            m_crop = QRectF(m_cropAnchor, n).normalized();
        else
            m_crop = resizeRect(m_cropStart, m_cropHandle, n - m_cropAnchor).intersected(QRectF(0, 0, 1, 1));
        update();
        Q_EMIT cropChanged();
        return;
    }
    case Tool::Pen:
    case Tool::Highlighter:
        if (m_working) {
            const QPointF last = m_working->points.last();
            if (std::hypot(u.x() - last.x(), u.y() - last.y()) > 1.5 * pageUnitsPerPixel()) {
                m_working->points << u;
                if (!m_working->pressure.isEmpty())
                    m_working->pressure << pressure;
                update();
            }
        }
        return;
    case Tool::Shape:
        if (m_working) {
            if (m_working->type == Annotation::Type::Rect || m_working->type == Annotation::Type::Ellipse)
                m_working->rect = QRectF(m_working->rect.topLeft(), u);
            else
                m_working->points.last() = u;
            update();
        }
        return;
    case Tool::Eraser: {
        if (!m_workingListActive)
            return;
        const double radius = 8 * pageUnitsPerPixel();
        QList<Annotation> next;
        bool any = false;
        for (const Annotation& a : m_workingList) {
            bool changed = false;
            next << AnnotationRender::erase(a, u, radius, &changed);
            any = any || changed;
        }
        if (any) {
            m_workingList = next;
            update();
        }
        return;
    }
    case Tool::Select: {
        if (!m_workingListActive || !m_dragOriginal || m_selHandle < 0)
            return;
        const QPointF d = u - m_dragStartPage;
        for (Annotation& a : m_workingList) {
            if (a.id != m_selectedId)
                continue;
            a = *m_dragOriginal;
            if (m_selHandle == 8)
                a.translate(d);
            else
                a.mapRect(m_dragStartRect, resizeRect(m_dragStartRect, m_selHandle, d));
        }
        update();
        return;
    }
    case Tool::Text: return;
    }
}

void PageCanvas::releaseAt(const QPointF&)
{
    switch (m_tool) {
    case Tool::Perspective: m_draggingCorner = false; return;
    case Tool::Crop: m_cropHandle = -1; return;
    case Tool::Pen:
    case Tool::Highlighter:
        if (m_working)
            commitWorking(m_tool == Tool::Pen ? tr("Draw") : tr("Highlight"));
        return;
    case Tool::Shape:
        if (m_working && m_working->type != Annotation::Type::Polyline) {
            const QRectF b = m_working->bounds();
            if (std::max(b.width(), b.height()) < 4 * pageUnitsPerPixel()) {
                m_working.reset(); // a click without dragging
                update();
                return;
            }
            commitWorking(tr("Add %1").arg(Annotation::typeName(m_working->type).toLower()));
        }
        return;
    case Tool::Eraser:
        if (m_workingListActive) {
            m_workingListActive = false;
            CombinePage copy = *page();
            bool changed = copy.annotations.size() != m_workingList.size();
            for (int i = 0; !changed && i < m_workingList.size(); ++i)
                changed = copy.annotations[i].toJson() != m_workingList[i].toJson();
            if (changed) {
                copy.annotations = m_workingList;
                m_doc->updatePage(copy, tr("Erase"));
            }
            update();
        }
        return;
    case Tool::Select:
        if (m_workingListActive) {
            m_workingListActive = false;
            CombinePage copy = *page();
            bool moved = false;
            for (const Annotation& a : m_workingList)
                if (a.id == m_selectedId && m_dragOriginal && a.toJson() != m_dragOriginal->toJson())
                    moved = true;
            if (moved) {
                copy.annotations = m_workingList;
                m_doc->updatePage(copy, m_selHandle == 8 ? tr("Move annotation") : tr("Resize annotation"));
            }
            m_selHandle = -1;
            m_dragOriginal.reset();
            update();
            Q_EMIT selectionChanged();
        }
        return;
    case Tool::Text: return;
    }
}

void PageCanvas::commitWorking(const QString& description)
{
    if (!m_working || !page())
        return;
    Annotation a = *m_working;
    m_working.reset();
    if (a.type == Annotation::Type::Rect || a.type == Annotation::Type::Ellipse)
        a.rect = a.rect.normalized();
    CombinePage copy = *page();
    copy.annotations << a;
    m_doc->updatePage(copy, description);
    update();
}

void PageCanvas::keyPressEvent(QKeyEvent* e)
{
    if (m_tool == Tool::Perspective && m_quad) {
        const double step = (e->modifiers() & Qt::ShiftModifier ? 10.0 : 1.0) * double(rotatedSourceSize().width()) / std::max(1.0, imageRect().width());
        QPointF d;
        switch (e->key()) {
        case Qt::Key_Left: d = QPointF(-step, 0); break;
        case Qt::Key_Right: d = QPointF(step, 0); break;
        case Qt::Key_Up: d = QPointF(0, -step); break;
        case Qt::Key_Down: d = QPointF(0, step); break;
        case Qt::Key_Tab: selectCorner((m_corner + 1) % 4); return;
        case Qt::Key_Backtab: selectCorner((m_corner + 3) % 4); return;
        default: break;
        }
        if (!d.isNull()) {
            setCornerPosition(m_corner, m_quad->pts[size_t(m_corner)] + d);
            return;
        }
    }
    if (m_tool == Tool::Shape && m_working && m_working->type == Annotation::Type::Polyline
        && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        m_working->points.removeLast();
        commitWorking(tr("Draw polyline"));
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        if (m_working) {
            m_working.reset();
            update();
            return;
        }
        clearSelection();
        return;
    }
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !m_selectedId.isNull()) {
        deleteSelection();
        return;
    }
    if (e->key() == Qt::Key_D && (e->modifiers() & Qt::ControlModifier) && !m_selectedId.isNull()) {
        duplicateSelection();
        return;
    }
    QWidget::keyPressEvent(e);
}

} // namespace folvio::combine
