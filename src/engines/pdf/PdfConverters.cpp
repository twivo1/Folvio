#include "engines/pdf/PdfConverters.h"

#include "engines/pdf/PdfDocument.h"

#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

namespace folvio {

QList<ConversionCapability> PdfRasterConverter::capabilities() const
{
    return {{"pdf", "png", 90, false, QObject::tr("Each page becomes one image.")},
            {"pdf", "jpg", 85, true, QObject::tr("Each page becomes one image.")}};
}

QList<OptionSpec> PdfRasterConverter::options(const QString& from, const QString& to) const
{
    Q_UNUSED(from)
    QList<OptionSpec> o;
    OptionSpec dpi;
    dpi.key = "dpi";
    dpi.label = QObject::tr("Resolution");
    dpi.choiceValues = {"96", "150", "200", "300", "600"};
    dpi.choiceLabels = {"96 DPI (screen)", "150 DPI", "200 DPI", "300 DPI (print)", "600 DPI"};
    dpi.defaultValue = "150";
    o << dpi;
    OptionSpec pages;
    pages.key = "pages";
    pages.label = QObject::tr("Pages");
    pages.choiceValues = {"all", "first"};
    pages.choiceLabels = {QObject::tr("All pages"), QObject::tr("First page only")};
    pages.defaultValue = "all";
    o << pages;
    if (to == "jpg") {
        OptionSpec q;
        q.key = "quality";
        q.label = QObject::tr("JPEG quality");
        q.type = OptionSpec::Type::Integer;
        q.minimum = 30;
        q.maximum = 100;
        q.defaultValue = 90;
        o << q;
    }
    return o;
}

ConversionResult PdfRasterConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    QString msg;
    PdfDocument::OpenError err;
    auto doc = PdfDocument::openFile(request.inputPath, request.options.value("password").toByteArray(), &err, &msg);
    if (!doc)
        return ConversionResult::failure(msg);
    const double dpi = request.options.value("dpi", "150").toDouble();
    const int quality = request.options.value("quality", 90).toInt();
    const int total = request.options.value("pages", "all").toString() == "first" ? 1 : doc->pageCount();
    constexpr int kMaxPages = 2000;
    if (total > kMaxPages)
        return ConversionResult::failure(QObject::tr("The document has more than %1 pages.").arg(kMaxPages));

    ConversionResult r;
    const QFileInfo out(request.outputPath);
    for (int i = 0; i < total; ++i) {
        if (ctx.isCancelled()) {
            r.cancelled = true;
            return r;
        }
        ctx.stepProgress(double(i) / total, QObject::tr("Rendering page %1 of %2").arg(i + 1).arg(total));
        const QImage img = doc->render(i, dpi / 72.0);
        if (img.isNull())
            return ConversionResult::failure(QObject::tr("Page %1 could not be rendered.").arg(i + 1));
        const QString path = total == 1 ? request.outputPath
                                        : out.absolutePath() + "/" + out.completeBaseName()
                                              + QStringLiteral("-p%1.").arg(i + 1, 3, 10, QLatin1Char('0')) + out.suffix();
        QImageWriter w(path, request.outputFormat == "jpg" ? "jpeg" : "png");
        if (request.outputFormat == "jpg")
            w.setQuality(quality);
        QImage tagged = img;
        tagged.setDotsPerMeterX(int(dpi / 0.0254));
        tagged.setDotsPerMeterY(int(dpi / 0.0254));
        if (!w.write(tagged))
            return ConversionResult::failure(w.errorString());
        r.outputs << path;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------

QList<ConversionCapability> ImagesToPdfConverter::capabilities() const
{
    QList<ConversionCapability> caps;
    for (const QString& in : {"png", "jpg", "bmp", "webp", "tiff", "gif"})
        caps << ConversionCapability{in, "pdf", 95, false, {}};
    return caps;
}

QList<OptionSpec> ImagesToPdfConverter::options(const QString& from, const QString& to) const
{
    Q_UNUSED(from)
    Q_UNUSED(to)
    OptionSpec page;
    page.key = "pageSize";
    page.label = QObject::tr("Page size");
    page.choiceValues = {"image", "a4", "letter"};
    page.choiceLabels = {QObject::tr("Fit to image"), "A4", "Letter"};
    page.defaultValue = "image";
    return {page};
}

ConversionResult ImagesToPdfConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    QImageReader reader(request.inputPath);
    reader.setAutoTransform(true); // honour EXIF orientation
    reader.setDecideFormatFromContent(true);
    QList<QImage> frames;
    do {
        QImage img = reader.read();
        if (img.isNull())
            break;
        frames << img;
        if (ctx.isCancelled())
            return ConversionResult::failure(QObject::tr("Cancelled."));
    } while (request.inputFormat == "tiff" && reader.jumpToNextImage() && frames.size() < 1000);
    if (frames.isEmpty())
        return ConversionResult::failure(QObject::tr("The image could not be decoded: %1").arg(reader.errorString()));

    const QString pageMode = request.options.value("pageSize", "image").toString();
    QPdfWriter writer(request.outputPath);
    writer.setCreator(QStringLiteral("Folvio"));
    writer.setTitle(QFileInfo(request.inputPath).completeBaseName());
    writer.setResolution(300);
    QPainter painter;
    for (int i = 0; i < frames.size(); ++i) {
        const QImage& img = frames[i];
        double dpiX = img.dotsPerMeterX() > 0 ? img.dotsPerMeterX() * 0.0254 : 96.0;
        double dpiY = img.dotsPerMeterY() > 0 ? img.dotsPerMeterY() * 0.0254 : 96.0;
        if (dpiX < 30 || dpiX > 2400) dpiX = 96;
        if (dpiY < 30 || dpiY > 2400) dpiY = 96;
        QPageSize ps;
        if (pageMode == "a4")
            ps = QPageSize(QPageSize::A4);
        else if (pageMode == "letter")
            ps = QPageSize(QPageSize::Letter);
        else
            ps = QPageSize(QSizeF(img.width() / dpiX * 72.0, img.height() / dpiY * 72.0), QPageSize::Point,
                           QString(), QPageSize::ExactMatch);
        writer.setPageSize(ps);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        if (i == 0) {
            if (!painter.begin(&writer))
                return ConversionResult::failure(QObject::tr("Cannot create the PDF file."));
        } else {
            writer.newPage();
        }
        const QRectF pageRect(QPointF(0, 0), QSizeF(writer.width(), writer.height()));
        QSizeF target = img.size();
        target.scale(pageRect.size(), Qt::KeepAspectRatio);
        const QRectF dst(pageRect.center() - QPointF(target.width() / 2, target.height() / 2), target);
        painter.fillRect(pageRect, Qt::white);
        painter.drawImage(dst, img);
        ctx.stepProgress(double(i + 1) / frames.size());
    }
    painter.end();
    ConversionResult r;
    r.ok = true;
    r.outputs << request.outputPath;
    return r;
}

} // namespace folvio
