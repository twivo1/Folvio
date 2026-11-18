#pragma once

#include "core/conversions/IConverter.h"

namespace folvio {

// Raster/vector image conversion with Qt image plugins.
// Honors EXIF orientation, keeps ICC colour profiles where the target format
// supports them and flattens transparency for formats without alpha.
class QtImageConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("qt-image"); }
    QString displayName() const override { return QStringLiteral("Qt imaging"); }
    QList<ConversionCapability> capabilities() const override;
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;
};

// HEIC/HEIF and AVIF through FFmpeg (libheif-free path; FFmpeg >= 7.1 assembles tiled HEIC).
class FfmpegImageConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("ffmpeg-image"); }
    QString displayName() const override { return QStringLiteral("FFmpeg imaging"); }
    QList<ConversionCapability> capabilities() const override;
    QStringList requiredTools() const override { return {QStringLiteral("ffmpeg"), QStringLiteral("ffprobe")}; }
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;
};

} // namespace folvio
