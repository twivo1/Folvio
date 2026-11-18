#include "editor/pdf_native/PdfEditSession.h"

#include "core/files/PathUtils.h"
#include "editor/pdf_native/FontMatcher.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QMutex>
#include <QRawFont>
#include <QRegularExpression>
#include <QSaveFile>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cmath>

namespace folvio {

namespace {

quint64 key(int page, int id) { return (quint64(quint32(page)) << 32) | quint32(id); }

// ---- font metrics & data caches -------------------------------------------

QMutex& cacheMutex()
{
    static QMutex m;
    return m;
}

QByteArray fontData(const QString& path)
{
    static QHash<QString, QByteArray> cache;
    QMutexLocker lock(&cacheMutex());
    auto it = cache.find(path);
    if (it != cache.end())
        return *it;
    QFile f(path);
    QByteArray data = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    cache.insert(path, data);
    return data;
}

const QRawFont& rawFont(const QString& path)
{
    static QHash<QString, QRawFont> cache;
    QMutexLocker lock(&cacheMutex());
    auto it = cache.find(path);
    if (it == cache.end())
        it = cache.insert(path, QRawFont(path, 100.0));
    return *it;
}

double textWidth(const QString& path, double size, const QString& text)
{
    const QRawFont& f = rawFont(path);
    if (!f.isValid() || text.isEmpty())
        return text.size() * size * 0.5;
    const QList<quint32> glyphs = f.glyphIndexesForString(text);
    const QList<QPointF> adv = f.advancesForGlyphIndexes(glyphs);
    double w = 0;
    for (const QPointF& a : adv)
        w += a.x();
    return w * size / 100.0;
}

bool fontSupports(const QString& path, const QString& text, QString* missing)
{
    const QRawFont& f = rawFont(path);
    if (!f.isValid())
        return false;
    for (const QChar c : text) {
        if (c.isSpace())
            continue;
        if (!f.supportsCharacter(c)) {
            if (missing)
                *missing += c;
        }
    }
    return !missing || missing->isEmpty();
}

QStringList wrapLine(const QString& line, const QString& fontPath, double size, double maxWidth)
{
    if (maxWidth <= 0 || textWidth(fontPath, size, line) <= maxWidth * 1.02)
        return {line};
    QStringList out;
    const QStringList words = line.split(QLatin1Char(' '));
    QString cur;
    for (const QString& w : words) {
        const QString candidate = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
        if (!cur.isEmpty() && textWidth(fontPath, size, candidate) > maxWidth * 1.02) {
            out << cur;
            cur = w;
        } else {
            cur = candidate;
        }
    }
    if (!cur.isEmpty() || out.isEmpty())
        out << cur;
    return out;
}

QString normalizeForCompare(QString s)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    s.remove(ws);
    s.remove(QChar(0xFFFE)); // PDFium soft hyphen marker
    s.remove(QChar(0x00AD));
    return s;
}

// ---- PDFium object creation (caller holds pdfiumMutex) ---------------------

struct FontCache {
    FPDF_DOCUMENT doc = nullptr;
    QHash<QString, FPDF_FONT> fonts;
    FPDF_FONT get(const QString& path)
    {
        auto it = fonts.find(path);
        if (it != fonts.end())
            return *it;
        const QByteArray data = fontData(path);
        FPDF_FONT f = data.isEmpty() ? nullptr
                                     : FPDFText_LoadFont(doc, reinterpret_cast<const uint8_t*>(data.constData()),
                                                         uint32_t(data.size()), FPDF_FONT_TRUETYPE, 1);
        fonts.insert(path, f);
        return f;
    }
    ~FontCache()
    {
        for (FPDF_FONT f : fonts)
            if (f)
                FPDFFont_Close(f);
    }
};

FPDF_PAGEOBJECT createTextObject(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FONT font, double rawSize,
                                 const QString& text, const QTransform& m, const QColor& color, int renderMode,
                                 int* insertIndex = nullptr)
{
    FPDF_PAGEOBJECT obj = FPDFPageObj_CreateTextObj(doc, font, float(rawSize));
    if (!obj)
        return nullptr;
    std::u16string utf16 = text.toStdU16String();
    FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(utf16.c_str()));
    FPDFPageObj_SetFillColor(obj, uint(color.red()), uint(color.green()), uint(color.blue()), uint(color.alpha()));
    FPDFTextObj_SetTextRenderMode(obj, FPDF_TEXT_RENDERMODE(renderMode));
    FS_MATRIX fm{float(m.m11()), float(m.m12()), float(m.m21()), float(m.m22()), float(m.dx()), float(m.dy())};
    FPDFPageObj_SetMatrix(obj, &fm);
    // Insert where the replaced text was, so the content (reading) order is preserved.
    if (insertIndex && *insertIndex >= 0 && FPDFPage_InsertObjectAtIndex(page, obj, size_t(*insertIndex)))
        ++*insertIndex;
    else
        FPDFPage_InsertObject(page, obj);
    return obj;
}

QRectF objectBounds(FPDF_PAGEOBJECT obj);

int indexOfObject(FPDF_PAGE page, FPDF_PAGEOBJECT obj)
{
    const int n = FPDFPage_CountObjects(page);
    for (int i = 0; i < n; ++i)
        if (FPDFPage_GetObject(page, i) == obj)
            return i;
    return -1;
}

