#pragma once

#include "core/conversions/IConverter.h"

namespace folvio {

// PDF pages -> PNG/JPG using PDFium.
class PdfRasterConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("pdf-raster"); }
    QString displayName() const override { return QStringLiteral("PDFium renderer"); }
    QList<ConversionCapability> capabilities() const override;
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;
};

// Images (one per page; multi-frame TIFF supported) -> PDF using QPdfWriter.
class ImagesToPdfConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("images-pdf"); }
    QString displayName() const override { return QStringLiteral("Folvio PDF writer"); }
    QList<ConversionCapability> capabilities() const override;
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;
};

} // namespace folvio
