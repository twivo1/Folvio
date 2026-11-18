#include "combine/CombinePdfExporter.h"

#include "combine/AnnotationRender.h"
#include "combine/ImageGeometry.h"
#include "core/files/SafeFileWriter.h"
#include "editor/pdf_native/PdfTextWriter.h"
#include "engines/pdf/PdfDocument.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QPainterPath>

#include <fpdf_edit.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <cmath>
#include <cstring>

namespace folvio::combine {

using namespace folvio::pdfedit;

namespace {

constexpr double kMm = 72.0 / 25.4;

QSizeF basePageSize(const ExportSettings& s)
{
    switch (s.pageSize) {
    case PageSizeMode::A4: return {210 * kMm, 297 * kMm};
    case PageSizeMode::Letter: return {612, 792};
    case PageSizeMode::Custom:
        return {std::max(20.0, s.customSizeMm.width()) * kMm, std::max(20.0, s.customSizeMm.height()) * kMm};
    case PageSizeMode::Auto: break;
    }
    return {};
}

bool isJpegFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(3) == QByteArray("\xFF\xD8\xFF", 3);
}

// The page needs no pixel processing: original JPEG bytes can be embedded unchanged.
bool canPassThrough(const CombinePage& page, const ExportSettings& s)
{
    if (s.quality == QualityMode::SmallFile || page.rotation || page.quad || page.crop || page.fromClipboard)
        return false;
    if (!isJpegFile(page.sourcePath))
        return false;
    QImageReader r(page.sourcePath);
    if (r.transformation() != QImageIOHandler::TransformationNone)
        return false; // EXIF orientation must be applied by re-rendering
    // CMYK JPEGs render differently across viewers; re-encode those as RGB.
    const QImage::Format f = r.imageFormat();
    return f != QImage::Format_CMYK8888;
}

bool sourceIsLossy(const CombinePage& page)
{
    const QString suffix = QFileInfo(page.sourcePath).suffix().toLower();
    return suffix == "jpg" || suffix == "jpeg" || suffix == "jpe" || suffix == "heic" || suffix == "heif" || suffix == "avif"
           || isJpegFile(page.sourcePath);
}

struct MemReader {
    const QByteArray* data;
};

int getBlock(void* param, unsigned long position, unsigned char* buf, unsigned long size)
{
    const QByteArray* d = static_cast<MemReader*>(param)->data;
    if (qint64(position) + qint64(size) > d->size())
        return 0;
    std::memcpy(buf, d->constData() + position, size);
    return 1;
}

FPDF_PAGEOBJECT jpegObject(FPDF_DOCUMENT doc, const QByteArray& jpeg)
{
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc);
    if (!obj)
        return nullptr;
    MemReader reader{&jpeg};
    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(jpeg.size());
    access.m_GetBlock = &getBlock;
    access.m_Param = &reader;
    // "Inline" copies the data into the document: the JPEG stream is embedded as-is (DCTDecode).
    if (!FPDFImageObj_LoadJpegFileInline(nullptr, 0, obj, &access)) {
        FPDFPageObj_Destroy(obj);
        return nullptr;
    }
    return obj;
}

FPDF_PAGEOBJECT bitmapObject(FPDF_DOCUMENT doc, const QImage& image)
{
    QImage img = image.convertToFormat(QImage::Format_RGB32); // BGRx in memory, opaque
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRx, img.bits(), int(img.bytesPerLine()));
    if (!obj || !bmp || !FPDFImageObj_SetBitmap(nullptr, 0, obj, bmp)) {
        if (bmp)
            FPDFBitmap_Destroy(bmp);
        if (obj)
            FPDFPageObj_Destroy(obj);
        return nullptr;
    }
    FPDFBitmap_Destroy(bmp);
    return obj; // PDFium Flate-compresses unfiltered streams when saving (lossless)
}

QImage flattened(const QImage& img)
{
    if (!img.hasAlphaChannel())
        return img;
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    p.drawImage(0, 0, img);
    return out;
}

// Page units -> PDF points for the page image placed at 'r'.
struct Mapper {
    QRectF r;
    QPointF map(const QPointF& u) const { return {r.left() + u.x() * r.width(), r.top() + r.height() - u.y() * r.width()}; }
    double len(double units) const { return units * r.width(); }
};

