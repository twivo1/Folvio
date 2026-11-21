#include "engines/data/TextDataConverter.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPageSize>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <QStringDecoder>
#include <QTextDocument>

namespace folvio {

namespace {

bool readText(const QString& path, QString* out, QString* error, qint64 limit = 256LL * 1024 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return false;
    }
    if (f.size() > limit) {
        *error = QObject::tr("The file is too large for this conversion.");
        return false;
    }
    QByteArray data = f.readAll();
    // Honour a BOM if present, otherwise try UTF-8 then fall back to the local 8-bit code page.
    auto enc = QStringConverter::encodingForData(data);
    if (enc) {
        QStringDecoder d(*enc);
        *out = d.decode(data);
        if (out->startsWith(QChar(0xFEFF)))
            out->remove(0, 1);
        return true;
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    *out = utf8.decode(data);
    if (utf8.hasError())
        *out = QString::fromLocal8Bit(data);
    return true;
}

bool writeBytes(const QString& path, const QByteArray& data, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
        *error = f.errorString();
        return false;
    }
    return true;
}

QJsonValue inferValue(const QString& s)
{
    const QString t = s.trimmed();
    if (t.isEmpty())
        return QJsonValue(s);
    if (t == "true" || t == "false")
        return QJsonValue(t == "true");
    if (t == "null")
        return QJsonValue(QJsonValue::Null);
    // Keep values like "007" or "+48..." as strings: leading zeros carry meaning.
    static const QRegularExpression num(QStringLiteral("^-?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?$"));
    if (num.match(t).hasMatch()) {
        bool ok = false;
        const double d = t.toDouble(&ok);
        if (ok && std::isfinite(d)) {
            if (!t.contains('.') && !t.contains('e') && !t.contains('E') && std::abs(d) < 9007199254740992.0)
                return QJsonValue(qint64(t.toLongLong()));
            return QJsonValue(d);
        }
    }
    return QJsonValue(s);
}

QString jsonScalarToString(const QJsonValue& v)
{
    switch (v.type()) {
    case QJsonValue::Null: return {};
    case QJsonValue::Bool: return v.toBool() ? "true" : "false";
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == std::floor(d) && std::abs(d) < 1e15)
            return QString::number(qint64(d));
        return QString::number(d, 'g', 17);
    }
    case QJsonValue::String: return v.toString();
    default: return {};
    }
}

} // namespace

QList<ConversionCapability> TextDataConverter::capabilities() const
{
    return {
        {"csv", "json", 95, false, {}},
        {"json", "csv", 90, false, QObject::tr("Requires a JSON array of flat objects or arrays.")},
        {"md", "html", 95, false, {}},
        {"html", "md", 70, false, QObject::tr("Styling that Markdown cannot express is dropped.")},
        {"md", "pdf", 85, false, {}},
        {"md", "txt", 95, true, QObject::tr("Markdown formatting is removed.")},
        {"html", "txt", 75, true, QObject::tr("Formatting is removed.")},
    };
}

QList<OptionSpec> TextDataConverter::options(const QString& from, const QString& to) const
{
    QList<OptionSpec> o;
    if (from == "csv" || to == "csv") {
        OptionSpec d;
        d.key = "delimiter";
        d.label = QObject::tr("Delimiter");
        d.choiceValues = {"auto", ",", ";", "\t"};
        d.choiceLabels = {QObject::tr("Detect"), QObject::tr("Comma"), QObject::tr("Semicolon"), QObject::tr("Tab")};
        d.defaultValue = from == "csv" ? "auto" : ",";
        if (to == "csv")
            d.choiceValues[0] = ",", d.choiceLabels[0] = QObject::tr("Comma"), d.choiceValues.removeAt(1),
            d.choiceLabels.removeAt(1);
        o << d;
    }
    if (from == "csv") {
        OptionSpec h;
        h.key = "header";
        h.label = QObject::tr("First row is a header");
        h.type = OptionSpec::Type::Boolean;
        h.defaultValue = true;
        o << h;
        OptionSpec t;
        t.key = "inferTypes";
        t.label = QObject::tr("Detect numbers and booleans");
        t.type = OptionSpec::Type::Boolean;
        t.defaultValue = true;
        o << t;
    }
    return o;
}

