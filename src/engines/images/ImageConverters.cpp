#include "engines/images/ImageConverters.h"

#include "core/deps/DependencyManager.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSvgRenderer>

namespace folvio {

namespace {

const QStringList kQtIn = {"png", "jpg", "webp", "bmp", "tiff", "gif", "svg"};
const QStringList kQtOut = {"png", "jpg", "webp", "bmp", "tiff"};

QByteArray qtFormat(const QString& id)
{
    if (id == "jpg") return "jpeg";
    if (id == "tiff") return "tiff";
    return id.toLatin1();
}

bool targetSupportsAlpha(const QString& id) { return id == "png" || id == "webp" || id == "tiff"; }

OptionSpec qualityOption(int def)
{
    OptionSpec q;
    q.key = "quality";
    q.label = QObject::tr("Quality");
    q.type = OptionSpec::Type::Integer;
    q.minimum = 10;
    q.maximum = 100;
    q.defaultValue = def;
    return q;
}

OptionSpec maxSizeOption()
{
    OptionSpec s;
    s.key = "maxSize";
    s.label = QObject::tr("Resize");
    s.choiceValues = {"0", "3840", "1920", "1280", "800"};
    s.choiceLabels = {QObject::tr("Original size"), QObject::tr("Max 3840 px"), QObject::tr("Max 1920 px"),
                      QObject::tr("Max 1280 px"), QObject::tr("Max 800 px")};
    s.defaultValue = "0";
    return s;
}

} // namespace

QList<ConversionCapability> QtImageConverter::capabilities() const
{
    QList<ConversionCapability> caps;
    for (const QString& in : kQtIn)
        for (const QString& out : kQtOut) {
            if (in == out)
                continue;
            ConversionCapability c{in, out, 90, out == "jpg" || out == "webp", {}};
            if (in == "gif")
                c.note = QObject::tr("Only the first frame of an animated GIF is converted.");
            if (in == "svg")
                c.note = QObject::tr("The vector image is rasterized.");
            if (!targetSupportsAlpha(out))
                c.note += (c.note.isEmpty() ? "" : " ") + QObject::tr("Transparent areas are filled with the background colour.");
            caps << c;
        }
    return caps;
}

QList<OptionSpec> QtImageConverter::options(const QString& from, const QString& to) const
{
    QList<OptionSpec> o;
    if (to == "jpg" || to == "webp")
        o << qualityOption(to == "jpg" ? 90 : 85);
    if (from == "svg") {
        OptionSpec w;
        w.key = "svgWidth";
        w.label = QObject::tr("Output width");
        w.choiceValues = {"0", "512", "1024", "2048", "4096"};
        w.choiceLabels = {QObject::tr("Native size"), "512 px", "1024 px", "2048 px", "4096 px"};
        w.defaultValue = "1024";
        o << w;
    } else {
        o << maxSizeOption();
    }
    if (!targetSupportsAlpha(to)) {
        OptionSpec bg;
        bg.key = "background";
        bg.label = QObject::tr("Background for transparency");
        bg.choiceValues = {"#ffffff", "#000000"};
        bg.choiceLabels = {QObject::tr("White"), QObject::tr("Black")};
        bg.defaultValue = "#ffffff";
        o << bg;
    }
    OptionSpec meta;
    meta.key = "keepProfile";
    meta.label = QObject::tr("Keep colour profile");
    meta.type = OptionSpec::Type::Boolean;
    meta.defaultValue = true;
    meta.tooltip = QObject::tr("When off, the image is converted to sRGB.");
    o << meta;
    return o;
}

ConversionResult QtImageConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    QImage img;
    if (request.inputFormat == "svg") {
        QSvgRenderer svg(request.inputPath);
        if (!svg.isValid())
            return ConversionResult::failure(QObject::tr("The SVG file is invalid."));
        QSize size = svg.defaultSize();
        if (size.isEmpty())
            size = QSize(1024, 1024);
        const int w = request.options.value("svgWidth", "1024").toInt();
        if (w > 0)
            size = QSize(w, std::max(1, int(double(size.height()) * w / std::max(1, size.width()))));
        if (qint64(size.width()) * size.height() > 400LL * 1000 * 1000)
            return ConversionResult::failure(QObject::tr("Requested image is too large."));
        img = QImage(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        svg.render(&p);
    } else {
        QImageReader reader(request.inputPath);
        reader.setAutoTransform(true);
        reader.setDecideFormatFromContent(true);
        reader.setAllocationLimit(1024); // MB: protects against decompression bombs
        img = reader.read();
        if (img.isNull())
            return ConversionResult::failure(QObject::tr("The image could not be decoded: %1").arg(reader.errorString()));
    }
    if (ctx.isCancelled())
        return ConversionResult::failure(QObject::tr("Cancelled."));
    ctx.stepProgress(0.4, QObject::tr("Encoding"));

    const int maxSize = request.options.value("maxSize", "0").toInt();
    if (maxSize > 0 && (img.width() > maxSize || img.height() > maxSize))
        img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (!request.options.value("keepProfile", true).toBool() && img.colorSpace().isValid()
        && img.colorSpace() != QColorSpace::SRgb)
        img.convertToColorSpace(QColorSpace::SRgb);

    if (!targetSupportsAlpha(request.outputFormat) && img.hasAlphaChannel()) {
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.setColorSpace(img.colorSpace());
        flat.setDotsPerMeterX(img.dotsPerMeterX());
        flat.setDotsPerMeterY(img.dotsPerMeterY());
        flat.fill(QColor(request.options.value("background", "#ffffff").toString()));
        QPainter p(&flat);
        p.drawImage(0, 0, img);
        p.end();
        img = flat;
    }

    QImageWriter writer(request.outputPath, qtFormat(request.outputFormat));
    if (request.outputFormat == "jpg" || request.outputFormat == "webp")
        writer.setQuality(request.options.value("quality", 90).toInt());
    if (request.outputFormat == "png")
        writer.setCompression(9);
    if (request.outputFormat == "jpg")
        writer.setOptimizedWrite(true);
    if (!writer.write(img))
        return ConversionResult::failure(QObject::tr("The image could not be written: %1").arg(writer.errorString()));
    ConversionResult r;
    r.ok = true;
    r.outputs << request.outputPath;
    return r;
}

// ---------------------------------------------------------------------------

QList<ConversionCapability> FfmpegImageConverter::capabilities() const
{
    return {{"heic", "jpg", 85, true, {}},
            {"heic", "png", 90, false, {}},
            {"avif", "jpg", 85, true, {}},
            {"avif", "png", 90, false, {}},
            {"png", "avif", 80, true, QObject::tr("AVIF encoding is slow for large images.")},
            {"jpg", "avif", 80, true, QObject::tr("AVIF encoding is slow for large images.")}};
}

QList<OptionSpec> FfmpegImageConverter::options(const QString& from, const QString& to) const
{
    Q_UNUSED(from)
    if (to == "jpg" || to == "avif")
        return {qualityOption(to == "jpg" ? 90 : 70)};
    return {};
}

ConversionResult FfmpegImageConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    const QString ffmpeg = DependencyManager::instance().path("ffmpeg");
    const int quality = request.options.value("quality", 85).toInt();
    ProcessSpec spec;
    spec.program = ffmpeg;
    spec.arguments = {"-hide_banner", "-nostdin", "-loglevel", "error", "-protocol_whitelist", "file",
                      "-i", "file:" + request.inputPath, "-frames:v", "1"};
    if (request.outputFormat == "jpg") {
        // ffmpeg mjpeg q: 2 (best) .. 31 (worst)
        const int q = std::clamp(31 - (quality * 29) / 100, 2, 31);
        spec.arguments << "-c:v" << "mjpeg" << "-q:v" << QString::number(q) << "-pix_fmt" << "yuvj444p";
    } else if (request.outputFormat == "png") {
        spec.arguments << "-c:v" << "png";
    } else if (request.outputFormat == "avif") {
        const int crf = std::clamp(63 - (quality * 63) / 100, 0, 63);
        spec.arguments << "-c:v" << "libaom-av1" << "-still-picture" << "1" << "-crf" << QString::number(crf)
                       << "-cpu-used" << "6";
    }
    spec.arguments << "-y" << "file:" + request.outputPath;
    spec.timeoutMs = 5 * 60 * 1000;
    ctx.stepProgress(0.2, QObject::tr("FFmpeg is converting…"));
    const ProcessResult pr = ctx.run(spec);
    if (pr.cancelled) {
        ConversionResult r;
        r.cancelled = true;
        return r;
    }
    if (!pr.succeeded() || !QFileInfo::exists(request.outputPath))
        return ConversionResult::failure(pr.describe());
    ConversionResult r;
    r.ok = true;
    r.outputs << request.outputPath;
    return r;
}

} // namespace folvio
