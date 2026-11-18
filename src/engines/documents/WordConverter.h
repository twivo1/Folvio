#pragma once

#include "core/conversions/IConverter.h"

namespace folvio {

// Optional engine: uses the user's installed Microsoft Word through COM
// automation (helper script engines/office/word_convert.ps1). Word's PDF
// reflow gives the best available PDF -> DOCX reconstruction offline.
class WordConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("msword"); }
    QString displayName() const override { return QStringLiteral("Microsoft Word"); }
    QList<ConversionCapability> capabilities() const override;
    QStringList requiredTools() const override { return {QStringLiteral("word")}; }
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;

    static ConversionResult runConversion(const QString& inputPath, const QString& inputFormat,
                                          const QString& outputFormat, const QString& outputPath,
                                          ConversionContext& ctx);
};

} // namespace folvio
