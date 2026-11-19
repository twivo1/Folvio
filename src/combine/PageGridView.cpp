#include "combine/PageGridView.h"

#include "combine/CombineModel.h"
#include "ui/themes/Theme.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

namespace folvio::combine {

namespace {
std::function<Qt::DropAction(QDrag*)>& dragExecutor()
{
    static std::function<Qt::DropAction(QDrag*)> f;
    return f;
}
constexpr char kPageMime[] = "application/x-folvio-combine-pages";
constexpr char kViewMime[] = "application/x-folvio-combine-view"; // identifies the view that started the drag
} // namespace

void PageGridView::setDragExecutorForTesting(std::function<Qt::DropAction(QDrag*)> executor)
{
    dragExecutor() = std::move(executor);
}

PageGridView::PageGridView(Layout layout, QWidget* parent) : QListView(parent), m_layout(layout)
{
    setViewMode(QListView::IconMode);
    setFlow(layout == Layout::Grid ? QListView::LeftToRight : QListView::TopToBottom);
    setWrapping(layout == Layout::Grid);
    setResizeMode(QListView::Adjust);
    // Static: the view never moves item rectangles on its own; positions always follow the model order.
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(false); // drawn by paintEvent
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
}

bool PageGridView::isInternal(const QMimeData* mime) const
{
    return mime && mime->hasFormat(QString::fromLatin1(kPageMime));
}

bool PageGridView::isFromThisView(const QMimeData* mime) const
{
    // QDropEvent::source() is not reliable on every platform (e.g. some Wayland setups), so the drag
    // carries the identity of the view that started it.
    return isInternal(mime) && mime->data(QString::fromLatin1(kViewMime)) == QByteArray::number(quintptr(this));
}

int PageGridView::insertionIndexAt(const QPoint& pos, QLine* indicator) const
{
    const int n = model() ? model()->rowCount() : 0;
    if (n == 0) {
        if (indicator)
            *indicator = QLine();
        return 0;
    }
    auto rectOf = [this](int row) { return visualRect(model()->index(row, 0)); };
    const bool grid = m_layout == Layout::Grid;
    // Below the last row of cards = the end, above the first row = the beginning.
    if (pos.y() > rectOf(n - 1).bottom()) {
        const QRect last = rectOf(n - 1);
        if (indicator)
            *indicator = grid ? QLine(last.right() + 2, last.top() + 4, last.right() + 2, last.bottom() - 4)
                              : QLine(last.left() + 4, last.bottom() + 2, last.right() - 4, last.bottom() + 2);
        return n;
    }
    if (pos.y() < rectOf(0).top()) {
        const QRect first = rectOf(0);
        if (indicator)
            *indicator = grid ? QLine(first.left() - 2, first.top() + 4, first.left() - 2, first.bottom() - 4)
                              : QLine(first.left() + 4, first.top() - 2, first.right() - 4, first.top() - 2);
        return 0;
    }
    // Rows of cards ("bands") in a grid share the same vertical span; a vertical list has one card per band.
    int bestBandRow = -1;
    double bestDist = 1e18;
    for (int r = 0; r < n; ++r) {
        const QRect rc = rectOf(r);
        const double d = grid ? (pos.y() < rc.top() ? rc.top() - pos.y() : pos.y() > rc.bottom() ? pos.y() - rc.bottom() : 0)
                              : std::abs(pos.y() - rc.center().y());
        if (d < bestDist) {
            bestDist = d;
            bestBandRow = r;
        }
    }
    int index = n;
    QLine line;
    if (!grid) {
        const QRect rc = rectOf(bestBandRow);
        index = pos.y() < rc.center().y() ? bestBandRow : bestBandRow + 1;
        const int y = index < n ? rectOf(index).top() - 2 : rectOf(n - 1).bottom() + 2;
        line = QLine(rc.left() + 4, y, rc.right() - 4, y);
    } else {
        const QRect band = rectOf(bestBandRow);
        QList<int> inBand;
        for (int r = 0; r < n; ++r) {
            const QRect rc = rectOf(r);
            if (rc.top() == band.top())
                inBand << r;
        }
        index = inBand.last() + 1;
        for (int r : inBand)
            if (pos.x() < rectOf(r).center().x()) {
                index = r;
                break;
            }
        // Line at the left edge of the card that follows, or after the last card of the band.
        const bool before = index <= inBand.last() && inBand.contains(index);
        const QRect rc = rectOf(before ? index : inBand.last());
        const int x = before ? rc.left() - 2 : rc.right() + 2;
        line = QLine(x, rc.top() + 4, x, rc.bottom() - 4);
    }
    if (indicator)
        *indicator = line;
    return index;
}

void PageGridView::startDrag(Qt::DropActions)
{
    const QModelIndexList selected = selectedIndexes();
    if (selected.isEmpty() || !model())
        return;
    QList<int> rows;
    for (const QModelIndex& i : selected)
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    auto* mime = new QMimeData;
    QStringList list;
    for (int r : rows)
        list << QString::number(r);
    mime->setData(QString::fromLatin1(kPageMime), list.join(',').toLatin1());
    mime->setData(QString::fromLatin1(kViewMime), QByteArray::number(quintptr(this)));

    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    // Drag preview: the first card, with the number of pages when several are dragged.
    const QRect card = visualRect(model()->index(rows.first(), 0));
    QPixmap pm = viewport()->grab(card);
    if (!pm.isNull()) {
        pm = pm.scaledToWidth(std::min(150, pm.width()), Qt::SmoothTransformation);
        if (rows.size() > 1) {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(tc().accent);
            p.setPen(Qt::NoPen);
            const QRectF badge(pm.width() - 34, 4, 30, 20);
            p.drawRoundedRect(badge, 10, 10);
            p.setPen(tc().accentText);
            p.drawText(badge, Qt::AlignCenter, QString::number(rows.size()));
        }
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 3));
    }
    // Only MoveAction: a page is never duplicated by dragging. Nothing is removed afterwards either -
    // the drop handler already reordered the document (QAbstractItemView would remove rows here).
    if (dragExecutor())
        dragExecutor()(drag);
    else
        drag->exec(Qt::MoveAction, Qt::MoveAction);
    clearIndicator();
    // Some platforms deliver the button release to the view after the drag loop; it would be treated
    // as a click and replace the selection of the moved pages.
    m_dragFinished = true;
    QTimer::singleShot(0, this, &PageGridView::restoreSelectionAfterDrop);
}

