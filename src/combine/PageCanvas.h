#pragma once

#include "combine/CombineModel.h"
#include "combine/ImageGeometry.h"

#include <QHash>
#include <QImage>
#include <QPlainTextEdit>
#include <QPointer>
#include <QWidget>

#include <optional>

namespace folvio::combine {

class CombineTextEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CombineTextEditor(QWidget* parent = nullptr);
Q_SIGNALS:
    void commitRequested();
    void cancelRequested();
protected:
    void keyPressEvent(QKeyEvent* e) override;
};

// Style used for new annotations (and edited through the options panel).
struct ToolStyle {
    QColor stroke = QColor(20, 20, 20);
    double width = 0.004;      // page units
    double opacity = 1.0;
    bool smooth = true;
    QColor fill = QColor(255, 235, 59);
    bool hasFill = false;
    QString fontFamily = QStringLiteral("Arial");
    double fontSize = 0.025;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    int align = 0;
};

class PageCanvas : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Perspective, Crop, Pen, Highlighter, Eraser, Shape, Text };
    enum class CompareMode { Original, Corrected, Split };

    explicit PageCanvas(QWidget* parent = nullptr);

    void setDocument(CombineDocument* doc);
    void setPage(const QUuid& id);
    QUuid pageId() const { return m_pageId; }
    void setTool(Tool t);
    Tool tool() const { return m_tool; }
    void setShapeType(Annotation::Type t) { m_shapeType = t; }
    Annotation::Type shapeType() const { return m_shapeType; }
    ToolStyle& style() { return m_style; }
    void setStyle(const ToolStyle& s) { m_style = s; }

    // View
    void setZoom(double z); // 1.0 = 100 % of preview pixels at 96 dpi
    double zoom() const { return m_zoom; }
    void fitToView(const QSize& viewport);
    QSize sizeHint() const override;

    // Perspective
    std::optional<Quad> pendingQuad() const { return m_quad; }
    QuadCheck pendingQuadCheck() const;
    PerspectiveSettings& pendingPerspective() { return m_perspective; }
    int selectedCorner() const { return m_corner; }
    void selectCorner(int c);
    void setCornerPosition(int c, const QPointF& fullResPoint);
    QSize rotatedSourceSize() const;
    bool detectPage();
    void resetQuad();
    void autoOrderCorners();
    bool applyPerspective(QString* error);
    void removePerspective();
    void setCompareMode(CompareMode m);
    CompareMode compareMode() const { return m_compare; }
    void refreshPerspectivePreview();

    // Crop
    QRectF pendingCrop() const { return m_crop; }
    void applyCrop();
    void resetCrop();

    // Selection
    std::optional<Annotation> selected() const;
    void updateSelected(const Annotation& a, const QString& description);
    void deleteSelection();
    void duplicateSelection();
    void raiseSelection(int delta); // +1 forward, -1 backward
    void clearSelection();
    bool isEditingText() const;
    void commitText();

    // Rotation of the whole page (90° steps)
    void rotatePage(int degrees);

    // Coordinate mapping (for tests and the options panel).
    QRectF imageRect() const;              // widget rect of the displayed image
    QPointF toPage(const QPointF& w) const; // widget -> page units
    QPointF fromPage(const QPointF& u) const;
    QPointF fullToWidget(const QPointF& p) const; // perspective: full-res source px -> widget
    QPointF widgetToFull(const QPointF& w) const;

Q_SIGNALS:
    void selectionChanged();
    void perspectiveChanged();
    void cropChanged();
    void zoomChanged(double zoom);
    void message(const QString& text, bool error);
    void toolRequested(folvio::combine::PageCanvas::Tool tool);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void tabletEvent(QTabletEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool focusNextPrevChild(bool next) override;

private:
    const CombinePage* page() const;
    QImage displayImage() const;      // image for the current tool / compare mode
    QImage stageImage(ImageProcessor::Stage stage) const;
    QImage correctedPreview() const;
    void invalidate();
    void initPendingQuad();
    void relayout();
    void pressAt(const QPointF& pos, double pressure, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void moveTo(const QPointF& pos, double pressure);
    void releaseAt(const QPointF& pos);
    void commitWorking(const QString& description);
    void startTextEdit(const std::optional<Annotation>& existing, const QPointF& pageTopLeft);
    int handleAt(const QPointF& w, const QRectF& rect) const; // 0..7 or -1
    QRectF selectionRectWidget() const;
    double pageUnitsPerPixel() const;

    QPointer<CombineDocument> m_doc;
    QUuid m_pageId;
    Tool m_tool = Tool::Select;
    Annotation::Type m_shapeType = Annotation::Type::Arrow;
    ToolStyle m_style;
    double m_zoom = 1.0;
    QSize m_imageSize;               // size of the displayed image (preview pixels)
    CompareMode m_compare = CompareMode::Original;

    // Caches (preview resolution)
    mutable QHash<QString, QImage> m_cache;
    mutable QStringList m_cacheOrder;
    mutable QImage m_correctedPreview;
    mutable QString m_correctedKey;

    // Perspective
    std::optional<Quad> m_quad;      // full-resolution coordinates
    PerspectiveSettings m_perspective;
    int m_corner = 0;
    bool m_draggingCorner = false;

    // Crop (normalized on the rectified image)
    QRectF m_crop = QRectF(0, 0, 1, 1);
    int m_cropHandle = -1;           // 0..7 handles, 8 = move, 9 = new rect
    QPointF m_cropAnchor;
    QRectF m_cropStart;

    // Annotation interaction
    std::optional<Annotation> m_working;   // being drawn
    QList<Annotation> m_workingList;       // page annotations during move/resize/erase
    bool m_workingListActive = false;
    QUuid m_selectedId;
    int m_selHandle = -1;                  // 0..7 resize, 8 move
    QPointF m_dragStartPage;
    QRectF m_dragStartRect;
    std::optional<Annotation> m_dragOriginal;
    bool m_tabletActive = false;
    CombineTextEditor* m_textEdit = nullptr;
    std::optional<Annotation> m_textTarget; // annotation being edited (new or existing)
};

} // namespace folvio::combine