// Longest-common-subsequence alignment of old and new lines.
// 'k' keep (old i, new j), 'd' delete old i, 'i' insert new j.
struct LineOp {
    char type;
    int oldIndex;
    int newIndex;
};
QList<LineOp> diffLines(const QStringList& a, const QStringList& b)
{
    const int n = int(a.size()), m = int(b.size());
    std::vector<std::vector<int>> L(size_t(n + 1), std::vector<int>(size_t(m + 1), 0));
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            L[size_t(i)][size_t(j)] = a[i] == b[j] ? L[size_t(i + 1)][size_t(j + 1)] + 1
                                                   : std::max(L[size_t(i + 1)][size_t(j)], L[size_t(i)][size_t(j + 1)]);
    QList<LineOp> ops;
    int i = 0, j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j])
            ops.append({'k', i++, j++});
        else if (L[size_t(i + 1)][size_t(j)] >= L[size_t(i)][size_t(j + 1)])
            ops.append({'d', i++, -1});
        else
            ops.append({'i', -1, j++});
    }
    while (i < n)
        ops.append({'d', i++, -1});
    while (j < m)
        ops.append({'i', -1, j++});
    return ops;
}

// Writes a line of text as one text object per word. The inter-word gap is the
// font's space advance. Encoding the space glyph itself is avoided because the
// ToUnicode map PDFium generates can resolve it to U+00A0, which breaks copy &
// search; positioned words extract as normal spaces in every viewer.
QRectF placeTextLine(FPDF_DOCUMENT doc, FPDF_PAGE page, FPDF_FONT font, const QString& fontPath, double rawSize,
                     const QString& text, const QTransform& m, const QColor& color, int renderMode,
                     int* insertIndex = nullptr)
{
    QRectF bounds;
    const double spaceAdvance = textWidth(fontPath, rawSize, QStringLiteral(" "));
    double advance = 0; // in text space units
    const QStringList words = text.split(QLatin1Char(' '));
    for (int i = 0; i < words.size(); ++i) {
        const QString& w = words[i];
        if (!w.isEmpty()) {
            const QTransform wm(m.m11(), m.m12(), m.m21(), m.m22(), m.dx() + advance * m.m11(),
                                m.dy() + advance * m.m12());
            FPDF_PAGEOBJECT obj = createTextObject(doc, page, font, rawSize, w, wm, color, renderMode, insertIndex);
            const QRectF b = objectBounds(obj);
            if (!b.isNull())
                bounds = bounds.isNull() ? b : bounds.united(b);
            advance += textWidth(fontPath, rawSize, w);
        }
        if (i + 1 < words.size())
            advance += spaceAdvance;
    }
    return bounds;
}

QRectF objectBounds(FPDF_PAGEOBJECT obj)
{
    float l = 0, b = 0, r = 0, t = 0;
    if (!obj || !FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
        return {};
    return QRectF(QPointF(l, b), QPointF(r, t)).normalized();
}

QJsonObject styleToJson(const TextStyle& s)
{
    return {{"family", s.family}, {"bold", s.bold}, {"italic", s.italic}, {"size", s.size},
            {"color", s.color.name(QColor::HexArgb)}, {"align", s.align}};
}

TextStyle styleFromJson(const QJsonObject& o)
{
    TextStyle s;
    s.family = o.value("family").toString();
    s.bold = o.value("bold").toBool();
    s.italic = o.value("italic").toBool();
    s.size = o.value("size").toDouble(11);
    s.color = QColor(o.value("color").toString("#ff000000"));
    s.align = o.value("align").toInt();
    return s;
}

} // namespace

// ---- EditOp serialization ---------------------------------------------------

QJsonObject EditOp::toJson() const
{
    QJsonObject o{{"kind", int(kind)}, {"page", page}, {"blockId", blockId}, {"addedId", addedId}, {"text", text},
                  {"style", styleToJson(style)}, {"styleChanged", styleChanged}, {"ox", origin.x()},
                  {"oy", origin.y()}, {"width", width}};
    if (kind == Kind::AddImage) {
        o.insert("rect", QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()});
        o.insert("image", QString::fromLatin1(image.toBase64()));
    }
    if (kind == Kind::OcrLayer) {
        QJsonArray w;
        for (const auto& word : words)
            w.append(QJsonArray{word.text, word.rect.x(), word.rect.y(), word.rect.width(), word.rect.height()});
        o.insert("words", w);
    }
    return o;
}

EditOp EditOp::fromJson(const QJsonObject& o)
{
    EditOp e;
    e.kind = Kind(o.value("kind").toInt());
    e.page = o.value("page").toInt();
    e.blockId = o.value("blockId").toInt(-1);
    e.addedId = o.value("addedId").toInt(-1);
    e.text = o.value("text").toString();
    e.style = styleFromJson(o.value("style").toObject());
    e.styleChanged = o.value("styleChanged").toBool();
    e.origin = QPointF(o.value("ox").toDouble(), o.value("oy").toDouble());
    e.width = o.value("width").toDouble();
    const QJsonArray r = o.value("rect").toArray();
    if (r.size() == 4)
        e.rect = QRectF(r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble());
    e.image = QByteArray::fromBase64(o.value("image").toString().toLatin1());
    for (const QJsonValue& v : o.value("words").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 5)
            e.words << OcrWordPdf{a[0].toString(), QRectF(a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble())};
    }
    return e;
}