void setColors(FPDF_PAGEOBJECT obj, const QColor& stroke, const QColor& fill, double opacity)
{
    FPDFPageObj_SetStrokeColor(obj, uint(stroke.red()), uint(stroke.green()), uint(stroke.blue()),
                               uint(std::lround(stroke.alpha() * opacity)));
    FPDFPageObj_SetFillColor(obj, uint(fill.red()), uint(fill.green()), uint(fill.blue()),
                             uint(std::lround(fill.alpha() * opacity)));
}

FPDF_PAGEOBJECT pathObject(const QPainterPath& path, const Mapper& m)
{
    FPDF_PAGEOBJECT obj = nullptr;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        const QPointF p = m.map(QPointF(e.x, e.y));
        if (e.isMoveTo()) {
            if (!obj)
                obj = FPDFPageObj_CreateNewPath(float(p.x()), float(p.y()));
            else
                FPDFPath_MoveTo(obj, float(p.x()), float(p.y()));
        } else if (e.isLineTo()) {
            FPDFPath_LineTo(obj, float(p.x()), float(p.y()));
        } else if (e.isCurveTo() && i + 2 < path.elementCount()) {
            const QPointF c2 = m.map(QPointF(path.elementAt(i + 1).x, path.elementAt(i + 1).y));
            const QPointF e3 = m.map(QPointF(path.elementAt(i + 2).x, path.elementAt(i + 2).y));
            FPDFPath_BezierTo(obj, float(p.x()), float(p.y()), float(c2.x()), float(c2.y()), float(e3.x()), float(e3.y()));
            i += 2;
        }
    }
    return obj;
}

bool addStroke(FPDF_PAGE page, const QPainterPath& path, const Mapper& m, const Annotation& a, double widthUnits,
               int cap, int join)
{
    FPDF_PAGEOBJECT obj = pathObject(path, m);
    if (!obj)
        return false;
    setColors(obj, a.stroke, a.stroke, a.opacity);
    FPDFPageObj_SetStrokeWidth(obj, float(std::max(0.05, m.len(widthUnits))));
    FPDFPageObj_SetLineCap(obj, cap);
    FPDFPageObj_SetLineJoin(obj, join);
    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_NONE, 1);
    FPDFPage_InsertObject(page, obj);
    return true;
}

bool addFill(FPDF_PAGE page, const QPainterPath& path, const Mapper& m, const QColor& color, double opacity)
{
    FPDF_PAGEOBJECT obj = pathObject(path, m);
    if (!obj)
        return false;
    setColors(obj, color, color, opacity);
    FPDFPath_SetDrawMode(obj, FPDF_FILLMODE_WINDING, 0);
    FPDFPage_InsertObject(page, obj);
    return true;
}

} // namespace

PagePlan CombinePdfExporter::plan(const CombinePage& page, const ExportSettings& s)
{
    PagePlan pl;
    pl.passthroughJpeg = canPassThrough(page, s);
    // Output image size (pixels) of the final geometry.
    QSize px = Geometry::rotatedSize(page.sourceSize, page.rotation);
    if (page.quad)
        px = Geometry::marginSize(Geometry::rectifiedSize(*page.quad, page.perspective, QSizeF(px)), page.perspective);
    if (page.crop && px.isValid())
        px = QSize(std::max(1, int(std::lround(page.crop->width() * px.width()))),
                   std::max(1, int(std::lround(page.crop->height() * px.height()))));
    if (s.quality == QualityMode::SmallFile && std::max(px.width(), px.height()) > 1800)
        px.scale(1800, 1800, Qt::KeepAspectRatio);
    pl.imagePixels = px;
    const double aspect = px.width() > 0 ? double(px.height()) / px.width() : 1.414;
    const bool imageLandscape = px.width() > px.height();
    const double margin = s.marginPoints();

    if (s.pageSize == PageSizeMode::Auto) {
        // Original image size at 150 dpi, clamped to sensible paper sizes.
        double w = px.width() / 150.0 * 72.0;
        double h = w * aspect;
        const double maxSide = 1440, minSide = 144; // 20 in / 2 in
        const double k = std::min(1.0, maxSide / std::max(w, h));
        w *= k;
        h *= k;
        const double k2 = std::max(1.0, minSide / std::min(w, h));
        w *= k2;
        h *= k2;
        pl.pageSize = QSizeF(w + 2 * margin, h + 2 * margin);
        pl.imageRect = QRectF(margin, margin, w, h);
        pl.landscape = w > h;
    } else {
        QSizeF base = basePageSize(s);
        bool landscape = false;
        switch (s.orientation) {
        case OrientationMode::Auto: landscape = imageLandscape; break;
        case OrientationMode::Portrait: landscape = false; break;
        case OrientationMode::Landscape: landscape = true; break;
        }
        if (landscape != (base.width() > base.height()))
            base.transpose();
        pl.pageSize = base;
        pl.landscape = landscape;
        const QRectF content(margin, margin, std::max(1.0, base.width() - 2 * margin), std::max(1.0, base.height() - 2 * margin));
        // Fit the whole image inside the content area (never crop, never distort).
        double w = content.width(), h = w * aspect;
        if (h > content.height()) {
            h = content.height();
            w = h / aspect;
        }
        pl.imageRect = QRectF(content.left() + (content.width() - w) / 2, content.top() + (content.height() - h) / 2, w, h);
    }
    // Size estimate: passthrough = file size; JPEG ~ bits per pixel by quality; lossless ~ 1.6 B/px for photos.
    if (pl.passthroughJpeg) {
        pl.estimatedBytes = QFileInfo(page.sourcePath).size();
    } else {
        const double pixels = double(px.width()) * px.height();
        const bool lossless = s.quality == QualityMode::High || (s.quality == QualityMode::Balanced && !sourceIsLossy(page));
        const double bpp = lossless ? (sourceIsLossy(page) ? 1.6 : 0.35) : (s.quality == QualityMode::Balanced ? 0.28 : 0.12);
        pl.estimatedBytes = qint64(pixels * bpp);
    }
    pl.estimatedBytes += 2000 + qint64(page.annotations.size()) * 400;
    return pl;
}