void PageGridView::restoreSelectionAfterDrop()
{
    if (m_selectionAfterDrop.isEmpty() || !selectionModel())
        return;
    selectionModel()->select(m_selectionAfterDrop, QItemSelectionModel::ClearAndSelect);
    selectionModel()->setCurrentIndex(m_selectionAfterDrop.indexes().value(0), QItemSelectionModel::NoUpdate);
}

void PageGridView::mousePressEvent(QMouseEvent* e)
{
    m_dragFinished = false;
    m_selectionAfterDrop = QItemSelection();
    QListView::mousePressEvent(e);
}

void PageGridView::mouseReleaseEvent(QMouseEvent* e)
{
    if (!m_dragFinished || !selectionModel()) {
        QListView::mouseReleaseEvent(e);
        return;
    }
    // Let Qt finish its press/release bookkeeping, but keep the selection of the moved pages.
    m_dragFinished = false;
    QListView::mouseReleaseEvent(e);
    restoreSelectionAfterDrop();
    m_selectionAfterDrop = QItemSelection();
}

void PageGridView::dragEnterEvent(QDragEnterEvent* e)
{
    const QMimeData* m = e->mimeData();
    if (isFromThisView(m)) {
        e->setDropAction(Qt::MoveAction);
        e->accept();
    } else if (!isInternal(m) && (m->hasUrls() || m->hasImage())) {
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else {
        e->ignore();
    }
}

void PageGridView::dragMoveEvent(QDragMoveEvent* e)
{
    const QMimeData* m = e->mimeData();
    const bool internal = isFromThisView(m);
    if (!internal && !(m->hasUrls() || m->hasImage())) {
        e->ignore();
        return;
    }
    const QPoint pos = e->position().toPoint();
    QLine line;
    const int index = insertionIndexAt(pos, &line);
    if (index != m_dropIndex || line != m_indicator) {
        m_dropIndex = index;
        m_indicator = line;
        viewport()->update();
    }
    autoScrollNear(pos);
    e->setDropAction(internal ? Qt::MoveAction : Qt::CopyAction);
    e->accept();
}

void PageGridView::dragLeaveEvent(QDragLeaveEvent* e)
{
    clearIndicator(); // leaving the view cancels the target; the order stays as it was
    e->accept();
}

void PageGridView::dropEvent(QDropEvent* e)
{
    const QMimeData* m = e->mimeData();
    const int index = insertionIndexAt(e->position().toPoint());
    clearIndicator();
    if (isInternal(m)) {
        if (!isFromThisView(m)) {
            e->ignore();
            return;
        }
        QList<int> rows;
        for (const QByteArray& r : m->data(QString::fromLatin1(kPageMime)).split(','))
            if (!r.isEmpty())
                rows << r.toInt();
        e->setDropAction(Qt::MoveAction);
        e->accept();
        Q_EMIT reorderRequested(rows, index);
        // Qt's own bookkeeping after the drag loop may still change the selection; the moved pages
        // are selected again once it has finished.
        m_selectionAfterDrop = selectionModel() ? selectionModel()->selection() : QItemSelection();
        return;
    }
    QStringList files;
    for (const QUrl& u : m->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    e->setDropAction(Qt::CopyAction);
    e->accept();
    if (!files.isEmpty())
        Q_EMIT filesDropped(files, index);
    else if (m->hasImage())
        Q_EMIT imageDropped(qvariant_cast<QImage>(m->imageData()), index);
}

void PageGridView::autoScrollNear(const QPoint& pos)
{
    const int margin = 36;
    const QRect r = viewport()->rect();
    QScrollBar* bar = verticalScrollBar();
    if (pos.y() < r.top() + margin)
        bar->setValue(bar->value() - 18);
    else if (pos.y() > r.bottom() - margin)
        bar->setValue(bar->value() + 18);
}

void PageGridView::clearIndicator()
{
    if (m_dropIndex >= 0 || !m_indicator.isNull()) {
        m_dropIndex = -1;
        m_indicator = QLine();
        viewport()->update();
    }
}

void PageGridView::paintEvent(QPaintEvent* e)
{
    QListView::paintEvent(e);
    if (m_dropIndex < 0 || m_indicator.isNull())
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(tc().accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(m_indicator);
    p.setBrush(tc().accent);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(m_indicator.p1()), 5, 5);
    p.drawEllipse(QPointF(m_indicator.p2()), 5, 5);
}

} // namespace folvio::combine
