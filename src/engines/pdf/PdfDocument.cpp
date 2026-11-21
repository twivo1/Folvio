#include "engines/pdf/PdfDocument.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>

#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_signature.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <cmath>
#include <vector>

namespace folvio {

std::recursive_mutex& pdfiumMutex()
{
    static std::recursive_mutex m;
    return m;
}

void ensurePdfiumInitialized()
{
    static std::once_flag once;
    std::call_once(once, [] {
        FPDF_LIBRARY_CONFIG config{};
        config.version = 2;
        config.m_pUserFontPaths = nullptr;
        config.m_pIsolate = nullptr;
        config.m_v8EmbedderSlot = 0;
        FPDF_InitLibraryWithConfig(&config);
    });
}

QString stripSubsetPrefix(const QString& fontName)
{
    static const QRegularExpression subset(QStringLiteral("^[A-Z]{6}\\+"));
    QString n = fontName;
    n.remove(subset);
    return n;
}

QString PdfAnalysis::summary() const
{
    QStringList parts;
    parts << QObject::tr("%n page(s)", nullptr, pageCount);
    if (isScanned)
        parts << QObject::tr("scanned (no text layer)");
    else if (hasTextLayer)
        parts << QObject::tr("text layer present");
    if (totalImages > 0)
        parts << QObject::tr("%n image(s)", nullptr, totalImages);
    if (likelyHasTables)
        parts << QObject::tr("tables/ruled areas detected");
    if (encrypted)
        parts << QObject::tr("encrypted");
    if (signatureCount > 0)
        parts << QObject::tr("%n digital signature(s)", nullptr, signatureCount);
    return parts.join(QStringLiteral(" · "));
}

PdfPageHandle::PdfPageHandle(fpdf_document_t__* doc, int index)
{
    if (doc)
        m_page = FPDF_LoadPage(doc, index);
}

PdfPageHandle::~PdfPageHandle()
{
    if (m_page)
        FPDF_ClosePage(m_page);
}

PdfDocument::~PdfDocument()
{
    std::lock_guard lock(pdfiumMutex());
    if (m_doc)
        FPDF_CloseDocument(m_doc);
}

QString PdfDocument::describe(OpenError e)
{
    switch (e) {
    case OpenError::None: return {};
    case OpenError::File: return QObject::tr("The file could not be read.");
    case OpenError::Format: return QObject::tr("The file is not a valid PDF or is damaged.");
    case OpenError::Password: return QObject::tr("The document is password protected.");
    case OpenError::Security: return QObject::tr("The document uses an unsupported security handler.");
    case OpenError::Unknown: break;
    }
    return QObject::tr("The document could not be opened.");
}

std::unique_ptr<PdfDocument> PdfDocument::openFile(const QString& path, const QByteArray& password, OpenError* error,
                                                   QString* message)
{
    QFile f(path);
    // Refuse absurd inputs before reading them into memory.
    constexpr qint64 kMaxPdfBytes = 2LL * 1024 * 1024 * 1024;
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxPdfBytes) {
        if (error)
            *error = OpenError::File;
        if (message)
            *message = f.size() > kMaxPdfBytes ? QObject::tr("The file is larger than 2 GB.") : f.errorString();
        return nullptr;
    }
    return openData(f.readAll(), password, error, message);
}

std::unique_ptr<PdfDocument> PdfDocument::openData(QByteArray data, const QByteArray& password, OpenError* error,
                                                   QString* message)
{
    ensurePdfiumInitialized();
    std::lock_guard lock(pdfiumMutex());
    std::unique_ptr<PdfDocument> doc(new PdfDocument);
    doc->m_data = std::move(data);
    doc->m_password = password;
    doc->m_doc = FPDF_LoadMemDocument64(doc->m_data.constData(), size_t(doc->m_data.size()),
                                        password.isEmpty() ? nullptr : password.constData());
    if (!doc->m_doc) {
        OpenError e = OpenError::Unknown;
        switch (FPDF_GetLastError()) {
        case FPDF_ERR_FILE: e = OpenError::File; break;
        case FPDF_ERR_FORMAT: e = OpenError::Format; break;
        case FPDF_ERR_PASSWORD: e = OpenError::Password; break;
        case FPDF_ERR_SECURITY: e = OpenError::Security; break;
        default: break;
        }
        if (error)
            *error = e;
        if (message)
            *message = describe(e);
        return nullptr;
    }
    if (FPDF_GetPageCount(doc->m_doc) <= 0) {
        if (error)
            *error = OpenError::Format;
        if (message)
            *message = QObject::tr("The PDF contains no pages.");
        return nullptr;
    }
    if (error)
        *error = OpenError::None;
    return doc;
}

