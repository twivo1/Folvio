#pragma once

#include "core/conversions/IConverter.h"

namespace folvio {

// Audio/video conversion through FFmpeg.
// - Stream copy (remux) when the target container supports the source codecs
//   and no re-encoding option was requested.
// - Real progress from "-progress pipe:1".
class FfmpegConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("ffmpeg"); }
    QString displayName() const override { return QStringLiteral("FFmpeg"); }
    QList<ConversionCapability> capabilities() const override;
    QStringList requiredTools() const override { return {QStringLiteral("ffmpeg"), QStringLiteral("ffprobe")}; }
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;

    // Exposed for tests: can the container hold these codecs without re-encoding?
    static bool containerAccepts(const QString& container, const QString& videoCodec, const QString& audioCodec);
};

} // namespace folvio