qint64 CombinePdfExporter::estimateTotalBytes(const QList<CombinePage>& pages, const ExportSettings& s)
{
    qint64 total = 5000;
    for (const CombinePage& p : pages)
        total += plan(p, s).estimatedBytes;
    return total;
}

bool CombinePdfExporter::build(const QList<CombinePage>& pages, const ExportSettings& s, QByteArray* out,
                               const Progress& progress, const std::atomic_bool* cancel, QString* error)
{
    if (pages.isEmpty()) {
        if (error)
            *error = QObject::tr("There are no pages to export.");
        return false;
    }
    ensurePdfiumInitialized();
    auto doc = PdfDocument::createEmpty();
    // Characters needed by text annotations (exact ToUnicode map for the embedded fonts).
    QSet<char32_t> charset;
    for (char32_t c = 0x20; c <= 0x7E; ++c)
        charset.insert(c);
    for (const CombinePage& p : pages)
        for (const Annotation& a : p.annotations)
            for (char32_t c : sanitizeText(a.text).toUcs4())
                if (c != U'\n')
                    charset.insert(c);

    for (int i = 0; i < pages.size(); ++i) {
        if (cancel && cancel->load()) {
            if (error)
                *error = QObject::tr("Cancelled.");
            return false;
        }
        const CombinePage& page = pages[i];
        if (progress)
            progress(i, int(pages.size()), QObject::tr("Page %1 of %2").arg(i + 1).arg(pages.size()));
        const PagePlan pl = plan(page, s);

        // Prepare the image outside the PDFium lock (decoding and warping are the slow part).
        QByteArray jpeg;
        QImage bitmap;
        if (pl.passthroughJpeg) {
            QFile f(page.sourcePath);
            if (!f.open(QIODevice::ReadOnly)) {
                if (error)
                    *error = QObject::tr("Page %1: the source image %2 cannot be read.").arg(i + 1).arg(page.sourcePath);
                return false;
            }
            jpeg = f.readAll();
        } else {
            QString err;
            const int maxEdge = s.quality == QualityMode::SmallFile ? 1800 : 0;
            QImage img = ImageProcessor::render(page, ImageProcessor::Stage::Final, maxEdge, &err);
            if (img.isNull()) {
                if (error)
                    *error = QObject::tr("Page %1 (%2): %3").arg(i + 1).arg(page.displayName, err);
                return false;
            }
            if (!err.isEmpty()) { // e.g. invalid perspective - do not silently export something else
                if (error)
                    *error = QObject::tr("Page %1 (%2): %3").arg(i + 1).arg(page.displayName, err);
                return false;
            }
            img = flattened(img);
            const bool lossless = s.quality == QualityMode::High || (s.quality == QualityMode::Balanced && !sourceIsLossy(page));
            if (lossless) {
                bitmap = img;
            } else {
                QBuffer buf(&jpeg);
                buf.open(QIODevice::WriteOnly);
                QImageWriter w(&buf, "jpeg");
                w.setQuality(s.quality == QualityMode::Balanced ? 90 : 72);
                w.setOptimizedWrite(true);
                if (!w.write(img.convertToFormat(QImage::Format_RGB888))) {
                    if (error)
                        *error = QObject::tr("Page %1: JPEG encoding failed.").arg(i + 1);
                    return false;
                }
            }
        }

        std::lock_guard lock(pdfiumMutex());
        FPDF_PAGE pdfPage = FPDFPage_New(doc->handle(), i, pl.pageSize.width(), pl.pageSize.height());
        if (!pdfPage) {
            if (error)
                *error = QObject::tr("PDFium could not create page %1.").arg(i + 1);
            return false;
        }
        struct Closer {
            FPDF_PAGE p;
            ~Closer() { FPDF_ClosePage(p); }
        } closer{pdfPage};
        FPDF_PAGEOBJECT imageObj = jpeg.isEmpty() ? bitmapObject(doc->handle(), bitmap) : jpegObject(doc->handle(), jpeg);
        if (!imageObj) {
            if (error)
                *error = QObject::tr("Page %1: the image could not be embedded.").arg(i + 1);
            return false;
        }
        const QRectF& r = pl.imageRect;
        FS_MATRIX m{float(r.width()), 0, 0, float(r.height()), float(r.left()), float(pl.pageSize.height() - r.bottom())};
        FPDFPageObj_SetMatrix(imageObj, &m);
        FPDFPage_InsertObject(pdfPage, imageObj);

        // Annotations as vector graphics; PDF y axis points up.
        const Mapper map{QRectF(r.left(), pl.pageSize.height() - r.bottom(), r.width(), r.height())};
        FontProvider fonts(doc->handle(), charset);
        QList<PlacedLine> placed;
        for (const Annotation& a : page.annotations) {
            switch (a.type) {
            case Annotation::Type::Pen:
            case Annotation::Type::Highlighter:
            case Annotation::Type::Line:
            case Annotation::Type::Polyline:
                if (!a.pressure.isEmpty() && a.pressure.size() == a.points.size() && a.type == Annotation::Type::Pen) {
                    for (int k = 1; k < a.points.size(); ++k) {
                        QPainterPath seg;
                        seg.moveTo(a.points[k - 1]);
                        seg.lineTo(a.points[k]);
                        addStroke(pdfPage, seg, map, a, a.width * (0.3 + 0.7 * (a.pressure[k - 1] + a.pressure[k]) / 2.0),
                                  FPDF_LINECAP_ROUND, FPDF_LINEJOIN_ROUND);
                    }
                } else {
                    addStroke(pdfPage, AnnotationRender::centerLine(a), map, a, a.width,
                              a.type == Annotation::Type::Highlighter ? FPDF_LINECAP_PROJECTING_SQUARE : FPDF_LINECAP_ROUND,
                              FPDF_LINEJOIN_ROUND);
                }
                break;
            case Annotation::Type::Arrow:
                addStroke(pdfPage, AnnotationRender::centerLine(a), map, a, a.width, FPDF_LINECAP_ROUND, FPDF_LINEJOIN_ROUND);
                addFill(pdfPage, AnnotationRender::arrowHead(a), map, a.stroke, a.opacity);
                break;
            case Annotation::Type::Rect:
            case Annotation::Type::Ellipse: {
                const QPainterPath outline = AnnotationRender::outline(a);
                if (a.hasFill)
                    addFill(pdfPage, outline, map, a.fill, a.opacity);
                if (a.width > 0)
                    addStroke(pdfPage, outline, map, a, a.width, FPDF_LINECAP_PROJECTING_SQUARE, FPDF_LINEJOIN_MITER);
                break;
            }
            case Annotation::Type::Text: {
                const QString file = AnnotationRender::fontFile(a);
                QString err;
                WriterFont* font = file.isEmpty() ? nullptr : fonts.system(file, &err);
                if (!font) {
                    if (error)
                        *error = QObject::tr("Page %1: no font is available for text annotations. %2").arg(i + 1).arg(err);
                    return false;
                }
                Annotation clean = a;
                clean.text = sanitizeText(a.text);
                const double size = map.len(a.fontSize);
                QColor color = a.stroke;
                color.setAlpha(int(std::lround(color.alpha() * a.opacity)));
                for (const TextLineLayout& l : AnnotationRender::layoutText(clean)) {
                    if (l.text.trimmed().isEmpty())
                        continue;
                    if (!font->supports(l.text)) {
                        if (error)
                            *error = QObject::tr("Page %1: the font %2 cannot display \"%3\".").arg(i + 1).arg(font->name, l.text);
                        return false;
                    }
                    const QPointF o = map.map(QPointF(l.x, l.baseline));
                    placed << placeLine(doc->handle(), pdfPage, *font, size, l.text, QTransform(1, 0, 0, 1, o.x(), o.y()),
                                        color, 0, nullptr);
                    if (a.underline) {
                        QPainterPath u;
                        u.addRect(QRectF(l.x, l.baseline + a.fontSize * 0.12, l.width, a.fontSize * 0.06));
                        addFill(pdfPage, u, map, a.stroke, a.opacity);
                    }
                }
                break;
            }
            }
        }
        QStringList details;
        if (!verifyReadBack(pdfPage, placed, &details).isEmpty()) {
            if (error)
                *error = QObject::tr("Page %1: text could not be written exactly (%2).").arg(i + 1).arg(details.join("; "));
            return false;
        }
        if (!FPDFPage_GenerateContent(pdfPage)) {
            if (error)
                *error = QObject::tr("PDFium could not generate page %1.").arg(i + 1);
            return false;
        }
    }
    if (progress)
        progress(int(pages.size()), int(pages.size()), QObject::tr("Writing PDF"));
    return doc->save(out, PdfDocument::SaveMode::Full, error);
}