std::unique_ptr<PdfDocument> PdfDocument::createEmpty()
{
    ensurePdfiumInitialized();
    std::lock_guard lock(pdfiumMutex());
    std::unique_ptr<PdfDocument> doc(new PdfDocument);
    doc->m_doc = FPDF_CreateNewDocument();
    return doc;
}

int PdfDocument::pageCount() const
{
    std::lock_guard lock(pdfiumMutex());
    return m_doc ? FPDF_GetPageCount(m_doc) : 0;
}

QSizeF PdfDocument::pageSize(int page) const
{
    std::lock_guard lock(pdfiumMutex());
    FS_SIZEF size{};
    if (m_doc && FPDF_GetPageSizeByIndexF(m_doc, page, &size))
        return {size.width, size.height};
    return {};
}

QTransform PdfDocument::pageToDevice(int page, const QSizeF& deviceSize) const
{
    std::lock_guard lock(pdfiumMutex());
    PdfPageHandle p(m_doc, page);
    if (!p)
        return {};
    // FPDF_PageToDevice returns integers; map into a 1000x larger device for precision.
    constexpr double k = 1000.0;
    const int w = int(deviceSize.width() * k), h = int(deviceSize.height() * k);
    auto map = [&](double x, double y) {
        int dx = 0, dy = 0;
        FPDF_PageToDevice(p.get(), 0, 0, w, h, 0, x, y, &dx, &dy);
        return QPointF(dx / k, dy / k);
    };
    const QPointF o = map(0, 0), ex = map(1000, 0), ey = map(0, 1000);
    return QTransform((ex.x() - o.x()) / 1000.0, (ex.y() - o.y()) / 1000.0, (ey.x() - o.x()) / 1000.0,
                      (ey.y() - o.y()) / 1000.0, o.x(), o.y());
}

QImage PdfDocument::render(int page, double scale, bool withAnnotations) const
{
    std::lock_guard lock(pdfiumMutex());
    PdfPageHandle p(m_doc, page);
    if (!p)
        return {};
    const double wPt = FPDF_GetPageWidthF(p.get());
    const double hPt = FPDF_GetPageHeightF(p.get());
    // Guard against absurd page sizes / zoom levels (memory protection).
    const double maxPixels = 120.0 * 1000 * 1000;
    double s = scale;
    if (wPt * hPt * s * s > maxPixels)
        s = std::sqrt(maxPixels / (wPt * hPt));
    const int w = std::max(1, int(std::lround(wPt * s)));
    const int h = std::max(1, int(std::lround(hPt * s)));

    QImage image(w, h, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    image.fill(Qt::white);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, image.bits(), int(image.bytesPerLine()));
    if (!bmp)
        return {};
    int flags = FPDF_LCD_TEXT;
    if (withAnnotations)
        flags |= FPDF_ANNOT;
    FPDF_RenderPageBitmap(bmp, p.get(), 0, 0, w, h, 0, flags);
    FPDFBitmap_Destroy(bmp);
    return image;
}

QString PdfDocument::pageText(int page) const
{
    std::lock_guard lock(pdfiumMutex());
    PdfPageHandle p(m_doc, page);
    if (!p)
        return {};
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(p.get());
    if (!tp)
        return {};
    const int n = FPDFText_CountChars(tp);
    QString text;
    if (n > 0) {
        std::vector<unsigned short> buf(size_t(n) + 1, 0);
        const int got = FPDFText_GetText(tp, 0, n, buf.data());
        text = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()), std::max(0, got - 1));
    }
    FPDFText_ClosePage(tp);
    return text;
}

QString PdfDocument::allText() const
{
    QStringList pages;
    for (int i = 0; i < pageCount(); ++i)
        pages << pageText(i);
    return pages.join(QChar(0x0C));
}

