#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>

namespace folvio {

struct ToolInfo {
    QString id;          // "ffmpeg", "ffprobe", "soffice", "tesseract", "word", "pdfium"
    QString displayName;
    QString path;        // executable (empty if not found)
    QString version;
    QString license;
    QString purpose;
    QString installHint;
    bool available = false;
    bool optional = true;
};

// Locates external engines. Lookup order for each tool:
//   1. explicit override (settings / environment FOLVIO_<TOOL>)
//   2. bundled copy next to the application (engines/<tool>/...)
//   3. well-known install locations
//   4. PATH
class DependencyManager {
public:
    static DependencyManager& instance();

    ToolInfo tool(const QString& id);
    QString path(const QString& id) { return tool(id).path; }
    bool available(const QString& id) { return tool(id).available; }
    QList<ToolInfo> all();

    void setOverride(const QString& id, const QString& path);
    void refresh();

    // Directory with Tesseract language data containing at least the given languages.
    QString tessdataDir(const QStringList& languages = {"eng"});
    QStringList tesseractLanguages();

private:
    DependencyManager() = default;
    ToolInfo detect(const QString& id);

    QMutex m_mutex;
    QHash<QString, ToolInfo> m_cache;
    QHash<QString, QString> m_overrides;
};

} // namespace folvio