bool CombinePdfExporter::verify(const QByteArray& pdf, const QList<CombinePage>& pages, const ExportSettings& s,
                                QString* error)
{
    PdfDocument::OpenError oe;
    QString msg;
    auto doc = PdfDocument::openData(pdf, {}, &oe, &msg);
    if (!doc) {
        if (error)
            *error = QObject::tr("The exported PDF cannot be opened: %1").arg(msg);
        return false;
    }
    if (doc->pageCount() != pages.size()) {
        if (error)
            *error = QObject::tr("The PDF has %1 pages instead of %2.").arg(doc->pageCount()).arg(pages.size());
        return false;
    }
    for (int i = 0; i < pages.size(); ++i) {
        const QSizeF expected = plan(pages[i], s).pageSize;
        const QSizeF got = doc->pageSize(i);
        if (std::abs(got.width() - expected.width()) > 1.0 || std::abs(got.height() - expected.height()) > 1.0) {
            if (error)
                *error = QObject::tr("Page %1 has an unexpected size.").arg(i + 1);
            return false;
        }
        // Text annotations must be present as text.
        const QString text = compact(doc->pageText(i));
        for (const Annotation& a : pages[i].annotations)
            if (a.type == Annotation::Type::Text && !compact(sanitizeText(a.text)).isEmpty()
                && !text.contains(compact(sanitizeText(a.text)))) {
                if (error)
                    *error = QObject::tr("Text on page %1 is missing in the PDF.").arg(i + 1);
                return false;
            }
    }
    return true;
}

