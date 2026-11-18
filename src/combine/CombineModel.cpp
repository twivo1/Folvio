#include "combine/CombineModel.h"

#include <QCollator>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QMimeData>
#include <QPixmap>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace folvio::combine {

namespace {

QJsonArray pointList(const QList<QPointF>& pts)
{
    QJsonArray a;
    for (const QPointF& p : pts) {
        a.append(p.x());
        a.append(p.y());
    }
    return a;
}

QList<QPointF> pointList(const QJsonArray& a)
{
    QList<QPointF> pts;
    for (int i = 0; i + 1 < a.size(); i += 2)
        pts << QPointF(a[i].toDouble(), a[i + 1].toDouble());
    return pts;
}

QJsonArray rectJson(const QRectF& r) { return {r.x(), r.y(), r.width(), r.height()}; }
QRectF rectFrom(const QJsonArray& a)
{
    return a.size() == 4 ? QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble()) : QRectF();
}

const char* kMime = "application/x-folvio-combine-pages";

} // namespace

// ---- Quad -----------------------------------------------------------------------

QJsonArray Quad::toJson() const
{
    return pointList(QList<QPointF>(pts.begin(), pts.end()));
}

std::optional<Quad> Quad::fromJson(const QJsonArray& a)
{
    const QList<QPointF> p = pointList(a);
    if (p.size() != 4)
        return std::nullopt;
    Quad q;
    for (int i = 0; i < 4; ++i)
        q.pts[size_t(i)] = p[i];
    return q;
}

// ---- Annotation -------------------------------------------------------------------

QString Annotation::typeName(Type t)
{
    switch (t) {
    case Type::Pen: return QObject::tr("Pen");
    case Type::Highlighter: return QObject::tr("Highlighter");
    case Type::Line: return QObject::tr("Line");
    case Type::Arrow: return QObject::tr("Arrow");
    case Type::Rect: return QObject::tr("Rectangle");
    case Type::Ellipse: return QObject::tr("Ellipse");
    case Type::Polyline: return QObject::tr("Polyline");
    case Type::Text: return QObject::tr("Text");
    }
    return {};
}

QRectF Annotation::bounds() const
{
    QRectF r;
    if (type == Type::Rect || type == Type::Ellipse || type == Type::Text) {
        r = rect.normalized();
    } else {
        for (const QPointF& p : points)
            r = r.isNull() ? QRectF(p, QSizeF(0, 0)) : r.united(QRectF(p, QSizeF(0, 0)));
    }
    const double pad = type == Type::Text ? 0 : width / 2 + (type == Type::Arrow ? width * 3 : 0);
    return r.adjusted(-pad, -pad, pad, pad);
}

void Annotation::translate(const QPointF& d)
{
    for (QPointF& p : points)
        p += d;
    rect.translate(d);
}

void Annotation::mapRect(const QRectF& from, const QRectF& to)
{
    if (from.width() <= 1e-9 || from.height() <= 1e-9)
        return;
    const double sx = to.width() / from.width(), sy = to.height() / from.height();
    auto map = [&](const QPointF& p) {
        return QPointF(to.left() + (p.x() - from.left()) * sx, to.top() + (p.y() - from.top()) * sy);
    };
    for (QPointF& p : points)
        p = map(p);
    if (!rect.isNull())
        rect = QRectF(map(rect.topLeft()), map(rect.bottomRight())).normalized();
    if (type == Type::Text)
        fontSize *= std::sqrt(std::max(1e-6, sx * sy));
}

QJsonObject Annotation::toJson() const
{
    QJsonObject o{{"id", id.toString(QUuid::WithoutBraces)},
                  {"type", int(type)},
                  {"stroke", stroke.name(QColor::HexArgb)},
                  {"fill", fill.name(QColor::HexArgb)},
                  {"hasFill", hasFill},
                  {"width", width},
                  {"opacity", opacity},
                  {"smooth", smooth}};
    if (!points.isEmpty())
        o.insert("points", pointList(points));
    if (!pressure.isEmpty()) {
        QJsonArray pr;
        for (double v : pressure)
            pr.append(v);
        o.insert("pressure", pr);
    }
    if (!rect.isNull())
        o.insert("rect", rectJson(rect));
    if (type == Type::Text) {
        o.insert("text", text);
        o.insert("font", fontFamily);
        o.insert("fontSize", fontSize);
        o.insert("bold", bold);
        o.insert("italic", italic);
        o.insert("underline", underline);
        o.insert("align", align);
    }
    return o;
}

