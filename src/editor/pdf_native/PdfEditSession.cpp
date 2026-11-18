#include "editor/pdf_native/PdfEditSession.h"

#include "core/files/PathUtils.h"
#include "editor/pdf_native/FontMatcher.h"
#include "editor/pdf_native/PdfTextWriter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace folvio {

using namespace pdfedit;

namespace {

quint64 key(int page, int id) { return (quint64(quint32(page)) << 32) | quint32(id); }

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

QStringList wrapText(const QString& line, const WriterFont& font, double rawSize, double scaleX, double maxWidth)
{
    auto width = [&](const QString& s) { return font.advance(s, rawSize) * scaleX; };
    if (maxWidth <= 0 || width(line) <= maxWidth * 1.01)
        return {line};
    QStringList out;
    QString cur;
    for (const QString& w : line.split(QLatin1Char(' '))) {
        const QString candidate = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
        if (!cur.isEmpty() && width(candidate) > maxWidth * 1.01) {
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

QString shorten(const QString& s, int n = 60)
{
    return s.size() > n ? s.left(n) + QStringLiteral("…") : s;
}

// Order-independent comparison of object texts.
QStringList sortedCompact(const QStringList& texts)
{
    QStringList out;
    for (const QString& t : texts) {
        const QString c = compact(t);
        if (!c.isEmpty())
            out << c;
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

// ---- EditOp serialization ---------------------------------------------------

QJsonObject EditOp::toJson() const
{
    QJsonObject o{{"kind", int(kind)}, {"page", page}, {"blockId", blockId}, {"addedId", addedId}, {"text", text},
                  {"batch", batch}, {"style", styleToJson(style)}, {"styleChanged", styleChanged},
                  {"ox", origin.x()}, {"oy", origin.y()}, {"width", width}};
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
    e.batch = o.value("batch").toInt();
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
    m_records.clear();
    m_forceSystemFont.clear();
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
        it = m_blocks.insert(page, m_original ? PdfTextLayout::build(runs, page, m_original->horizontalRules(page),
                                                                   m_original->graphicBounds(page))
                                              : QList<TextBlock>{});
    }
    return *it;
}

QSet<char32_t> PdfEditSession::embeddedFontChars(const QString& fontName) const
{
    // Characters PDFium extracted from objects using this font: they have a glyph in the
    // (subset) font and a ToUnicode entry, so the font can write them again.
    QSet<char32_t> chars;
    for (auto it = m_runs.cbegin(); it != m_runs.cend(); ++it)
        for (const PdfTextRun& r : it.value())
            if (r.fontName == fontName) {
                for (char32_t c : r.text.toUcs4())
                    if (c != 0xFFFD && c != 0xFFFE)
                        chars.insert(c);
                if (r.spaceAfter && r.text.contains(QLatin1Char(' ')))
                    chars.insert(U' ');
            }
    return chars;
}

QSet<char32_t> PdfEditSession::writeCharset() const
{
    QSet<char32_t> set;
    for (char32_t c = 0x20; c <= 0x7E; ++c)
        set.insert(c);
    for (char32_t c = 0xA1; c <= 0x17F; ++c)
        set.insert(c);
    for (char32_t c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x2030,
                       0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2192, 0x2212, 0x00D7})
        set.insert(char32_t(c));
    for (int i = 0; i < m_cursor; ++i) {
        for (char32_t c : m_ops[i].text.toUcs4())
            set.insert(c);
        for (const OcrWordPdf& w : m_ops[i].words)
            for (char32_t c : w.text.toUcs4())
                set.insert(c);
    }
    set.remove(U'\n');
    return set;
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

bool PdfEditSession::applyBatch(const QList<EditOp>& ops, QString* error)
{
    if (ops.isEmpty())
        return true;
    if (ops.size() == 1)
        return apply(ops.first(), error);
    for (const EditOp& op : ops)
        if (op.page < 0 || op.page >= pageCount()) {
            if (error)
                *error = tr("Invalid page.");
            return false;
        }
    const bool wasModified = isModified();
    const QList<EditOp> backup = m_ops;
    const int backupCursor = m_cursor, backupSaved = m_savedCursor;
    m_ops = m_ops.mid(0, m_cursor);
    if (m_savedCursor > m_cursor)
        m_savedCursor = -1;
    int batchId = 1;
    for (const EditOp& existing : m_ops)
        batchId = std::max(batchId, existing.batch + 1);
    for (EditOp op : ops) {
        op.batch = batchId;
        op.text = sanitizeText(op.text);
        m_ops.append(op);
    }
    m_cursor = int(m_ops.size());
    QString e;
    if (!rebuild(&e)) {
        m_ops = backup;
        m_cursor = backupCursor;
        m_savedCursor = backupSaved;
        rebuild(nullptr);
        if (error)
            *error = e;
        return false;
    }
    QSet<int> pages;
    for (const EditOp& op : ops)
        pages.insert(op.page);
    Q_EMIT documentChanged(pages);
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

bool PdfEditSession::apply(const EditOp& opIn, QString* error)
{
    if (!m_original) {
        if (error)
            *error = tr("No document is open.");
        return false;
    }
    if (opIn.page < 0 || opIn.page >= pageCount()) {
        if (error)
            *error = tr("Invalid page.");
        return false;
    }
    EditOp op = opIn;
    op.text = sanitizeText(op.text);
    const bool wasModified = isModified();
    const QList<EditOp> backup = m_ops;
    const int backupCursor = m_cursor, backupSaved = m_savedCursor;
    m_ops = m_ops.mid(0, m_cursor);
    if (m_savedCursor > m_cursor)
        m_savedCursor = -1; // the saved state is no longer reachable by undo/redo
    m_ops.append(op);
    m_cursor = int(m_ops.size());
    QString e;
    if (!rebuild(&e)) {
        // The document stays exactly as before the attempted edit.
        m_ops = backup;
        m_cursor = backupCursor;
        m_savedCursor = backupSaved;
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
    QSet<int> pages;
    const int batch = m_ops[m_cursor - 1].batch;
    do {
        pages.insert(m_ops[m_cursor - 1].page);
        --m_cursor;
    } while (batch != 0 && m_cursor > 0 && m_ops[m_cursor - 1].batch == batch);
    rebuild(nullptr);
    Q_EMIT documentChanged(pages);
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

bool PdfEditSession::redo()
{
    if (!canRedo())
        return false;
    const bool wasModified = isModified();
    QSet<int> pages;
    const int batch = m_ops[m_cursor].batch;
    do {
        pages.insert(m_ops[m_cursor].page);
        ++m_cursor;
    } while (batch != 0 && m_cursor < m_ops.size() && m_ops[m_cursor].batch == batch);
    rebuild(nullptr);
    Q_EMIT documentChanged(pages);
    if (wasModified != isModified())
        Q_EMIT modifiedChanged(isModified());
    return true;
}

// ---------------------------------------------------------------------------
// Rebuild: replay all active operations on the pristine original bytes.

bool PdfEditSession::rebuild(QString* error)
{
    // A line whose original-font rendering does not read back correctly is retried with a
    // system font; m_forceSystemFont remembers those lines across attempts.
    for (int attempt = 0; attempt < 3; ++attempt) {
        m_warnings.clear();
        m_currentBounds.clear();
        m_added.clear();
        m_records.clear();
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

        QMap<int, QList<int>> opsByPage;
        for (int i = 0; i < m_cursor; ++i)
            opsByPage[m_ops[i].page] << i;

        std::lock_guard lock(pdfiumMutex());
        FontProvider fonts(doc->handle(), writeCharset());
        bool retry = false;

        for (auto pit = opsByPage.cbegin(); pit != opsByPage.cend() && !retry; ++pit) {
            const int pageIndex = pit.key();
            // Effective operations: the last op for a block / added item wins.
            QMap<int, int> lastForBlock, lastForAdded;
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
            const QList<QRectF> graphics = m_original->graphicBounds(pageIndex);
            PageRecord& record = m_records[pageIndex];

            FPDF_PAGE page = FPDF_LoadPage(doc->handle(), pageIndex);
            if (!page) {
                if (error)
                    *error = tr("Page %1 could not be loaded.").arg(pageIndex + 1);
                return false;
            }
            struct PageCloser {
                FPDF_PAGE p;
                ~PageCloser() { FPDF_ClosePage(p); }
            } closer{page};
            const double pageWidth = FPDF_GetPageWidthF(page);
            const double pageHeight = FPDF_GetPageHeightF(page);

            // Snapshot original object handles and the fonts they use before anything is removed.
            const int objCount = FPDFPage_CountObjects(page);
            std::vector<FPDF_PAGEOBJECT> handles(size_t(std::max(0, objCount)));
            for (int i = 0; i < objCount; ++i)
                handles[size_t(i)] = FPDFPage_GetObject(page, i);
            QHash<QString, FPDF_FONT> pageFonts;
            for (const PdfTextRun& r : runs)
                if (!pageFonts.contains(r.fontName) && r.objectIndex >= 0 && r.objectIndex < objCount)
                    pageFonts.insert(r.fontName, FPDFTextObj_GetFont(handles[size_t(r.objectIndex)]));
            auto removeObject = [&](int index) {
                if (index >= 0 && index < objCount && handles[size_t(index)]) {
                    if (FPDFPage_RemoveObject(page, handles[size_t(index)]))
                        FPDFPageObj_Destroy(handles[size_t(index)]);
                    handles[size_t(index)] = nullptr;
                    record.removedObjects << index;
                }
            };
            QList<PlacedLine> placed;
            QList<QString> placedKeys; // retry keys for original-font lines

            // Regions of the page that other content occupies (for collision checks).
            auto occupiedBy = [&](const QRectF& area, int ownBlock, const QRectF& ownOriginal) -> QString {
                const QRectF a = area.adjusted(0.6, 0.6, -0.6, -0.6);
                for (const TextBlock& other : pageBlocks) {
                    if (other.id == ownBlock)
                        continue;
                    const BlockState st = blockState(pageIndex, other.id);
                    const QRectF r = st.edited && st.text.isEmpty() ? QRectF() : (m_currentBounds.contains(key(pageIndex, other.id))
                                                                                     ? m_currentBounds[key(pageIndex, other.id)]
                                                                                     : other.bounds);
                    if (!r.isNull() && r.intersects(a))
                        return other.text().split('\n').value(0);
                }
                for (const QRectF& g : graphics) {
                    // Graphics that already overlapped the paragraph (backgrounds, boxes) are not obstacles.
                    if (g.intersects(ownOriginal.adjusted(-1, -1, 1, 1)))
                        continue;
                    if (g.intersects(a))
                        return tr("a graphic element");
                }
                return {};
            };

            // --- block replacements -------------------------------------------
            for (auto it = lastForBlock.cbegin(); it != lastForBlock.cend(); ++it) {
                const EditOp& op = m_ops[it.value()];
                if (op.blockId < 0 || op.blockId >= pageBlocks.size())
                    continue;
                const TextBlock& block = pageBlocks[op.blockId];
                if (!block.editable) {
                    if (error)
                        *error = tr("This text cannot be edited in place (rotated text or an OCR layer).");
                    return false;
                }
                TextStyle original = styleForRun(block.style);
                original.align = detectAlignment(block);
                const TextStyle style = op.styleChanged ? op.style : original;
                const bool styleChanged = op.styleChanged && style != original;
                const bool fontChanged = styleChanged && (style.family != original.family || style.bold != original.bold
                                                          || style.italic != original.italic);

                QStringList oldLines;
                for (const TextLine& l : block.lines)
                    oldLines << l.text;
                const QStringList newLines = op.text.isEmpty() ? QStringList() : op.text.split(QLatin1Char('\n'));
                const QList<LineOp> diff = [&] {
                    if (!styleChanged)
                        return diffLines(oldLines, newLines);
                    QList<LineOp> all;
                    for (int i = 0; i < oldLines.size(); ++i)
                        all.append({'d', i, -1});
                    for (int j = 0; j < newLines.size(); ++j)
                        all.append({'i', -1, j});
                    return all;
                }();

                // Lines whose characters PDFium cannot map to Unicode cannot be rewritten faithfully.
                for (const LineOp& d : diff) {
                    const QString t = d.type == 'd' ? oldLines[d.oldIndex] : d.type == 'i' ? newLines[d.newIndex] : QString();
                    if (t.contains(QChar(0xFFFD))) {
                        if (error)
                            *error = tr("The line \"%1\" contains characters that cannot be read from this PDF (shown as �). "
                                        "Folvio will not rewrite it, because that would corrupt those characters. Edit other "
                                        "lines, or use Reflow edit.").arg(shorten(t));
                        return false;
                    }
                }

                const PdfTextRun& base = block.style;
                const double scaleY = std::max(1e-6, std::hypot(base.matrix.m21(), base.matrix.m22()));
                const double scaleX = std::max(1e-6, std::hypot(base.matrix.m11(), base.matrix.m12()));
                const double rawSize = styleChanged || base.fontSize <= 0 ? style.size / scaleY : base.fontSize;
                const double visualSize = rawSize * scaleY;
                const double spacing = block.lineSpacing > 0 ? block.lineSpacing : visualSize * 1.2;
                const double blockLeft = block.bounds.left();
                const double blockRight = block.bounds.right();
                const double blockWidth = std::max(1.0, blockRight - blockLeft);
                const bool wrap = block.lineCount() > 1;
                // Wrapping width: up to the page margin or the nearest text to the right in the same band.
                double rightLimit = pageWidth - std::clamp(blockLeft, 36.0, 90.0);
                for (const TextBlock& other : pageBlocks) {
                    if (other.id == block.id || other.bounds.left() < blockRight - 1.0)
                        continue;
                    if (other.bounds.top() < block.bounds.bottom() + spacing && other.bounds.bottom() > block.bounds.top() - spacing)
                        rightLimit = std::min(rightLimit, other.bounds.left() - 0.5 * visualSize);
                }
                const double wrapWidth = std::max(blockWidth, rightLimit - blockLeft);

                // Font for new lines: the document's own embedded font when it can represent every
                // character (identical look and metrics), otherwise an installed font with exact ToUnicode.
                const FontMatch fm = FontMatcher::byFamily(style.family, style.bold, style.italic).isValid()
                                         ? FontMatcher::byFamily(style.family, style.bold, style.italic)
                                         : FontMatcher::match(base.fontName, style.bold, style.italic, base.serif, base.monospace);
                auto chooseFont = [&](const QString& text, const PdfTextRun& ref, bool* original) -> WriterFont* {
                    const QString retryKey = QStringLiteral("%1:%2:%3").arg(pageIndex).arg(op.blockId).arg(text);
                    if (!fontChanged && !m_forceSystemFont.contains(retryKey) && pageFonts.value(ref.fontName)) {
                        WriterFont* f = fonts.original(ref.fontName, pageFonts.value(ref.fontName), embeddedFontChars(ref.fontName));
                        if (f && f->supports(text)) {
                            *original = true;
                            return f;
                        }
                    }
                    *original = false;
                    QString e;
                    WriterFont* f = fm.isValid() ? fonts.system(fm.filePath, &e) : nullptr;
                    return f;
                };

                // ---- plan ------------------------------------------------------
                struct Piece {
                    QString text;
                    double baseline;
                    double x;
                    WriterFont* font;
                    double rawSize;
                    QTransform matrix;
                    int insertBeforeLine; // old line index whose position the piece takes (-1 = append)
                };
                QList<Piece> pieces;
                QList<std::pair<int, double>> kept; // old line index, vertical shift
                QList<int> deletedLines;
                double shift = 0;
                double prevBaseline = block.lines.first().baseline + spacing;
                int produced = 0;
                bool usedSystemFont = false;
                int k = 0;
                while (k < diff.size()) {
                    if (diff[k].type == 'k') {
                        const TextLine& line = block.lines[diff[k].oldIndex];
                        kept.append({diff[k].oldIndex, shift});
                        prevBaseline = line.baseline + shift;
                        ++produced;
                        ++k;
                        continue;
                    }
                    QList<int> deleted, inserted;
                    while (k < diff.size() && diff[k].type != 'k') {
                        if (diff[k].type == 'd')
                            deleted << diff[k].oldIndex;
                        else
                            inserted << diff[k].newIndex;
                        ++k;
                    }
                    deletedLines << deleted;
                    // Style reference: the first replaced line's dominant run (keeps per-line size/font).
                    const TextLine& refLine = block.lines[deleted.isEmpty() ? std::min(int(block.lines.size()) - 1, std::max(0, produced - 1))
                                                                            : deleted.first()];
                    int refRun = refLine.runs.first();
                    for (int r : refLine.runs)
                        if (runs[r].text.size() > runs[refRun].text.size())
                            refRun = r;
                    const PdfTextRun& ref = styleChanged ? base : runs[refRun];
                    QStringList regionTexts;
                    for (int j : inserted)
                        regionTexts << newLines[j];
                    bool isOriginal = false;
                    WriterFont* font = chooseFont(regionTexts.join(QLatin1Char(' ')), ref, &isOriginal);
                    if (!font) {
                        if (error)
                            *error = tr("No suitable font is available to write the edited text.");
                        return false;
                    }
                    if (!font->supports(regionTexts.join(QString()))) {
                        if (error)
                            *error = tr("The font %1 cannot display some of the characters you typed.").arg(font->name);
                        return false;
                    }
                    usedSystemFont |= !isOriginal;
                    const double pieceRaw = isOriginal && !styleChanged && ref.fontSize > 0 ? ref.fontSize : rawSize;
                    const QTransform refMatrix = isOriginal && !styleChanged ? ref.matrix : base.matrix;
                    const double refScaleX = std::max(1e-6, std::hypot(refMatrix.m11(), refMatrix.m12()));
                    QStringList regionPieces;
                    for (int j : inserted)
                        regionPieces << (wrap ? wrapText(newLines[j], *font, pieceRaw, refScaleX, wrapWidth) : QStringList{newLines[j]});
                    for (int p = 0; p < regionPieces.size(); ++p) {
                        double baseline;
                        double origLeft = -1;
                        if (p < deleted.size()) {
                            const TextLine& slot = block.lines[deleted[p]];
                            baseline = slot.baseline + shift;
                            origLeft = slot.left;
                        } else {
                            baseline = prevBaseline - spacing;
                        }
                        const double w = font->advance(regionPieces[p], pieceRaw) * refScaleX;
                        double x;
                        switch (style.align) {
                        case TextStyle::Center: x = blockLeft + (blockWidth - w) / 2.0; break;
                        case TextStyle::Right: x = blockRight - w; break;
                        default: x = styleChanged || origLeft < 0 ? (origLeft < 0 ? block.lines.first().left : blockLeft) : origLeft; break;
                        }
                        pieces.append({regionPieces[p], baseline, x, font, pieceRaw,
                                       QTransform(refMatrix.m11(), refMatrix.m12(), refMatrix.m21(), refMatrix.m22(), x, baseline),
                                       p < deleted.size() ? deleted[p] : (deleted.isEmpty() ? -1 : deleted.last())});
                        prevBaseline = baseline;
                        ++produced;
                    }
                    shift += double(deleted.size() - regionPieces.size()) * spacing;
                }

                // ---- safety checks before touching the page ----------------------
                QRectF finalArea;
                for (const Piece& pc : pieces) {
                    const double w = pc.font->advance(pc.text, pc.rawSize) * std::hypot(pc.matrix.m11(), pc.matrix.m12());
                    const double sz = pc.rawSize * std::hypot(pc.matrix.m21(), pc.matrix.m22());
                    const QRectF r(pc.x, pc.baseline - 0.25 * sz, w, 1.05 * sz);
                    finalArea = finalArea.isNull() ? r : finalArea.united(r);
                    if (r.right() > pageWidth + 0.5 || r.left() < -0.5 || r.bottom() > pageHeight + 0.5 || r.top() < -0.5) {
                        if (error)
                            *error = tr("The edited text would extend beyond the page. Shorten it, or use Reflow edit.");
                        return false;
                    }
                }
                for (const auto& [lineIndex, dy] : kept)
                    finalArea = finalArea.isNull() ? block.lines[lineIndex].bounds.translated(0, dy)
                                                   : finalArea.united(block.lines[lineIndex].bounds.translated(0, dy));
                // Only the part outside the paragraph's original area can collide with neighbours.
                if (!finalArea.isNull()) {
                    const QRectF own = block.bounds.adjusted(-0.5, -0.5, 0.5, 0.5);
                    QList<QRectF> outside;
                    if (finalArea.top() < own.top())
                        outside << QRectF(finalArea.left(), finalArea.top(), finalArea.width(), own.top() - finalArea.top());
                    if (finalArea.right() > own.right())
                        outside << QRectF(own.right(), finalArea.top(), finalArea.right() - own.right(), finalArea.height());
                    if (finalArea.left() < own.left())
                        outside << QRectF(finalArea.left(), finalArea.top(), own.left() - finalArea.left(), finalArea.height());
                    if (finalArea.bottom() > own.bottom())
                        outside << QRectF(finalArea.left(), own.bottom(), finalArea.width(), finalArea.bottom() - own.bottom());
                    for (const QRectF& area : outside) {
                        const QString hit = occupiedBy(area, block.id, block.bounds);
                        if (!hit.isEmpty()) {
                            if (error)
                                *error = tr("The edited paragraph needs more space than it has and would overlap \"%1\". "
                                            "Folvio does not move other content. Shorten the text (%n line(s) available), "
                                            "or use Reflow edit for larger changes.", nullptr, block.lineCount())
                                             .arg(shorten(hit, 40));
                            return false;
                        }
                    }
                }

                // ---- execute ------------------------------------------------------
                QRectF newBounds;
                for (const auto& [lineIndex, dy] : kept) {
                    const TextLine& line = block.lines[lineIndex];
                    if (std::abs(dy) > 0.01)
                        for (int r : line.runs) {
                            const int oi = runs[r].objectIndex;
                            if (oi >= 0 && oi < objCount && handles[size_t(oi)])
                                FPDFPageObj_Transform(handles[size_t(oi)], 1, 0, 0, 1, 0, dy);
                        }
                    newBounds = newBounds.isNull() ? line.bounds.translated(0, dy) : newBounds.united(line.bounds.translated(0, dy));
                }
                for (const Piece& pc : pieces) {
                    int insertAt = -1;
                    if (pc.insertBeforeLine >= 0)
                        for (int r : block.lines[pc.insertBeforeLine].runs) {
                            const int oi = runs[r].objectIndex;
                            if (oi >= 0 && oi < objCount && handles[size_t(oi)]) {
                                insertAt = indexOfObject(page, handles[size_t(oi)]);
                                break;
                            }
                        }
                    if (pc.text.isEmpty())
                        continue;
                    PlacedLine pl = placeLine(doc->handle(), page, *pc.font, pc.rawSize, pc.text, pc.matrix, style.color, 0,
                                              insertAt >= 0 ? &insertAt : nullptr);
                    if (pl.objects.isEmpty()) {
                        if (error)
                            *error = tr("PDFium could not create the text object.");
                        return false;
                    }
                    for (FPDF_PAGEOBJECT o : pl.objects)
                        Q_UNUSED(o)
                    record.insertedTexts << (pl.objects.size() == 1 ? QStringList{pc.text} : pc.text.split(' ', Qt::SkipEmptyParts));
                    record.expectedLines << pc.text;
                    if (!pl.bounds.isNull())
                        newBounds = newBounds.isNull() ? pl.bounds : newBounds.united(pl.bounds);
                    placedKeys << (pc.font->original ? QStringLiteral("%1:%2:%3").arg(pageIndex).arg(op.blockId).arg(pc.text) : QString());
                    placed << pl;
                }
                for (int i : deletedLines)
                    for (int r : block.lines[i].runs)
                        removeObject(runs[r].objectIndex);
                if (usedSystemFont && !fm.exact && !block.style.fontName.isEmpty())
                    m_warnings << tr("Some characters are not available in the embedded font \"%1\"; the changed line uses "
                                     "%2 instead.").arg(block.style.fontName, fm.family);
                if (block.mixedStyle && !deletedLines.isEmpty())
                    m_warnings << tr("This paragraph had mixed formatting; changed lines use a single style.");
                if (produced > block.lineCount())
                    m_warnings << tr("The paragraph grew by %n line(s) into free space below it.", nullptr, produced - block.lineCount());
                m_currentBounds.insert(key(pageIndex, op.blockId), newBounds.isNull() ? block.bounds : newBounds);
            }

            // --- added text boxes ----------------------------------------------
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
                QString e;
                WriterFont* font = fm.isValid() ? fonts.system(fm.filePath, &e) : nullptr;
                if (!font || !font->supports(op.text)) {
                    if (error)
                        *error = font ? tr("The font %1 cannot display some of the characters you typed.").arg(font->name) : e;
                    return false;
                }
                const double size = std::max(1.0, op.style.size);
                double baseline = op.origin.y() - size * 0.95;
                QRectF bounds;
                for (const QString& para : op.text.split(QLatin1Char('\n'))) {
                    for (const QString& w : wrapText(para, *font, size, 1.0, op.width)) {
                        const double tw = font->advance(w, size);
                        double x = op.origin.x();
                        if (op.width > 0 && op.style.align == TextStyle::Center)
                            x += (op.width - tw) / 2;
                        else if (op.width > 0 && op.style.align == TextStyle::Right)
                            x += op.width - tw;
                        if (!w.isEmpty()) {
                            PlacedLine pl = placeLine(doc->handle(), page, *font, size, w, QTransform(1, 0, 0, 1, x, baseline),
                                                      op.style.color, 0, nullptr);
                            record.insertedTexts << (pl.objects.size() == 1 ? QStringList{w} : w.split(' ', Qt::SkipEmptyParts));
                            record.expectedLines << w;
                            if (!pl.bounds.isNull())
                                bounds = bounds.isNull() ? pl.bounds : bounds.united(pl.bounds);
                            placedKeys << QString();
                            placed << pl;
                        }
                        baseline -= size * 1.2;
                    }
                }
                if (bounds.isNull())
                    bounds = QRectF(op.origin.x(), op.origin.y() - size, std::max(op.width, size), size);
                item.bounds = bounds;
                m_added.insert(key(pageIndex, op.addedId), item);
            }

            // --- images -------------------------------------------------------------
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

            // --- OCR text layer (invisible; the scanned image stays untouched) --------
            for (int i : ocr) {
                const EditOp& op = m_ops[i];
                const FontMatch fm = FontMatcher::byFamily("Arial", false, false);
                QString e;
                WriterFont* font = fm.isValid() ? fonts.system(fm.filePath, &e) : nullptr;
                if (!font) {
                    if (error)
                        *error = e;
                    return false;
                }
                for (const OcrWordPdf& w : op.words) {
                    const QString text = sanitizeText(w.text).trimmed();
                    if (text.isEmpty() || w.rect.isEmpty() || !font->supports(text))
                        continue;
                    const double size = std::max(1.0, w.rect.height() * 0.9);
                    const double natural = std::max(0.1, font->advance(text, size));
                    const double sx = w.rect.width() / natural;
                    PlacedLine pl = placeLine(doc->handle(), page, *font, size, text,
                                              QTransform(sx, 0, 0, 1, w.rect.left(), w.rect.top() + w.rect.height() * 0.2),
                                              Qt::black, 3 /* invisible */, nullptr);
                    record.insertedTexts << (pl.objects.size() == 1 ? QStringList{text} : text.split(' ', Qt::SkipEmptyParts));
                    record.expectedLines << text;
                    placedKeys << QString();
                    placed << pl;
                }
            }

            // --- read back everything written on this page --------------------------
            QStringList details;
            const QList<int> bad = verifyReadBack(page, placed, &details);
            if (!bad.isEmpty()) {
                bool canRetry = false;
                for (int b : bad)
                    if (!placedKeys.value(b).isEmpty() && !m_forceSystemFont.contains(placedKeys[b])) {
                        m_forceSystemFont.insert(placedKeys[b]);
                        canRetry = true;
                    }
                if (canRetry) {
                    retry = true;
                    break;
                }
                if (error)
                    *error = tr("The edited text could not be written exactly (%1). Nothing was changed.").arg(details.join("; "));
                return false;
            }

            if (!FPDFPage_GenerateContent(page)) {
                if (error)
                    *error = tr("PDFium could not regenerate page %1.").arg(pageIndex + 1);
                return false;
            }
        }
        if (retry)
            continue;
        m_warnings.removeDuplicates();
        m_current = std::move(doc);
        return true;
    }
    if (error)
        *error = tr("The edited text could not be written reliably. Nothing was changed.");
    return false;
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

namespace {
std::atomic_bool g_failVerification{false};
}

void PdfEditSession::setVerificationFailureForTesting(bool fail) { g_failVerification = fail; }

bool PdfEditSession::verifyOutput(const QByteArray& bytes, QString* error) const
{
    auto fail = [error](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (g_failVerification)
        return fail(QStringLiteral("Injected verification failure."));
    PdfDocument::OpenError oe;
    QString msg;
    auto doc = PdfDocument::openData(bytes, m_password, &oe, &msg);
    if (!doc)
        return fail(tr("The new PDF cannot be opened: %1").arg(msg));
    if (doc->pageCount() != m_original->pageCount())
        return fail(tr("Page count changed from %1 to %2.").arg(m_original->pageCount()).arg(doc->pageCount()));

    const QSet<int> touched = changedPages();
    for (int p = 0; p < doc->pageCount(); ++p) {
        if (!touched.contains(p)) {
            // Untouched pages: extracted text must be identical.
            if (compact(doc->pageText(p)) != compact(m_original->pageText(p)))
                return fail(tr("Text on page %1, which was not edited, changed unexpectedly.").arg(p + 1));
            continue;
        }
        // Edited pages are checked object by object - independent of how lines and
        // paragraphs are recognised, and strict about every character.
        const QList<PdfTextRun> outRuns = doc->textRuns(p);
        QStringList outTexts;
        for (const PdfTextRun& r : outRuns)
            outTexts << r.text;

        // 1. Original text that was not intentionally replaced is still there, unchanged,
        //    and the only additions are the new texts.
        const PageRecord record = m_records.value(p);
        QStringList expected;
        const QList<PdfTextRun>& originalRuns = m_runs.value(p);
        const QSet<int> removed(record.removedObjects.begin(), record.removedObjects.end());
        for (const PdfTextRun& r : originalRuns)
            if (!removed.contains(r.objectIndex))
                expected << r.text;
        expected << record.insertedTexts;
        const QStringList want = sortedCompact(expected), got = sortedCompact(outTexts);
        if (want != got) {
            // Report the first concrete difference.
            QStringList missing = want, extra = got;
            for (const QString& g : got)
                missing.removeOne(g);
            for (const QString& w : want)
                extra.removeOne(w);
            if (!missing.isEmpty())
                return fail(tr("Text \"%1\" on page %2 is missing after saving.").arg(shorten(missing.first())).arg(p + 1));
            return fail(tr("Unexpected text \"%1\" appeared on page %2.").arg(shorten(extra.value(0))).arg(p + 1));
        }

        // 2. Every new line is present as written, in content order.
        QString stream;
        for (const QString& t : outTexts)
            stream += compact(t);
        for (const QString& line : record.expectedLines)
            if (!compact(line).isEmpty() && !stream.contains(compact(line)))
                return fail(tr("Text \"%1\" on page %2 is missing after saving.").arg(shorten(line)).arg(p + 1));
        // 3. The saved file contains exactly what was built in memory (serialization round trip).
        if (m_current) {
            QStringList memTexts;
            for (const PdfTextRun& r : m_current->textRuns(p))
                memTexts << r.text;
            if (sortedCompact(memTexts) != sortedCompact(outTexts))
                return fail(tr("Page %1: the saved text objects differ from the edited document.").arg(p + 1));
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
