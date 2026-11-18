#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace folvio {

enum class FormatCategory {
    Pdf,
    Document,
    Spreadsheet,
    Presentation,
    Text,
    Data,
    Image,
    Audio,
    Video,
    Unknown
};

QString categoryName(FormatCategory c);

struct FormatInfo {
    QString id;              // canonical lower-case id, e.g. "pdf", "docx"
    QString displayName;     // "PDF document"
    FormatCategory category = FormatCategory::Unknown;
    QStringList extensions;  // without dot, first is preferred
    bool lossy = false;      // lossy encoding (jpg, mp3, ...)
};

// Static catalogue of formats Folvio knows about. Knowing a format does not
// mean it can be converted - that is decided by the ConversionRegistry.
class FormatRegistry {
public:
    static const QList<FormatInfo>& all();
    static const FormatInfo* byId(const QString& id);
    static QString idForExtension(const QString& extension);
    static QString preferredExtension(const QString& id);
    static QString displayName(const QString& id);
    static FormatCategory category(const QString& id);
};

struct DetectedFile {
    QString path;
    QString formatId;          // empty if unknown
    QString extensionFormatId; // what the extension claims
    bool contentMatchesExtension = true;
    qint64 size = 0;
    QString error;             // non-empty if file cannot be read
    bool ok() const { return error.isEmpty() && !formatId.isEmpty(); }
};

// Detects formats using magic bytes first, then the extension.
class FormatDetector {
public:
    static DetectedFile detect(const QString& path);
    // Exposed for tests: classify a file header. Returns empty if unknown.
    static QString sniff(const QByteArray& head, const QString& extensionHint);
};

} // namespace folvio