// ---- Session ----------------------------------------------------------------

PdfEditSession::PdfEditSession(QObject* parent) : QObject(parent) {}
PdfEditSession::~PdfEditSession() = default;

PdfEditSession::OpenStatus PdfEditSession::open(const QString& path, const QByteArray& password, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return OpenStatus::Failed;
    }
    QByteArray bytes = f.readAll();
    f.close();
    PdfDocument::OpenError err;
    QString msg;
    auto doc = PdfDocument::openData(bytes, password, &err, &msg);
    if (!doc) {
        if (error)
            *error = msg;
        return err == PdfDocument::OpenError::Password ? OpenStatus::NeedsPassword : OpenStatus::Failed;
    }
    m_path = QFileInfo(path).absoluteFilePath();
    m_originalBytes = bytes;
    m_password = password;
    m_fingerprint = FileFingerprint::of(path);
    m_original = std::move(doc);
    m_analysis = m_original->analyze();
    m_blocks.clear();
    m_runs.clear();
    m_ops.clear();
    m_cursor = m_savedCursor = 0;
    m_currentBounds.clear();
    m_added.clear();
    QString e;
    if (!rebuild(&e)) {
        if (error)
            *error = e;
        return OpenStatus::Failed;
    }
    return OpenStatus::Ok;
}

int PdfEditSession::pageCount() const { return m_original ? m_original->pageCount() : 0; }

const QList<TextBlock>& PdfEditSession::blocks(int page) const
{
    auto it = m_blocks.find(page);
    if (it == m_blocks.end()) {
        const QList<PdfTextRun> runs = m_original ? m_original->textRuns(page) : QList<PdfTextRun>{};
        m_runs.insert(page, runs);
        it = m_blocks.insert(page, PdfTextLayout::build(runs, page, m_original->horizontalRules(page)));
    }
    return *it;
}

TextStyle PdfEditSession::styleForRun(const PdfTextRun& run) const
{
    TextStyle s;
    const FontMatch m = FontMatcher::match(run.fontName, run.bold, run.italic, run.serif, run.monospace);
    s.family = m.family.isEmpty() ? QStringLiteral("Arial") : m.family;
    s.bold = run.bold;
    s.italic = run.italic;
    s.size = std::round(run.effectiveSize * 2.0) / 2.0;
    s.color = run.color.isValid() ? run.color : QColor(Qt::black);
    s.color.setAlpha(255);
    return s;
}

int PdfEditSession::detectAlignment(const TextBlock& b)
{
    if (b.lines.size() < 2)
        return TextStyle::Left;
    const double size = std::max(1.0, b.style.effectiveSize);
    double maxLeftDev = 0, maxRightDev = 0, maxCenterDev = 0;
    const TextLine& f = b.lines.first();
    for (const TextLine& l : b.lines) {
        maxLeftDev = std::max(maxLeftDev, std::abs(l.bounds.left() - f.bounds.left()));
        maxRightDev = std::max(maxRightDev, std::abs(l.bounds.right() - f.bounds.right()));
        maxCenterDev = std::max(maxCenterDev, std::abs(l.bounds.center().x() - f.bounds.center().x()));
    }
    if (maxLeftDev < 0.5 * size)
        return TextStyle::Left;
    if (maxCenterDev < 0.5 * size)
        return TextStyle::Center;
    if (maxRightDev < 0.5 * size)
        return TextStyle::Right;
    return TextStyle::Left;
}

BlockState PdfEditSession::blockState(int page, int blockId) const
{
    BlockState st;
    const QList<TextBlock>& bl = blocks(page);
    if (blockId < 0 || blockId >= bl.size())
        return st;
    const TextBlock& b = bl[blockId];
    st.text = b.text();
    st.style = styleForRun(b.style);
    st.style.align = detectAlignment(b);
    st.bounds = b.bounds;
    st.editable = b.editable;
    st.mixedStyle = b.mixedStyle;
    for (int i = m_cursor - 1; i >= 0; --i) {
        const EditOp& op = m_ops[i];
        if (op.kind == EditOp::Kind::ReplaceBlock && op.page == page && op.blockId == blockId) {
            st.text = op.text;
            if (op.styleChanged)
                st.style = op.style;
            st.edited = true;
            break;
        }
    }
    const auto it = m_currentBounds.find(key(page, blockId));
    if (it != m_currentBounds.end())
        st.bounds = *it;
    return st;
}

QList<AddedItem> PdfEditSession::addedItems(int page) const
{
    QList<AddedItem> list;
    for (const AddedItem& a : m_added)
        if (a.page == page && !a.text.isEmpty())
            list << a;
    return list;
}

