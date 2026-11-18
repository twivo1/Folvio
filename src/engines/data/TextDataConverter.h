#pragma once

#include "core/conversions/IConverter.h"

#include <QJsonArray>
#include <QStringList>

namespace folvio {

// Native conversions for text and tabular data:
// CSV <-> JSON, Markdown <-> HTML, Markdown -> PDF/TXT, HTML -> Markdown.
class TextDataConverter : public IConverter {
public:
    QString id() const override { return QStringLiteral("text-data"); }
    QString displayName() const override { return QStringLiteral("Folvio text & data"); }
    QList<ConversionCapability> capabilities() const override;
    QList<OptionSpec> options(const QString& from, const QString& to) const override;
    ConversionResult convert(const ConversionRequest& request, ConversionContext& ctx) override;

    // RFC 4180 parser. delimiter 0 = auto-detect.
    static QList<QStringList> parseCsv(const QString& text, QChar delimiter = QChar(), QString* error = nullptr);
    static QChar detectDelimiter(const QString& text);
    static QString writeCsv(const QList<QStringList>& rows, QChar delimiter = QLatin1Char(','));

    static bool csvToJson(const QString& csv, bool header, bool inferTypes, QChar delimiter, QByteArray* json,
                          QString* error);
    static bool jsonToCsv(const QByteArray& json, QChar delimiter, QString* csv, QString* error);
};

} // namespace folvio