QChar TextDataConverter::detectDelimiter(const QString& text)
{
    const QString sample = text.left(16384);
    const QList<QChar> candidates = {QLatin1Char(','), QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char('|')};
    QChar best = QLatin1Char(',');
    int bestScore = -1;
    for (QChar c : candidates) {
        // Score = consistent count per line across the first lines.
        const QList<QStringList> rows = parseCsv(sample, c);
        if (rows.size() < 1)
            continue;
        const int cols = int(rows.first().size());
        if (cols < 2)
            continue;
        int consistent = 0;
        for (const auto& r : rows)
            if (r.size() == cols)
                ++consistent;
        const int score = consistent * 100 + cols;
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

QList<QStringList> TextDataConverter::parseCsv(const QString& text, QChar delimiter, QString* error)
{
    const QChar d = delimiter.isNull() ? detectDelimiter(text) : delimiter;
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, fieldStarted = false;
    const int n = int(text.size());
    for (int i = 0; i < n; ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < n && text[i + 1] == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == QLatin1Char('"') && !fieldStarted) {
            quoted = true;
            fieldStarted = true;
        } else if (c == d) {
            row << field;
            field.clear();
            fieldStarted = false;
        } else if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            if (c == QLatin1Char('\r') && i + 1 < n && text[i + 1] == QLatin1Char('\n'))
                ++i;
            row << field;
            field.clear();
            fieldStarted = false;
            if (!(row.size() == 1 && row.first().isEmpty()))
                rows << row;
            row.clear();
        } else {
            field += c;
            fieldStarted = true;
        }
    }
    if (quoted && error)
        *error = QObject::tr("Unterminated quoted field.");
    if (fieldStarted || !field.isEmpty() || !row.isEmpty()) {
        row << field;
        rows << row;
    }
    return rows;
}

QString TextDataConverter::writeCsv(const QList<QStringList>& rows, QChar delimiter)
{
    QString out;
    for (const QStringList& r : rows) {
        QStringList cells;
        for (QString c : r) {
            if (c.contains(delimiter) || c.contains('"') || c.contains('\n') || c.contains('\r')
                || c.startsWith(' ') || c.endsWith(' ')) {
                c.replace("\"", "\"\"");
                c = "\"" + c + "\"";
            }
            cells << c;
        }
        out += cells.join(delimiter) + "\r\n";
    }
    return out;
}

bool TextDataConverter::csvToJson(const QString& csv, bool header, bool inferTypes, QChar delimiter, QByteArray* json,
                                  QString* error)
{
    QString perr;
    const QList<QStringList> rows = parseCsv(csv, delimiter, &perr);
    if (!perr.isEmpty()) {
        *error = perr;
        return false;
    }
    if (rows.isEmpty()) {
        *error = QObject::tr("The CSV file contains no rows.");
        return false;
    }
    const int cols = int(rows.first().size());
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != cols) {
            *error = QObject::tr("Row %1 has %2 columns, expected %3. The data is not a consistent table.")
                         .arg(i + 1).arg(rows[i].size()).arg(cols);
            return false;
        }
    }
    auto cell = [inferTypes](const QString& v) { return inferTypes ? inferValue(v) : QJsonValue(v); };
    auto encode = [](const QJsonValue& v) {
        // Serialize a single value by wrapping it in an array and stripping the brackets.
        const QByteArray a = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
        return a.mid(1, a.size() - 2);
    };
    if (!header) {
        QJsonArray arr;
        for (const QStringList& r : rows) {
            QJsonArray a;
            for (const QString& v : r)
                a.append(cell(v));
            arr.append(a);
        }
        *json = QJsonDocument(arr).toJson(QJsonDocument::Indented);
        return true;
    }
    // Unique, non-empty keys in column order (QJsonObject would sort them, so write objects manually).
    QStringList keys;
    QSet<QString> seen;
    for (int i = 0; i < cols; ++i) {
        QString k = rows.first()[i].trimmed();
        if (k.isEmpty())
            k = QStringLiteral("column%1").arg(i + 1);
        QString unique = k;
        for (int n = 2; seen.contains(unique); ++n)
            unique = QStringLiteral("%1_%2").arg(k).arg(n);
        seen.insert(unique);
        keys << unique;
    }
    QByteArray out = "[\n";
    for (int r = 1; r < rows.size(); ++r) {
        QByteArrayList parts;
        for (int c = 0; c < cols; ++c)
            parts << encode(QJsonValue(keys[c])) + ": " + encode(cell(rows[r][c]));
        out += "  {" + parts.join(", ") + "}" + (r + 1 < rows.size() ? ",\n" : "\n");
    }
    out += "]\n";
    *json = out;
    return true;
}