QList<PdfTextRun> PdfDocument::textRuns(int page) const
{
    std::lock_guard lock(pdfiumMutex());
    QList<PdfTextRun> runs;
    PdfPageHandle p(m_doc, page);
    if (!p)
        return runs;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(p.get());
    const int count = FPDFPage_CountObjects(p.get());
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(p.get(), i);
        if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT)
            continue;
        PdfTextRun run;
        run.objectIndex = i;
        const unsigned long bytes = FPDFTextObj_GetText(obj, tp, nullptr, 0);
        if (bytes > 2) {
            std::vector<unsigned short> buf(bytes / 2 + 1, 0);
            FPDFTextObj_GetText(obj, tp, buf.data(), bytes);
            run.text = QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.data()), int(bytes / 2) - 1);
        }
        float l = 0, b = 0, r = 0, t = 0;
        FPDFPageObj_GetBounds(obj, &l, &b, &r, &t);
        run.bounds = QRectF(QPointF(l, b), QPointF(r, t)).normalized();
        FS_MATRIX m{1, 0, 0, 1, 0, 0};
        FPDFPageObj_GetMatrix(obj, &m);
        run.matrix = QTransform(m.a, m.b, m.c, m.d, m.e, m.f);
        run.axisAligned = std::abs(m.b) < 1e-3 && std::abs(m.c) < 1e-3 && m.a > 0 && m.d > 0;
        float size = 0;
        FPDFTextObj_GetFontSize(obj, &size);
        run.fontSize = size;
        run.effectiveSize = size * std::sqrt(m.c * m.c + m.d * m.d);
        if (run.effectiveSize <= 0)
            run.effectiveSize = run.bounds.height();
        if (FPDF_FONT font = FPDFTextObj_GetFont(obj)) {
            char name[256] = {0};
            const size_t len = FPDFFont_GetBaseFontName(font, name, sizeof(name));
            run.fontName = stripSubsetPrefix(QString::fromUtf8(name, len > 0 ? int(len) - 1 : 0));
            const int flags = FPDFFont_GetFlags(font);
            const int weight = FPDFFont_GetWeight(font);
            const QString lower = run.fontName.toLower();
            run.bold = weight >= 600 || (flags & (1 << 18)) || lower.contains("bold") || lower.contains("black")
                       || lower.contains("heavy") || lower.contains("semibold");
            run.italic = (flags & (1 << 6)) || lower.contains("italic") || lower.contains("oblique");
            run.serif = (flags & (1 << 1)) != 0;
            run.monospace = (flags & 1) != 0;
            run.embedded = FPDFFont_GetIsEmbedded(font);
        }
        unsigned int R = 0, G = 0, B = 0, A = 255;
        if (FPDFPageObj_GetFillColor(obj, &R, &G, &B, &A))
            run.color = QColor(int(R), int(G), int(B), int(A));
        run.renderMode = int(FPDFTextObj_GetTextRenderMode(obj));
        runs.append(run);
    }
    // Use PDFium's text page (advance-width based word segmentation) for the
    // characters of each object and for word boundaries between objects. This
    // avoids inventing spaces between narrow glyphs ("1000" -> "1 000").
    if (tp) {
        QHash<FPDF_PAGEOBJECT, int> runOf;
        for (int r = 0; r < runs.size(); ++r)
            runOf.insert(FPDFPage_GetObject(p.get(), runs[r].objectIndex), r);
        QList<QString> pageTexts(runs.size());
        QList<bool> seen(runs.size(), false);
        const int n = FPDFText_CountChars(tp);
        int last = -1;
        for (int i = 0; i < n; ++i) {
            const unsigned int ch = FPDFText_GetUnicode(tp, i);
            const bool generated = FPDFText_IsGenerated(tp, i) == 1;
            FPDF_PAGEOBJECT o = FPDFText_GetTextObject(tp, i);
            const auto it = o ? runOf.constFind(o) : runOf.constEnd();
            if (!generated && it != runOf.constEnd()) {
                const int r = *it;
                if (ch == ' ' && !pageTexts[r].isEmpty() && r == last)
                    runs[r].spaceAfter = true; // real space inside the object
                if (ch != 0)
                    pageTexts[r] += QString::fromUcs4(reinterpret_cast<const char32_t*>(&ch), 1);
                seen[r] = true;
                last = r;
                continue;
            }
            if (last >= 0 && (ch == ' ' || ch == '\r' || ch == '\n'))
                runs[last].spaceAfter = true;
        }
        for (int r = 0; r < runs.size(); ++r) {
            if (!seen[r])
                continue;
            QString t = pageTexts[r];
            while (!t.isEmpty() && t.back().isSpace()) {
                t.chop(1);
                runs[r].spaceAfter = true;
            }
            runs[r].text = t;
        }
        FPDFText_ClosePage(tp);
    }
    return runs;
}

QList<QRectF> PdfDocument::horizontalRules(int page) const
{
    std::lock_guard lock(pdfiumMutex());
    QList<QRectF> rules;
    PdfPageHandle p(m_doc, page);
    if (!p)
        return rules;
    const int count = FPDFPage_CountObjects(p.get());
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(p.get(), i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_PATH)
            continue;
        float l = 0, b = 0, r = 0, t = 0;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
            continue;
        const QRectF rc = QRectF(QPointF(l, b), QPointF(r, t)).normalized();
        if (rc.height() < 3.0 && rc.width() > 8.0)
            rules << rc;
        else if (rc.height() >= 3.0 && rc.width() > 8.0) {
            // Rectangles (cell borders drawn as boxes): their top and bottom edges act as rules.
            rules << QRectF(rc.left(), rc.top(), rc.width(), 0.5) << QRectF(rc.left(), rc.bottom() - 0.5, rc.width(), 0.5);
        }
    }
    return rules;
}

