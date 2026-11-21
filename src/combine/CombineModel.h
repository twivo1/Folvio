#pragma once

// COMBINE domain model: pages built from images, non-destructive geometry and
// vector annotations. No GUI dependencies beyond QtGui value types.
//
// Order of operations for every page (documented in docs/COMBINE.md):
//   source image -> EXIF orientation -> user rotation (90° steps)
//   -> perspective rectification (quad -> rectangle, optional margin)
//   -> crop (normalized rectangle of the rectified image)
//   -> annotations (page-width units on the final image)

#include <QAbstractListModel>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QUuid>

#include <functional>

#include <array>
#include <optional>

namespace folvio::combine {

// Four page corners in pixel coordinates of the oriented (EXIF + rotation) source image.
struct Quad {
    std::array<QPointF, 4> pts; // TL, TR, BR, BL
    QJsonArray toJson() const;
    static std::optional<Quad> fromJson(const QJsonArray& a);
    bool operator==(const Quad& o) const { return pts == o.pts; }
};

enum class AspectMode { Auto, A4, Letter, Custom };

struct PerspectiveSettings {
    AspectMode aspect = AspectMode::Auto;
    double customRatio = 1.4142; // height / width for Custom
    double marginPercent = 0.0;  // extra border around the page, % of the page width
    bool operator==(const PerspectiveSettings& o) const
    {
        return aspect == o.aspect && customRatio == o.customRatio && marginPercent == o.marginPercent;
    }
};

struct Annotation {
    enum class Type { Pen, Highlighter, Line, Arrow, Rect, Ellipse, Polyline, Text };
    QUuid id = QUuid::createUuid();
    Type type = Type::Pen;
    // Geometry in page units: x and y divided by the page width (aspect-preserving).
    QList<QPointF> points;      // Pen, Highlighter, Line, Arrow, Polyline
    QList<double> pressure;     // optional, per point (0..1), Pen only
    QRectF rect;                // Rect, Ellipse, Text
    QColor stroke = QColor(20, 20, 20);
    QColor fill = QColor(255, 235, 59);
    bool hasFill = false;
    double width = 0.004;       // stroke width in page units
    double opacity = 1.0;
    bool smooth = true;
    // Text
    QString text;
    QString fontFamily = QStringLiteral("Arial");
    double fontSize = 0.025;    // page units (fraction of page width)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    int align = 0;              // 0 left, 1 center, 2 right
    bool autoSize = true;       // text box height follows the text; false = user-set height (never below the text)

    QRectF bounds() const;      // page units, including stroke
    void translate(const QPointF& d);
    // Maps the geometry with 'map' (page units -> page units) where lengths change by 'scale':
    // stroke widths and font sizes follow, a text box keeps its proportions around its mapped centre.
    void transform(const std::function<QPointF(const QPointF&)>& map, double scale);
    // Maps the annotation from bounds 'from' to 'to' (resize handles).
    void mapRect(const QRectF& from, const QRectF& to);
    QJsonObject toJson() const;
    static Annotation fromJson(const QJsonObject& o);
    static QString typeName(Type t);
    bool isStroke() const { return type == Type::Pen || type == Type::Highlighter || type == Type::Polyline; }
};

struct CombinePage {
    QUuid id = QUuid::createUuid();
    QString sourcePath;          // image file the page was created from (never modified)
    QString displayName;
    QByteArray sourceSha256;     // fingerprint at import time (detects missing/changed sources)
    bool fromClipboard = false;
    QSize sourceSize;            // oriented source size in pixels (after EXIF orientation)
    int rotation = 0;            // 0, 90, 180, 270 clockwise, applied after EXIF orientation
    std::optional<Quad> quad;    // perspective correction, if applied
    PerspectiveSettings perspective;
    std::optional<QRectF> crop;  // normalized (0..1) rectangle of the rectified image
    QList<Annotation> annotations;