bool TextDataConverter::jsonToCsv(const QByteArray& json, QChar delimiter, QString* csv, QString* error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError) {
        *error = QObject::tr("Invalid JSON: %1").arg(pe.errorString());
        return false;
    }
    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject()) {
        // Accept {"key": [ ... ]} with exactly one array member.
        const QJsonObject o = doc.object();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (it->isArray()) {
                if (!arr.isEmpty()) {
                    *error = QObject::tr("JSON object contains several arrays; cannot pick a table.");
                    return false;
                }
                arr = it->toArray();
            }
    }
    if (arr.isEmpty()) {
        *error = QObject::tr("JSON must be an array of records.");
        return false;
    }
    QList<QStringList> rows;
    if (arr.first().isObject()) {
        // Column order: order of first appearance. Parse keys from raw text to keep source order.
        QStringList keys;
        for (const QJsonValue& v : arr) {
            if (!v.isObject()) {
                *error = QObject::tr("All records must be objects.");
                return false;
            }
            for (const QString& k : v.toObject().keys())
                if (!keys.contains(k))
                    keys << k;
        }
        // Restore the source key order where possible.
        std::stable_sort(keys.begin(), keys.end(), [&json](const QString& a, const QString& b) {
            return json.indexOf(QJsonDocument(QJsonArray{a}).toJson(QJsonDocument::Compact).mid(1).chopped(1))
                   < json.indexOf(QJsonDocument(QJsonArray{b}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
        });
        rows << keys;
        for (const QJsonValue& v : arr) {
            const QJsonObject o = v.toObject();
            QStringList r;
            for (const QString& k : keys) {
                const QJsonValue cell = o.value(k);
                if (cell.isObject() || cell.isArray()) {
                    *error = QObject::tr("Field \"%1\" contains nested data, which CSV cannot represent.").arg(k);
                    return false;
                }
                r << jsonScalarToString(cell);
            }
            rows << r;
        }
    } else if (arr.first().isArray()) {
        for (const QJsonValue& v : arr) {
            if (!v.isArray()) {
                *error = QObject::tr("All records must be arrays.");
                return false;
            }
            QStringList r;
            for (const QJsonValue& c : v.toArray()) {
                if (c.isObject() || c.isArray()) {
                    *error = QObject::tr("Nested data cannot be represented in CSV.");
                    return false;
                }
                r << jsonScalarToString(c);
            }
            rows << r;
        }
    } else {
        *error = QObject::tr("JSON array must contain objects or arrays.");
        return false;
    }
    *csv = writeCsv(rows, delimiter);
    return true;
}

ConversionResult TextDataConverter::convert(const ConversionRequest& request, ConversionContext& ctx)
{
    QString text, error;
    if (!readText(request.inputPath, &text, &error))
        return ConversionResult::failure(error);
    ctx.stepProgress(0.3);
    const QString from = request.inputFormat, to = request.outputFormat;
    const QVariantMap& o = request.options;
    QByteArray out;

    if (from == "csv" && to == "json") {
        const QString d = o.value("delimiter", "auto").toString();
        if (!csvToJson(text, o.value("header", true).toBool(), o.value("inferTypes", true).toBool(),
                       d == "auto" ? QChar() : d.at(0), &out, &error))
            return ConversionResult::failure(error);
    } else if (from == "json" && to == "csv") {
        QString csv;
        const QString d = o.value("delimiter", ",").toString();
        if (!jsonToCsv(text.toUtf8(), d.isEmpty() || d == "auto" ? QLatin1Char(',') : d.at(0), &csv, &error))
            return ConversionResult::failure(error);
        out = "\xEF\xBB\xBF" + csv.toUtf8(); // BOM so Excel opens UTF-8 correctly
    } else {
        QTextDocument doc;
        if (from == "md")
            doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
        else
            doc.setHtml(text);
        if (to == "html") {
            QString html = doc.toHtml();
            html.replace("<head>", "<head><meta charset=\"utf-8\">");
            out = html.toUtf8();
        } else if (to == "md") {
            out = doc.toMarkdown(QTextDocument::MarkdownDialectGitHub).toUtf8();
        } else if (to == "txt") {
            out = doc.toPlainText().toUtf8();
        } else if (to == "pdf") {
            QPdfWriter writer(request.outputPath);
            writer.setCreator("Folvio");
            writer.setTitle(QFileInfo(request.inputPath).completeBaseName());
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setPageMargins(QMarginsF(20, 20, 20, 20), QPageLayout::Millimeter);
            writer.setResolution(300);
            QFont base(QStringLiteral("Segoe UI"), 11);
            doc.setDefaultFont(base);
            doc.setDefaultStyleSheet("pre, code { font-family: Consolas, monospace; } table { border-collapse: collapse; }"
                                     " td, th { border: 1px solid #999; padding: 4px; }");
            if (from == "md")
                doc.setMarkdown(text, QTextDocument::MarkdownDialectGitHub);
            doc.print(&writer);
            ConversionResult r;
            r.ok = QFileInfo(request.outputPath).size() > 0;
            if (!r.ok)
                r.error = QObject::tr("PDF could not be written.");
            r.outputs << request.outputPath;
            return r;
        } else {
            return ConversionResult::failure(QObject::tr("Unsupported conversion."));
        }
    }
    if (!writeBytes(request.outputPath, out, &error))
        return ConversionResult::failure(error);
    ConversionResult r;
    r.ok = true;
    r.outputs << request.outputPath;
    return r;
}

} // namespace folvio