QList<QRectF> PdfDocument::graphicBounds(int page) const
{
    std::lock_guard lock(pdfiumMutex());
    QList<QRectF> out;
    PdfPageHandle p(m_doc, page);
    if (!p)
        return out;
    const int count = FPDFPage_CountObjects(p.get());
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(p.get(), i);
        if (FPDFPageObj_GetType(obj) == FPDF_PAGEOBJ_TEXT)
            continue;
        float l = 0, b = 0, r = 0, t = 0;
        if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
            out << QRectF(QPointF(l, b), QPointF(r, t)).normalized();
    }
    return out;
}

PdfAnalysis PdfDocument::analyze() const
{
    std::lock_guard lock(pdfiumMutex());
    PdfAnalysis a;
    if (!m_doc)
        return a;
    a.pageCount = FPDF_GetPageCount(m_doc);
    FPDF_GetFileVersion(m_doc, &a.fileVersion);
    a.encrypted = FPDF_GetSecurityHandlerRevision(m_doc) != -1;
    a.signatureCount = FPDF_GetSignatureCount(m_doc);
    const unsigned long perms = FPDF_GetDocPermissions(m_doc);
    a.canModify = !a.encrypted || (perms & (1u << 3));

    for (int i = 0; i < a.pageCount; ++i) {
        PdfPageInfo info;
        PdfPageHandle p(m_doc, i);
        if (!p) {
            a.pages << info;
            continue;
        }
        info.size = {FPDF_GetPageWidthF(p.get()), FPDF_GetPageHeightF(p.get())};
        const double pageArea = std::max(1.0, info.size.width() * info.size.height());
        int thinPaths = 0;
        double imageArea = 0;
        const int count = FPDFPage_CountObjects(p.get());
        for (int k = 0; k < count; ++k) {
            FPDF_PAGEOBJECT obj = FPDFPage_GetObject(p.get(), k);
            float l = 0, b = 0, r = 0, t = 0;
            FPDFPageObj_GetBounds(obj, &l, &b, &r, &t);
            switch (FPDFPageObj_GetType(obj)) {
            case FPDF_PAGEOBJ_TEXT: ++info.textObjects; break;
            case FPDF_PAGEOBJ_IMAGE:
                ++info.imageObjects;
                imageArea += std::abs((r - l) * (t - b));
                break;
            case FPDF_PAGEOBJ_PATH:
                ++info.pathObjects;
                if (std::abs(r - l) < 2.0 || std::abs(t - b) < 2.0)
                    ++thinPaths;
                break;
            case FPDF_PAGEOBJ_FORM: ++info.formObjects; break;
            default: break;
            }
        }
        if (FPDF_TEXTPAGE tp = FPDFText_LoadPage(p.get())) {
            info.charCount = FPDFText_CountChars(tp);
            FPDFText_ClosePage(tp);
        }
        info.imageCoverage = std::min(1.0, imageArea / pageArea);
        info.looksScanned = info.charCount < 8 && info.imageCoverage > 0.45;
        if (info.charCount > 0)
            a.hasTextLayer = true;
        if (info.looksScanned)
            ++a.scannedPages;
        if (thinPaths >= 8)
            a.likelyHasTables = true;
        a.totalImages += info.imageObjects;
        a.pages << info;
    }
    a.isScanned = a.pageCount > 0 && a.scannedPages * 2 >= a.pageCount;
    return a;
}

bool PdfDocument::isEncrypted() const
{
    std::lock_guard lock(pdfiumMutex());
    return m_doc && FPDF_GetSecurityHandlerRevision(m_doc) != -1;
}

int PdfDocument::signatureCount() const
{
    std::lock_guard lock(pdfiumMutex());
    return m_doc ? FPDF_GetSignatureCount(m_doc) : 0;
}

namespace {
struct BufferWriter : FPDF_FILEWRITE {
    QByteArray* out = nullptr;
};
int writeBlock(FPDF_FILEWRITE* self, const void* data, unsigned long size)
{
    auto* w = static_cast<BufferWriter*>(self);
    w->out->append(static_cast<const char*>(data), qsizetype(size));
    return 1;
}
} // namespace

bool PdfDocument::save(QByteArray* out, SaveMode mode, QString* error) const
{
    std::lock_guard lock(pdfiumMutex());
    out->clear();
    BufferWriter w;
    w.version = 1;
    w.WriteBlock = &writeBlock;
    w.out = out;
    const FPDF_DWORD flags = mode == SaveMode::Incremental ? FPDF_INCREMENTAL : FPDF_NO_INCREMENTAL;
    if (!m_doc || !FPDF_SaveAsCopy(m_doc, &w, flags) || out->isEmpty()) {
        if (error)
            *error = QObject::tr("PDFium could not serialize the document.");
        return false;
    }
    return true;
}

} // namespace folvio