PdfEditSession::Hit PdfEditSession::hitTest(int page, const QPointF& p) const
{
    Hit h;
    for (const AddedItem& a : addedItems(page))
        if (a.bounds.adjusted(-2, -2, 2, 2).contains(p)) {
            h.type = Hit::Added;
            h.id = a.addedId;
            return h;
        }
    const QList<TextBlock>& bl = blocks(page);
    double bestArea = 1e18;
    for (const TextBlock& b : bl) {
        const BlockState st = blockState(page, b.id);
        QRectF r = st.bounds.isEmpty() ? b.bounds : st.bounds;
        if (st.edited && st.text.isEmpty())
            r = b.bounds; // deleted block: still clickable at its old place
        if (r.adjusted(-2, -2, 2, 2).contains(p) && r.width() * r.height() < bestArea) {
            bestArea = r.width() * r.height();
            h.type = Hit::Block;
            h.id = b.id;
        }
    }
    return h;
}

int PdfEditSession::nextAddedId() const
{
    int maxId = 0;
    for (const EditOp& op : m_ops)
        maxId = std::max(maxId, op.addedId);
    return maxId + 1;
}

QSet<int> PdfEditSession::changedPages() const
{
    QSet<int> pages;
    for (int i = 0; i < m_cursor; ++i)
        pages.insert(m_ops[i].page);
    return pages;
}