Annotation Annotation::fromJson(const QJsonObject& o)
{
    Annotation a;
    a.id = QUuid::fromString(o.value("id").toString());
    if (a.id.isNull())
        a.id = QUuid::createUuid();
    a.type = Type(std::clamp(o.value("type").toInt(), 0, int(Type::Text)));
    a.stroke = QColor(o.value("stroke").toString("#ff141414"));
    a.fill = QColor(o.value("fill").toString("#ffffeb3b"));
    a.hasFill = o.value("hasFill").toBool();
    a.width = o.value("width").toDouble(0.004);
    a.opacity = std::clamp(o.value("opacity").toDouble(1.0), 0.0, 1.0);
    a.smooth = o.value("smooth").toBool(true);
    a.points = pointList(o.value("points").toArray());
    for (const QJsonValue& v : o.value("pressure").toArray())
        a.pressure << v.toDouble();
    a.rect = rectFrom(o.value("rect").toArray());
    a.text = o.value("text").toString();
    a.fontFamily = o.value("font").toString("Arial");
    a.fontSize = o.value("fontSize").toDouble(0.025);
    a.bold = o.value("bold").toBool();
    a.italic = o.value("italic").toBool();
    a.underline = o.value("underline").toBool();
    a.align = o.value("align").toInt();
    return a;
}

// ---- Page ---------------------------------------------------------------------

QString CombinePage::editSummary() const
{
    QStringList parts;
    if (rotation)
        parts << QObject::tr("rotated");
    if (quad)
        parts << QObject::tr("straightened");
    if (crop)
        parts << QObject::tr("cropped");
    if (!annotations.isEmpty())
        parts << QObject::tr("%n annotation(s)", nullptr, int(annotations.size()));
    return parts.join(QStringLiteral(" · "));
}

QJsonObject CombinePage::toJson() const
{
    QJsonObject o{{"id", id.toString(QUuid::WithoutBraces)},
                  {"source", sourcePath},
                  {"name", displayName},
                  {"sha256", QString::fromLatin1(sourceSha256.toHex())},
                  {"clipboard", fromClipboard},
                  {"w", sourceSize.width()},
                  {"h", sourceSize.height()},
                  {"rotation", rotation},
                  {"aspect", int(perspective.aspect)},
                  {"customRatio", perspective.customRatio},
                  {"margin", perspective.marginPercent}};
    if (quad)
        o.insert("quad", quad->toJson());
    if (crop)
        o.insert("crop", rectJson(*crop));
    QJsonArray anns;
    for (const Annotation& a : annotations)
        anns.append(a.toJson());
    o.insert("annotations", anns);
    return o;
}

CombinePage CombinePage::fromJson(const QJsonObject& o)
{
    CombinePage p;
    p.id = QUuid::fromString(o.value("id").toString());
    if (p.id.isNull())
        p.id = QUuid::createUuid();
    p.sourcePath = o.value("source").toString();
    p.displayName = o.value("name").toString();
    p.sourceSha256 = QByteArray::fromHex(o.value("sha256").toString().toLatin1());
    p.fromClipboard = o.value("clipboard").toBool();
    p.sourceSize = QSize(o.value("w").toInt(), o.value("h").toInt());
    p.rotation = ((o.value("rotation").toInt() % 360) + 360) % 360 / 90 * 90;
    p.perspective.aspect = AspectMode(std::clamp(o.value("aspect").toInt(), 0, 3));
    p.perspective.customRatio = o.value("customRatio").toDouble(1.4142);
    p.perspective.marginPercent = o.value("margin").toDouble();
    p.quad = Quad::fromJson(o.value("quad").toArray());
    if (o.contains("crop"))
        p.crop = rectFrom(o.value("crop").toArray());
    for (const QJsonValue& v : o.value("annotations").toArray())
        p.annotations << Annotation::fromJson(v.toObject());
    return p;
}