    bool hasEdits() const { return rotation != 0 || quad || crop || !annotations.isEmpty(); }
    QString editSummary() const;
    QJsonObject toJson() const;
    static CombinePage fromJson(const QJsonObject& o);
    CombinePage duplicated() const;
};

enum class PageSizeMode { Auto, A4, Letter, Custom };
enum class OrientationMode { Auto, Portrait, Landscape };
enum class MarginMode { None, Small, Normal, Custom };
enum class QualityMode { High, Balanced, SmallFile };

struct ExportSettings {
    PageSizeMode pageSize = PageSizeMode::A4;
    QSizeF customSizeMm = QSizeF(210, 297);
    OrientationMode orientation = OrientationMode::Auto;
    MarginMode margins = MarginMode::Small;
    double customMarginMm = 10;
    QualityMode quality = QualityMode::Balanced;
    QString lastFolder;
    QString fileName = QStringLiteral("Combined.pdf");

    double marginPoints() const;
    QJsonObject toJson() const;
    static ExportSettings fromJson(const QJsonObject& o);
};

// The document: ordered pages + export settings, with snapshot-based undo/redo.
class CombineDocument : public QObject {
    Q_OBJECT
public:
    explicit CombineDocument(QObject* parent = nullptr);

    int count() const { return int(m_pages.size()); }
    const CombinePage& page(int index) const { return m_pages[index]; }
    int indexOf(const QUuid& id) const;
    const QList<CombinePage>& pages() const { return m_pages; }
    const ExportSettings& exportSettings() const { return m_export; }

    // Mutations (each records one undo step unless 'record' is false).
    void addPages(const QList<CombinePage>& pages, int at = -1);
    void removePages(QList<int> indices);
    void duplicatePages(QList<int> indices);
    void movePages(QList<int> indices, int destination); // destination = index before which to insert
    void movePage(int from, int to);
    void sortByName();
    void updatePage(const CombinePage& page, const QString& description); // matched by id
    void setExportSettings(const ExportSettings& s);
    void clear();

    // Undo/redo of whole-document snapshots.
    bool canUndo() const { return m_undoIndex > 0; }
    bool canRedo() const { return m_undoIndex + 1 < m_history.size(); }
    QString undoText() const;
    QString redoText() const;
    void undo();
    void redo();
    // Groups many small changes (e.g. dragging a handle) into one undo step.
    void beginMacro(const QString& description);
    void endMacro();
    // Forgets the history (the current state becomes the start), e.g. after opening an image.
    void clearHistory();
    // Changed since the last markSaved()/clearHistory() (image editor: since the last export).
    bool isModified() const { return m_undoIndex != m_savedIndex; }
    void markSaved() { m_savedIndex = m_undoIndex; }

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& o, QString* error);
    bool isEmpty() const { return m_pages.isEmpty(); }

Q_SIGNALS:
    void pagesChanged();          // structure or order changed
    void pageChanged(const QUuid& id);
    void historyChanged();
    void exportSettingsChanged();

private:
    struct Snapshot {
        QList<CombinePage> pages;
        ExportSettings exportSettings;
        QString description;
    };
    void record(const QString& description);
    void restore(const Snapshot& s);

    QList<CombinePage> m_pages;
    ExportSettings m_export;
    QList<Snapshot> m_history; // m_history[m_undoIndex] is the current state
    int m_undoIndex = -1;
    int m_savedIndex = 0;
    int m_macroDepth = 0;
    QString m_macroDescription;
};

// Thumbnail list model over a CombineDocument with drag & drop reordering.
class PageListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { PageIdRole = Qt::UserRole + 1, EditedRole, SummaryRole };
    explicit PageListModel(CombineDocument* doc, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction | Qt::CopyAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;

    void setThumbnail(const QUuid& id, const QImage& image);
    QImage thumbnail(const QUuid& id) const { return m_thumbs.value(id); }
    void invalidateThumbnail(const QUuid& id)
    {
        m_thumbs.remove(id);
        m_requested.remove(id);
    }

Q_SIGNALS:
    void thumbnailNeeded(const QUuid& id);
    void externalFilesDropped(const QStringList& paths, int row);

private:
    CombineDocument* m_doc;
    QHash<QUuid, QImage> m_thumbs;
    mutable QSet<QUuid> m_requested;
};

} // namespace folvio::combine
