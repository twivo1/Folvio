#pragma once

#include <QLine>
#include <QItemSelection>
#include <QListView>

#include <functional>

class QDrag;

namespace folvio::combine {

// Thumbnail list/grid of Combine pages with predictable drag & drop reordering.
//
// QListView in IconMode moves item *rectangles* for internal drags (like desktop icons) and never
// tells the model, so pages overlapped and the order did not change. This view is static: Qt never
// positions items itself. While dragging it computes an insertion index (left/right half of a card in
// a grid, top/bottom half in a vertical list), draws an insertion line, and on drop asks the owner to
// reorder the document. The document stays the only source of truth for the page order.
class PageGridView : public QListView {
    Q_OBJECT
public:
    enum class Layout { Grid, VerticalList };
    explicit PageGridView(Layout layout, QWidget* parent = nullptr);

    // Insertion index (0..rowCount) for a position in viewport coordinates.
    int insertionIndexAt(const QPoint& viewportPos, QLine* indicator = nullptr) const;
    int dropIndicatorIndex() const { return m_dropIndex; }

    // Tests replace the platform drag loop (OLE / X11 / Wayland) with a function that delivers the
    // drag/drop events; the application always uses QDrag::exec.
    static void setDragExecutorForTesting(std::function<Qt::DropAction(QDrag*)> executor);

Q_SIGNALS:
    // Rows (current indices) dropped before 'insertIndex' (index in the current order).
    void reorderRequested(const QList<int>& rows, int insertIndex);
    void filesDropped(const QStringList& paths, int insertIndex);
    void imageDropped(const QImage& image, int insertIndex);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    bool isInternal(const QMimeData* mime) const;
    bool isFromThisView(const QMimeData* mime) const;
    void clearIndicator();
    void autoScrollNear(const QPoint& pos);

    Layout m_layout;
    int m_dropIndex = -1;
    QLine m_indicator;
    bool m_dragFinished = false; // a release right after a drag must not change the selection
    QItemSelection m_selectionAfterDrop; // the moved pages, selected by the owner after the reorder
    void restoreSelectionAfterDrop();
};

} // namespace folvio::combine