CombinePage CombinePage::duplicated() const
{
    CombinePage p = *this;
    p.id = QUuid::createUuid();
    for (Annotation& a : p.annotations)
        a.id = QUuid::createUuid();
    return p;
}

// ---- Export settings --------------------------------------------------------------

double ExportSettings::marginPoints() const
{
    switch (margins) {
    case MarginMode::None: return 0;
    case MarginMode::Small: return 18;   // 6.35 mm
    case MarginMode::Normal: return 36;  // 12.7 mm
    case MarginMode::Custom: return std::max(0.0, customMarginMm) * 72.0 / 25.4;
    }
    return 0;
}

QJsonObject ExportSettings::toJson() const
{
    return {{"pageSize", int(pageSize)}, {"customW", customSizeMm.width()}, {"customH", customSizeMm.height()},
            {"orientation", int(orientation)}, {"margins", int(margins)}, {"customMargin", customMarginMm},
            {"quality", int(quality)}, {"folder", lastFolder}, {"fileName", fileName}};
}

ExportSettings ExportSettings::fromJson(const QJsonObject& o)
{
    ExportSettings s;
    s.pageSize = PageSizeMode(std::clamp(o.value("pageSize").toInt(int(s.pageSize)), 0, 3));
    s.customSizeMm = QSizeF(o.value("customW").toDouble(210), o.value("customH").toDouble(297));
    s.orientation = OrientationMode(std::clamp(o.value("orientation").toInt(), 0, 2));
    s.margins = MarginMode(std::clamp(o.value("margins").toInt(int(s.margins)), 0, 3));
    s.customMarginMm = o.value("customMargin").toDouble(10);
    s.quality = QualityMode(std::clamp(o.value("quality").toInt(int(s.quality)), 0, 2));
    s.lastFolder = o.value("folder").toString();
    s.fileName = o.value("fileName").toString(s.fileName);
    return s;
}

// ---- Document ------------------------------------------------------------------

CombineDocument::CombineDocument(QObject* parent) : QObject(parent)
{
    m_history.append({m_pages, m_export, tr("Start")});
    m_undoIndex = 0;
}

int CombineDocument::indexOf(const QUuid& id) const
{
    for (int i = 0; i < m_pages.size(); ++i)
        if (m_pages[i].id == id)
            return i;
    return -1;
}

void CombineDocument::record(const QString& description)
{
    if (m_macroDepth > 0)
        return;
    while (m_history.size() > m_undoIndex + 1)
        m_history.removeLast();
    m_history.append({m_pages, m_export, description});
    constexpr int kMaxHistory = 200;
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
    m_undoIndex = int(m_history.size()) - 1;
    Q_EMIT historyChanged();
}

void CombineDocument::beginMacro(const QString& description)
{
    if (m_macroDepth++ == 0)
        m_macroDescription = description;
}

void CombineDocument::endMacro()
{
    if (m_macroDepth == 0)
        return;
    if (--m_macroDepth == 0) {
        // Only record if something actually changed.
        const Snapshot& last = m_history[m_undoIndex];
        bool changed = last.pages.size() != m_pages.size() || !(last.exportSettings.toJson() == m_export.toJson());
        for (int i = 0; !changed && i < m_pages.size(); ++i)
            changed = last.pages[i].toJson() != m_pages[i].toJson();
        if (changed)
            record(m_macroDescription);
    }
}

void CombineDocument::restore(const Snapshot& s)
{
    QSet<QUuid> before, after;
    for (const CombinePage& p : m_pages)
        before.insert(p.id);
    m_pages = s.pages;
    m_export = s.exportSettings;
    for (const CombinePage& p : m_pages)
        after.insert(p.id);
    Q_EMIT pagesChanged();
    for (const CombinePage& p : m_pages)
        Q_EMIT pageChanged(p.id);
    Q_EMIT exportSettingsChanged();
    Q_EMIT historyChanged();
}

QString CombineDocument::undoText() const
{
    return canUndo() ? m_history[m_undoIndex].description : QString();
}

QString CombineDocument::redoText() const
{
    return canRedo() ? m_history[m_undoIndex + 1].description : QString();
}

