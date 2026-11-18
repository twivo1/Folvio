#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

class QTextDocument;

namespace folvio {

// Converts documents into the editor's internal model (HTML + resources) and
// back. Conversions to/from office formats go through LibreOffice (or Word for
// PDF reconstruction). None of this is lossless; callers must tell the user.
class RichDocumentIO {
public:
    enum class Kind { Html, Markdown, PlainText };

    struct Imported {
        bool ok = false;
        QString error;
        Kind kind = Kind::Html;
        QString content;     // HTML, Markdown or plain text
        QString baseDir;     // directory for relative resources (images)
        QStringList warnings;
        QString engine;      // which engine reconstructed the document
    };

    static QStringList editableFormats(); // formats that can be opened in the editor
    static bool canEdit(const QString& format);
    static bool canSaveBack(const QString& format, QString* reason = nullptr);

    // Worker-thread safe: may run external engines.
    static Imported importFile(const QString& path, const QString& format, const QString& workDir,
                               const std::atomic_bool* cancel);

    // GUI thread: serialize the document to an intermediate file ("odt", "html", "md", "txt" or "pdf").
    static bool writeIntermediate(QTextDocument* doc, const QString& format, const QString& path, QString* error);
    // Worker-thread safe: converts an intermediate ODT into docx/doc/rtf/odt.
    static bool convertFromOdt(const QString& odtPath, const QString& targetFormat, const QString& outputPath,
                               const QString& workDir, const std::atomic_bool* cancel, QString* error);
    static QString fidelityWarning(const QString& format);
};

} // namespace folvio