ExportResult CombinePdfExporter::exportTo(const QString& path, const QList<CombinePage>& pages, const ExportSettings& s,
                                          bool overwrite, const Progress& progress, const std::atomic_bool* cancel)
{
    ExportResult r;
    r.path = path;
    QByteArray pdf;
    QString err;
    if (!build(pages, s, &pdf, progress, cancel, &err)) {
        r.cancelled = cancel && cancel->load();
        r.error = err;
        return r;
    }
    if (cancel && cancel->load()) {
        r.cancelled = true;
        r.error = QObject::tr("Cancelled.");
        return r;
    }
    if (!verify(pdf, pages, s, &err)) {
        r.error = err;
        return r;
    }
    const auto res = SafeFileWriter::writeNew(
        path,
        [&pdf](const QString& tmp, QString* e) {
            QFile f(tmp);
            if (!f.open(QIODevice::WriteOnly) || f.write(pdf) != pdf.size() || !f.flush()) {
                *e = f.errorString();
                return false;
            }
            return true;
        },
        [&](const QString& tmp, QString* e) {
            QFile f(tmp);
            return f.open(QIODevice::ReadOnly) && verify(f.readAll(), pages, s, e);
        },
        overwrite);
    r.ok = res.ok();
    r.error = res.error;
    r.bytes = pdf.size();
    r.pages = int(pages.size());
    return r;
}

} // namespace folvio::combine