bool PdfEditSession::apply(const EditOp& op, QString* error)
{
    if (!m_original) {
        if (error)
            *error = tr("No document is open.");
        return false;
    }
    if (op.page < 0 || op.page >= pageCount()) {
        if (error)
            *error = tr("Invalid page.");
        return false;
    }
    const bool wasModified = isModified();
    const QList<EditOp> backup = m_ops;
    const int backupCursor = m_cursor;
    m_ops = m_ops.mid(0, m_cursor);
    if (m_savedCursor > m_cursor)
        m_savedCursor = -1; // the saved state is no longer reachable by undo/redo
    m_ops.append(op);
    m_cursor = int(m_ops.size());
    QString e;
    if (!rebuild(&e)) {
        m_ops = backup;
        m_cursor = backupCursor;
        rebuild(nullptr);
        if (error)
            *error = e;
        return false;
    }
    Q_EMIT documentChanged({op.page});
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

bool PdfEditSession::undo()
{
    if (!canUndo())
        return false;
    const bool wasModified = isModified();
    const int page = m_ops[m_cursor - 1].page;
    --m_cursor;
    rebuild(nullptr);
    Q_EMIT documentChanged({page});
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

bool PdfEditSession::redo()
{
    if (!canRedo())
        return false;
    const bool wasModified = isModified();
    const int page = m_ops[m_cursor].page;
    ++m_cursor;
    rebuild(nullptr);
    Q_EMIT documentChanged({page});
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

bool PdfEditSession::rebuild(QString* error)
{
    m_warnings.clear();
    m_currentBounds.clear();
    m_added.clear();
    PdfDocument::OpenError oe;
    QString msg;
    auto doc = PdfDocument::openData(m_originalBytes, m_password, &oe, &msg);
    if (!doc) {
        if (error)
            *error = msg;
        return false;
    }
    if (m_cursor == 0) {
        m_current = std::move(doc);
        return true;
    }

    // Effective operations per page: the last op for a block / added item wins.
    QMap<int, QList<int>> opsByPage;
    for (int i = 0; i < m_cursor; ++i)
        opsByPage[m_ops[i].page] << i;

    std::lock_guard lock(pdfiumMutex());
    FontCache fonts;
    fonts.doc = doc->handle();

    for (auto pit = opsByPage.cbegin(); pit != opsByPage.cend(); ++pit) {
        const int pageIndex = pit.key();
        QHash<int, int> lastForBlock, lastForAdded;
        QList<int> images, ocr;
        for (int i : pit.value()) {
            const EditOp& op = m_ops[i];
            switch (op.kind) {
            case EditOp::Kind::ReplaceBlock: lastForBlock[op.blockId] = i; break;
            case EditOp::Kind::AddText: lastForAdded[op.addedId] = i; break;
            case EditOp::Kind::AddImage: images << i; break;
            case EditOp::Kind::OcrLayer: ocr = {i}; break;
            }
        }
        const QList<TextBlock>& pageBlocks = blocks(pageIndex);
        const QList<PdfTextRun>& runs = m_runs[pageIndex];

        FPDF_PAGE page = FPDF_LoadPage(doc->handle(), pageIndex);
        if (!page) {
            if (error)
                *error = tr("Page %1 could not be loaded.").arg(pageIndex + 1);
            return false;
        }
        // Snapshot original object handles before anything is removed.
        const int objCount = FPDFPage_CountObjects(page);
        std::vector<FPDF_PAGEOBJECT> handles(size_t(std::max(0, objCount)));
        for (int i = 0; i < objCount; ++i)
            handles[size_t(i)] = FPDFPage_GetObject(page, i);
        auto removeObject = [&](int index) {
            if (index >= 0 && index < objCount && handles[size_t(index)]) {
                if (FPDFPage_RemoveObject(page, handles[size_t(index)]))
                    FPDFPageObj_Destroy(handles[size_t(index)]);
                handles[size_t(index)] = nullptr;
            }
        };

        // --- block replacements -------------------------------------------
        for (auto it = lastForBlock.cbegin(); it != lastForBlock.cend(); ++it) {
            const EditOp& op = m_ops[it.value()];
            if (op.blockId < 0 || op.blockId >= pageBlocks.size())
                continue;
            const TextBlock& block = pageBlocks[op.blockId];
            const TextStyle original = [&] {
                TextStyle s = styleForRun(block.style);
                s.align = detectAlignment(block);
                return s;
            }();
            const TextStyle style = op.styleChanged ? op.style : original;
            const bool styleChanged = op.styleChanged && style != original;

            const QStringList oldLines = [&] {
                QStringList l;
                for (const auto& line : block.lines)
                    l << line.text;
                return l;
            }();
            const QStringList newLines = op.text.isEmpty() ? QStringList() : op.text.split(QLatin1Char('\n'));
            const FontMatch fm = FontMatcher::byFamily(style.family, style.bold, style.italic).isValid()
                                     ? FontMatcher::byFamily(style.family, style.bold, style.italic)
                                     : FontMatcher::match(block.style.fontName, style.bold, style.italic,
                                                          block.style.serif, block.style.monospace);
            if (!fm.isValid()) {
                FPDF_ClosePage(page);
                if (error)
                    *error = tr("No suitable font is installed for the edited text.");
                return false;
            }
            QString missing;
            if (!fontSupports(fm.filePath, op.text, &missing))
                m_warnings << tr("Font %1 cannot display: %2").arg(fm.family, missing);
            if (!fm.exact && !block.style.fontName.isEmpty())
                m_warnings << tr("Edited text uses %1 as a replacement for the embedded font \"%2\".")
                                  .arg(fm.family, block.style.fontName);
            if (block.mixedStyle)
                m_warnings << tr("This paragraph had mixed formatting; edited lines use a single style.");
            FPDF_FONT font = fonts.get(fm.filePath);
            if (!font) {
                FPDF_ClosePage(page);
                if (error)
                    *error = tr("Font %1 could not be embedded.").arg(fm.family);
                return false;
            }

            const PdfTextRun& base = block.style;
            const double scaleY = std::max(1e-6, std::sqrt(base.matrix.m21() * base.matrix.m21() + base.matrix.m22() * base.matrix.m22()));
            const double rawSize = styleChanged || base.fontSize <= 0 ? style.size / scaleY : base.fontSize;
            const double visualSize = rawSize * scaleY;
            const double spacing = block.lineSpacing > 0 ? block.lineSpacing : visualSize * 1.2;
            const double blockLeft = block.bounds.left();
            const double blockRight = block.bounds.right();
            const double blockWidth = std::max(1.0, blockRight - blockLeft);
            const bool wrap = block.lineCount() > 1;
            // Wrapping width: the text column, not just the longest existing line. The column
            // ends at the page margin or at the nearest text to the right in the same band.
            const double pageWidth = FPDF_GetPageWidthF(page);
            double rightLimit = pageWidth - std::clamp(blockLeft, 36.0, 90.0);
            for (const TextBlock& other : pageBlocks) {
                if (other.id == block.id || other.bounds.left() < blockRight - 1.0)
                    continue;
                const bool overlapsVertically = other.bounds.top() < block.bounds.bottom() + spacing
                                                && other.bounds.bottom() > block.bounds.top() - spacing;
                if (overlapsVertically)
                    rightLimit = std::min(rightLimit, other.bounds.left() - 0.5 * visualSize);
            }
            const double wrapWidth = std::max(blockWidth, rightLimit - blockLeft);

            QRectF newBounds;
            auto unite = [&](const QRectF& b) {
                if (!b.isNull())
                    newBounds = newBounds.isNull() ? b : newBounds.united(b);
            };
            auto lineX = [&](const QString& text, double originalLeft) {
                const double w = textWidth(fm.filePath, visualSize, text);
                switch (style.align) {
                case TextStyle::Center: return blockLeft + (blockWidth - w) / 2.0;
                case TextStyle::Right: return blockRight - w;
                default: return styleChanged || originalLeft < 0 ? blockLeft : originalLeft;
                }
            };
            auto firstHandleOfLine = [&](int lineIndex) -> FPDF_PAGEOBJECT {
                for (int r : block.lines[lineIndex].runs) {
                    const int oi = runs[r].objectIndex;
                    if (oi >= 0 && oi < objCount && handles[size_t(oi)])
                        return handles[size_t(oi)];
                }
                return nullptr;
            };

            // Align old and new lines. Unchanged lines keep their original objects
            // (moved vertically only if lines were added/removed above them); only
            // changed lines are re-created, at the content-stream position of the
            // text they replace.
            const QList<LineOp> diff = styleChanged ? [&] {
                QList<LineOp> all;
                for (int i = 0; i < oldLines.size(); ++i)
                    all.append({'d', i, -1});
                for (int j = 0; j < newLines.size(); ++j)
                    all.append({'i', -1, j});
                return all;
            }()
                                                    : diffLines(oldLines, newLines);
            double shift = 0;           // vertical displacement applied to following kept lines
            double prevBaseline = block.lines.first().baseline + spacing; // virtual line above the block
            int produced = 0;
            int k = 0;
            while (k < diff.size()) {
                if (diff[k].type == 'k') {
                    const TextLine& line = block.lines[diff[k].oldIndex];
                    if (std::abs(shift) > 0.01) {
                        for (int r : line.runs) {
                            const int oi = runs[r].objectIndex;
                            if (oi >= 0 && oi < objCount && handles[size_t(oi)])
                                FPDFPageObj_Transform(handles[size_t(oi)], 1, 0, 0, 1, 0, shift);
                        }
                    }
                    unite(line.bounds.translated(0, shift));
                    prevBaseline = line.baseline + shift;
                    ++produced;
                    ++k;
                    continue;
                }
                // A change region: consecutive deletions/insertions.
                QList<int> deleted, inserted;
                while (k < diff.size() && diff[k].type != 'k') {
                    if (diff[k].type == 'd')
                        deleted << diff[k].oldIndex;
                    else
                        inserted << diff[k].newIndex;
                    ++k;
                }
                QStringList pieces;
                for (int j : inserted) {
                    const QStringList wrapped = wrap ? wrapLine(newLines[j], fm.filePath, visualSize, wrapWidth)
                                                     : QStringList{newLines[j]};
                    pieces << wrapped;
                }
                int insertAt = -1;
                if (!deleted.isEmpty()) {
                    if (FPDF_PAGEOBJECT h = firstHandleOfLine(deleted.first()))
                        insertAt = indexOfObject(page, h);
                }
                for (int p = 0; p < pieces.size(); ++p) {
                    double baseline;
                    double origLeft = -1;
                    if (p < deleted.size()) {
                        const TextLine& slot = block.lines[deleted[p]];
                        baseline = slot.baseline + shift;
                        origLeft = slot.left;
                    } else {
                        baseline = prevBaseline - spacing;
                    }
                    if (!pieces[p].isEmpty()) {
                        const QTransform m(base.matrix.m11(), base.matrix.m12(), base.matrix.m21(), base.matrix.m22(),
                                           lineX(pieces[p], origLeft), baseline);
                        unite(placeTextLine(doc->handle(), page, font, fm.filePath, rawSize, pieces[p], m, style.color,
                                            0, insertAt >= 0 ? &insertAt : nullptr));
                    }
                    prevBaseline = baseline;
                    ++produced;
                }
                // Remove the replaced lines after the new objects took their place.
                for (int i : deleted)
                    for (int r : block.lines[i].runs)
                        removeObject(runs[r].objectIndex);
                // Lines below move up/down by the difference in line count.
                shift += double(deleted.size() - pieces.size()) * spacing;
            }
            if (produced > block.lineCount())
                m_warnings << tr("The edited paragraph is %n line(s) longer than the original and may overlap "
                                 "content below it.", nullptr, produced - block.lineCount());
            if (!op.text.isEmpty() && newBounds.right() > pageWidth + 1)
                m_warnings << tr("The edited text extends beyond the right edge of the page.");
            m_currentBounds.insert(key(pageIndex, op.blockId), newBounds.isNull() ? block.bounds : newBounds);
        }

        // --- added text boxes --------------------------------------------
        for (auto it = lastForAdded.cbegin(); it != lastForAdded.cend(); ++it) {
            const EditOp& op = m_ops[it.value()];
            AddedItem item;
            item.addedId = op.addedId;
            item.page = pageIndex;
            item.text = op.text;
            item.style = op.style;
            item.origin = op.origin;
            item.width = op.width;
            if (op.text.isEmpty()) {
                m_added.insert(key(pageIndex, op.addedId), item);
                continue;
            }
            const FontMatch fm = FontMatcher::byFamily(op.style.family, op.style.bold, op.style.italic).isValid()
                                     ? FontMatcher::byFamily(op.style.family, op.style.bold, op.style.italic)
                                     : FontMatcher::byFamily("Arial", op.style.bold, op.style.italic);
            FPDF_FONT font = fonts.get(fm.filePath);
            if (!font)
                continue;
            const double size = std::max(1.0, op.style.size);
            double baseline = op.origin.y() - size * 0.95;
            QRectF bounds;
            for (const QString& para : op.text.split(QLatin1Char('\n'))) {
                for (const QString& w : wrapLine(para, fm.filePath, size, op.width)) {
                    const double tw = textWidth(fm.filePath, size, w);
                    double x = op.origin.x();
                    if (op.width > 0 && op.style.align == TextStyle::Center)
                        x += (op.width - tw) / 2;
                    else if (op.width > 0 && op.style.align == TextStyle::Right)
                        x += op.width - tw;
                    if (!w.isEmpty()) {
                        const QRectF b = placeTextLine(doc->handle(), page, font, fm.filePath, size, w,
                                                       QTransform(1, 0, 0, 1, x, baseline), op.style.color, 0);
                        if (!b.isNull())
                            bounds = bounds.isNull() ? b : bounds.united(b);
                    }
                    baseline -= size * 1.2;
                }
            }
            if (bounds.isNull())
                bounds = QRectF(op.origin.x(), op.origin.y() - size, std::max(op.width, size), size);
            item.bounds = bounds;
            m_added.insert(key(pageIndex, op.addedId), item);
        }

        // --- images ------------------------------------------------------------
        for (int i : images) {
            const EditOp& op = m_ops[i];
            QImage img = QImage::fromData(op.image).convertToFormat(QImage::Format_ARGB32);
            if (img.isNull() || op.rect.isEmpty())
                continue;
            FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc->handle());
            FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRA, img.bits(),
                                                  int(img.bytesPerLine()));
            if (!obj || !bmp || !FPDFImageObj_SetBitmap(nullptr, 0, obj, bmp)) {
                if (bmp)
                    FPDFBitmap_Destroy(bmp);
                if (obj)
                    FPDFPageObj_Destroy(obj);
                continue;
            }
            FPDFBitmap_Destroy(bmp);
            FS_MATRIX m{float(op.rect.width()), 0, 0, float(op.rect.height()), float(op.rect.x()), float(op.rect.y())};
            FPDFPageObj_SetMatrix(obj, &m);
            FPDFPage_InsertObject(page, obj);
        }

        // --- OCR text layer (invisible, keeps the scanned image untouched) --------
        for (int i : ocr) {
            const EditOp& op = m_ops[i];
            const FontMatch fm = FontMatcher::byFamily("Arial", false, false);
            FPDF_FONT font = fonts.get(fm.filePath);
            if (!font)
                continue;
            for (const OcrWordPdf& w : op.words) {
                if (w.text.isEmpty() || w.rect.isEmpty())
                    continue;
                const double size = std::max(1.0, w.rect.height() * 0.9);
                const double natural = std::max(0.1, textWidth(fm.filePath, size, w.text));
                const double sx = w.rect.width() / natural;
                placeTextLine(doc->handle(), page, font, fm.filePath, size, w.text,
                              QTransform(sx, 0, 0, 1, w.rect.left(), w.rect.top() + w.rect.height() * 0.2),
                              Qt::black, 3 /* invisible */);
            }
        }

        if (!FPDFPage_GenerateContent(page)) {
            FPDF_ClosePage(page);
            if (error)
                *error = tr("PDFium could not regenerate page %1.").arg(pageIndex + 1);
            return false;
        }
        FPDF_ClosePage(page);
    }
    m_warnings.removeDuplicates();
    m_current = std::move(doc);
    return true;
}

// ---- Saving ---------------------------------------------------------------------

bool PdfEditSession::serialize(QByteArray* out, QString* error) const
{
    if (!m_current) {
        if (error)
            *error = tr("No document is open.");
        return false;
    }
    // Signed documents: append changes as an incremental update so the signed
    // revision stays byte-identical inside the file.
    const auto mode = m_analysis.signatureCount > 0 ? PdfDocument::SaveMode::Incremental : PdfDocument::SaveMode::Full;
    return m_current->save(out, mode, error);
}

bool PdfEditSession::verifyOutput(const QByteArray& bytes, QString* error) const
{
    PdfDocument::OpenError oe;
    QString msg;
    auto doc = PdfDocument::openData(bytes, m_password, &oe, &msg);
    if (!doc) {
        if (error)
            *error = tr("The new PDF cannot be opened: %1").arg(msg);
        return false;
    }
    if (doc->pageCount() != m_original->pageCount()) {
        if (error)
            *error = tr("Page count changed from %1 to %2.").arg(m_original->pageCount()).arg(doc->pageCount());
        return false;
    }
    const QSet<int> touched = changedPages();
    for (int p = 0; p < doc->pageCount(); ++p) {
        const QString newText = normalizeForCompare(doc->pageText(p));
        if (!touched.contains(p)) {
            if (newText != normalizeForCompare(m_original->pageText(p))) {
                if (error)
                    *error = tr("Text on untouched page %1 changed unexpectedly.").arg(p + 1);
                return false;
            }
            continue;
        }
        // Every block's current text must be present; unedited blocks must survive.
        for (const TextBlock& b : blocks(p)) {
            const BlockState st = blockState(p, b.id);
            if (!st.editable)
                continue;
            for (const QString& line : st.text.split(QLatin1Char('\n'))) {
                const QString expect = normalizeForCompare(line);
                if (!expect.isEmpty() && !newText.contains(expect)) {
                    if (error)
                        *error = tr("Text \"%1\" on page %2 is missing after saving.").arg(line.left(60)).arg(p + 1);
                    return false;
                }
            }
        }
        for (const AddedItem& a : addedItems(p)) {
            const QString expect = normalizeForCompare(a.text);
            if (!expect.isEmpty() && !newText.contains(expect)) {
                if (error)
                    *error = tr("Added text on page %1 is missing after saving.").arg(p + 1);
                return false;
            }
        }
    }
    return true;
}

bool PdfEditSession::hasExternalChange() const
{
    return !FileFingerprint::of(m_path).sameContentAs(m_fingerprint);
}

QStringList PdfEditSession::preSaveWarnings() const
{
    QStringList w;
    if (m_analysis.signatureCount > 0)
        w << tr("This PDF is digitally signed. Saving changes will invalidate the signature "
                "(the signed version is kept inside the file as an earlier revision).");
    if (m_analysis.encrypted) {
        QByteArray out;
        if (serialize(&out, nullptr)) {
            PdfDocument::OpenError oe;
            auto probe = PdfDocument::openData(out, {}, &oe, nullptr);
            if (probe)
                w << tr("The saved file will no longer be password protected.");
        }
    }
    if (!m_analysis.canModify)
        w << tr("The document's permissions do not allow modification. Only continue if you are entitled to edit it.");
    return w;
}

PdfEditSession::SaveResult PdfEditSession::save(const SaveOptions& options)
{
    SaveResult r;
    QByteArray bytes;
    QString err;
    if (!serialize(&bytes, &err) || !verifyOutput(bytes, &err)) {
        r.status = SafeFileWriter::Status::ValidationFailed;
        r.error = err;
        return r;
    }
    SafeSaveOptions so;
    so.createBackup = options.createBackup;
    so.backupDir = options.backupDir;
    if (!options.ignoreExternalChange)
        so.expectedOriginal = m_fingerprint;
    const auto res = SafeFileWriter::replace(
        m_path,
        [&bytes](const QString& temp, QString* e) {
            QFile f(temp);
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.flush()) {
                if (e)
                    *e = f.errorString();
                return false;
            }
            return true;
        },
        [this](const QString& temp, QString* e) {
            QFile f(temp);
            if (!f.open(QIODevice::ReadOnly))
                return false;
            return verifyOutput(f.readAll(), e);
        },
        so);
    r.status = res.status;
    r.error = res.error;
    r.backupPath = res.backupPath;
    r.ok = res.ok();
    r.warnings = m_warnings;
    if (r.ok) {
        const bool was = isModified();
        m_fingerprint = FileFingerprint::of(m_path);
        m_savedCursor = m_cursor;
        discardRecovery();
        if (was)
            Q_EMIT modifiedChanged(false);
    }
    return r;
}