void CombineDocument::undo()
{
    if (!canUndo())
        return;
    --m_undoIndex;
    restore(m_history[m_undoIndex]);
}

void CombineDocument::redo()
{
    if (!canRedo())
        return;
    ++m_undoIndex;
    restore(m_history[m_undoIndex]);
}

void CombineDocument::addPages(const QList<CombinePage>& pages, int at)
{
    if (pages.isEmpty())
        return;
    if (at < 0 || at > m_pages.size())
        at = int(m_pages.size());
    for (int i = 0; i < pages.size(); ++i)
        m_pages.insert(at + i, pages[i]);
    Q_EMIT pagesChanged();
    record(tr("Add %n page(s)", nullptr, int(pages.size())));
}

void CombineDocument::removePages(QList<int> indices)
{
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    int removed = 0;
    for (int i : indices)
        if (i >= 0 && i < m_pages.size()) {
            m_pages.removeAt(i);
            ++removed;
        }
    if (!removed)
        return;
    Q_EMIT pagesChanged();
    record(tr("Remove %n page(s)", nullptr, removed));
}

void CombineDocument::duplicatePages(QList<int> indices)
{
    std::sort(indices.begin(), indices.end());
    int offset = 0;
    for (int i : indices) {
        const int src = i + offset;
        if (src < 0 || src >= m_pages.size())
            continue;
        m_pages.insert(src + 1, m_pages[src].duplicated());
        ++offset;
    }
    if (!offset)
        return;
    Q_EMIT pagesChanged();
    record(tr("Duplicate %n page(s)", nullptr, offset));
}

void CombineDocument::movePages(QList<int> indices, int destination)
{
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    QList<CombinePage> moving;
    int before = 0;
    for (int i : indices)
        if (i >= 0 && i < m_pages.size()) {
            moving << m_pages[i];
            if (i < destination)
                ++before;
        }
    if (moving.isEmpty())
        return;
    for (auto it = indices.crbegin(); it != indices.crend(); ++it)
        if (*it >= 0 && *it < m_pages.size())
            m_pages.removeAt(*it);
    int dest = std::clamp(destination - before, 0, int(m_pages.size()));
    QList<QUuid> oldOrder;
    for (int i = 0; i < moving.size(); ++i)
        m_pages.insert(dest + i, moving[i]);
    Q_EMIT pagesChanged();
    record(tr("Reorder pages"));
}

void CombineDocument::movePage(int from, int to)
{
    if (from < 0 || from >= m_pages.size() || to < 0 || to >= m_pages.size() || from == to)
        return;
    m_pages.move(from, to);
    Q_EMIT pagesChanged();
    record(tr("Move page"));
}

void CombineDocument::sortByName()
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(m_pages.begin(), m_pages.end(), [&](const CombinePage& a, const CombinePage& b) {
        return collator.compare(a.displayName, b.displayName) < 0;
    });
    Q_EMIT pagesChanged();
    record(tr("Sort by name"));
}

void CombineDocument::updatePage(const CombinePage& page, const QString& description)
{
    const int i = indexOf(page.id);
    if (i < 0)
        return;
    m_pages[i] = page;
    Q_EMIT pageChanged(page.id);
    record(description);
}

void CombineDocument::setExportSettings(const ExportSettings& s)
{
    m_export = s;
    Q_EMIT exportSettingsChanged();
    // Export settings are not part of undo history for each keystroke; snapshot keeps the latest.
    if (m_undoIndex >= 0)
        m_history[m_undoIndex].exportSettings = s;
}

void CombineDocument::clear()
{
    m_pages.clear();
    m_history.clear();
    m_history.append({m_pages, m_export, tr("Start")});
    m_undoIndex = 0;
    Q_EMIT pagesChanged();
    Q_EMIT historyChanged();
}

QJsonObject CombineDocument::toJson() const
{
    QJsonArray pages;
    for (const CombinePage& p : m_pages)
        pages.append(p.toJson());
    return {{"format", "folvio-combine"}, {"version", 1}, {"pages", pages}, {"export", m_export.toJson()}};
}

