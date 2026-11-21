#pragma once

#include "core/conversions/IConverter.h"

namespace folvio {

// Office conversions through LibreOffice in headless mode.
// Each conversion runs in an isolated user profile with macros disabled.
class LibreOfficeConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("libreoffice"); }
    QString displayName() const override { return QStringLiteral("LibreOffice"); }
    QList<ConversionCapability> capabilities() const override;
    QStringList requiredTools() const override { return {QStringLiteral("soffice")}; }
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;

    // Shared helper also used by the editor import/export pipeline.
    static ConversionResult runConversion(const QString& inputPath, const QString& inputFormat,
                                          const QString& outputFormat, const QString& outputPath,
                                          ConversionContext& ctx, const QVariantMap& options = {});
};

} // namespace folvio