PdfEditSession::SaveResult PdfEditSession::saveAs(const QString& path, bool overwrite)
{
    SaveResult r;
    QByteArray bytes;
    QString err;
    if (!serialize(&bytes, &err) || !verifyOutput(bytes, &err)) {
        r.status = SafeFileWriter::Status::ValidationFailed;
        r.error = err;
        return r;
    }
    const auto res = SafeFileWriter::writeNew(
        path,
        [&bytes](const QString& temp, QString* e) {
            QFile f(temp);
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
                if (e)
                    *e = f.errorString();
                return false;
            }
            return true;
        },
        [this](const QString& temp, QString* e) {
            QFile f(temp);
            return f.open(QIODevice::ReadOnly) && verifyOutput(f.readAll(), e);
        },
        overwrite);
    r.status = res.status;
    r.error = res.error;
    r.backupPath = res.backupPath;
    r.ok = res.ok();
    return r;
}

// ---- Recovery ---------------------------------------------------------------------

QString PdfEditSession::recoveryPath() const
{
    const QByteArray h = QCryptographicHash::hash(m_path.toLower().toUtf8(), QCryptographicHash::Sha1).toHex();
    return PathUtils::recoveryDir() + "/" + QString::fromLatin1(h) + ".json";
}

void PdfEditSession::writeRecovery() const
{
    if (m_path.isEmpty())
        return;
    if (!isModified()) {
        discardRecovery();
        return;
    }
    QJsonArray ops;
    for (const EditOp& op : activeOps())
        ops.append(op.toJson());
    const QJsonObject root{{"version", 1},
                           {"path", m_path},
                           {"sha256", QString::fromLatin1(m_fingerprint.sha256.toHex())},
                           {"savedAt", QDateTime::currentDateTime().toString(Qt::ISODate)},
                           {"ops", ops}};
    QSaveFile f(recoveryPath());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void PdfEditSession::discardRecovery() const
{
    if (!m_path.isEmpty())
        QFile::remove(recoveryPath());
}

QList<RecoveryInfo> PdfEditSession::pendingRecoveries()
{
    QList<RecoveryInfo> list;
    for (const QFileInfo& fi : QDir(PathUtils::recoveryDir()).entryInfoList({"*.json"}, QDir::Files)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        RecoveryInfo info;
        info.recoveryFile = fi.absoluteFilePath();
        info.documentPath = o.value("path").toString();
        info.savedAt = QDateTime::fromString(o.value("savedAt").toString(), Qt::ISODate);
        info.editCount = int(o.value("ops").toArray().size());
        if (!info.documentPath.isEmpty() && info.editCount > 0)
            list << info;
    }
    return list;
}

void PdfEditSession::deleteRecoveryFile(const QString& recoveryFile) { QFile::remove(recoveryFile); }

bool PdfEditSession::restoreFromRecovery(const QString& recoveryFile, QString* error)
{
    QFile f(recoveryFile);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (QString::fromLatin1(m_fingerprint.sha256.toHex()) != o.value("sha256").toString()) {
        if (error)
            *error = tr("The document changed since the unsaved edits were recorded; they cannot be restored safely.");
        return false;
    }
    QList<EditOp> ops;
    for (const QJsonValue& v : o.value("ops").toArray())
        ops << EditOp::fromJson(v.toObject());
    m_ops = ops;
    m_cursor = int(ops.size());
    m_savedCursor = 0;
    QString e;
    if (!rebuild(&e)) {
        m_ops.clear();
        m_cursor = 0;
        rebuild(nullptr);
        if (error)
            *error = e;
        return false;
    }
    Q_EMIT documentChanged(changedPages());
    Q_EMIT modifiedChanged(true);
    return true;
}

} // namespace folvio