bool CombineDocument::fromJson(const QJsonObject& o, QString* error)
{
    if (o.value("format").toString() != "folvio-combine") {
        if (error)
            *error = tr("This is not a Folvio Combine project.");
        return false;
    }
    if (o.value("version").toInt() > 1) {
        if (error)
            *error = tr("The project was created by a newer version of Folvio.");
        return false;
    }
    QList<CombinePage> pages;
    for (const QJsonValue& v : o.value("pages").toArray())
        pages << CombinePage::fromJson(v.toObject());
    m_pages = pages;
    m_export = ExportSettings::fromJson(o.value("export").toObject());
    m_history.clear();
    m_history.append({m_pages, m_export, tr("Open")});
    m_undoIndex = 0;
    Q_EMIT pagesChanged();
    Q_EMIT exportSettingsChanged();
    Q_EMIT historyChanged();
    return true;
}

// ---- PageListModel ----------------------------------------------------------------

PageListModel::PageListModel(CombineDocument* doc, QObject* parent) : QAbstractListModel(parent), m_doc(doc)
{
    connect(doc, &CombineDocument::pagesChanged, this, [this] {
        beginResetModel();
        endResetModel();
    });
    connect(doc, &CombineDocument::pageChanged, this, [this](const QUuid& id) {
        const int i = m_doc->indexOf(id);
        if (i >= 0)
            Q_EMIT dataChanged(index(i), index(i));
    });
}

int PageListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_doc->count();
}

QVariant PageListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_doc->count())
        return {};
    const CombinePage& p = m_doc->page(index.row());
    switch (role) {
    case Qt::DisplayRole: return QStringLiteral("%1. %2").arg(index.row() + 1).arg(p.displayName);
    case Qt::ToolTipRole: {
        QString tip = p.displayName;
        if (!p.sourcePath.isEmpty() && !p.fromClipboard)
            tip += "\n" + p.sourcePath;
        if (p.hasEdits())
            tip += "\n" + p.editSummary();
        return tip;
    }
    case Qt::DecorationRole: {
        auto it = m_thumbs.constFind(p.id);
        if (it != m_thumbs.constEnd())
            return QPixmap::fromImage(*it);
        if (!m_requested.contains(p.id)) {
            m_requested.insert(p.id);
            Q_EMIT const_cast<PageListModel*>(this)->thumbnailNeeded(p.id);
        }
        return {};
    }
    case PageIdRole: return p.id;
    case EditedRole: return p.hasEdits();
    case SummaryRole: return p.editSummary();
    default: return {};
    }
}

Qt::ItemFlags PageListModel::flags(const QModelIndex& index) const
{
    const Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (index.isValid())
        return f | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable;
    return f | Qt::ItemIsDropEnabled;
}

QStringList PageListModel::mimeTypes() const
{
    return {QString::fromLatin1(kMime), QStringLiteral("text/uri-list")};
}

QMimeData* PageListModel::mimeData(const QModelIndexList& indexes) const
{
    auto* m = new QMimeData;
    QStringList rows;
    for (const QModelIndex& i : indexes)
        rows << QString::number(i.row());
    m->setData(QString::fromLatin1(kMime), rows.join(',').toLatin1());
    return m;
}

bool PageListModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex&) const
{
    return data->hasFormat(QString::fromLatin1(kMime)) || data->hasUrls();
}

bool PageListModel::dropMimeData(const QMimeData* data, Qt::DropAction, int row, int, const QModelIndex& parent)
{
    if (row < 0)
        row = parent.isValid() ? parent.row() : m_doc->count();
    if (data->hasFormat(QString::fromLatin1(kMime))) {
        QList<int> rows;
        for (const QByteArray& r : data->data(QString::fromLatin1(kMime)).split(','))
            rows << r.toInt();
        m_doc->movePages(rows, row);
        return false; // the document already moved the rows; prevent removeRows by the view
    }
    QStringList files;
    for (const QUrl& u : data->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (!files.isEmpty())
        Q_EMIT externalFilesDropped(files, row);
    return false;
}

void PageListModel::setThumbnail(const QUuid& id, const QImage& image)
{
    m_thumbs.insert(id, image);
    m_requested.remove(id);
    const int i = m_doc->indexOf(id);
    if (i >= 0)
        Q_EMIT dataChanged(index(i), index(i), {Qt::DecorationRole});
}

} // namespace folvio::combine
