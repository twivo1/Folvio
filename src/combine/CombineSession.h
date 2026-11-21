#pragma once

#include "combine/CombineModel.h"

#include <QImage>
#include <QStringList>

class QMimeData;

namespace folvio::combine {

struct ImportResult {
    QList<CombinePage> pages;
    QStringList errors;   // one entry per rejected input, user-readable
};

// Importing images, clipboard data, autosave/recovery and .folvio project files.
// Source files are never modified; clipboard bitmaps and converted HEIC/AVIF images are
// stored as PNG in the session's asset folder.
class CombineSession {
public:
    static QString sessionDir();   // %LOCALAPPDATA%\Folvio\combine\session
    static QString assetsDir();
    // Auto Change image editor: its pasted/converted images live apart from the Combine session,
    // so neither workspace can delete the other's files.
    static QString imageEditorAssetsDir();

    static bool isSupportedFile(const QString& path);
    // Validates and fingerprints files (safe to call from a worker thread).
    // 'assetsDir' (empty = the Combine session) receives converted HEIC/AVIF and clipboard images.
    static ImportResult importFiles(const QStringList& paths, const QString& assetsDir = {});
    // Clipboard / drop: bitmap images and/or local file URLs.
    static ImportResult importMime(const QMimeData* mime, const QString& assetsDir = {});
    static bool mimeHasImportable(const QMimeData* mime);
    static CombinePage pageFromImage(const QImage& image, const QString& name, QString* error, const QString& assetsDir = {});

    // Autosave of the working session (structure + edits; images stay where they are).
    static bool autosave(const CombineDocument& doc, QString* error = nullptr);
    static bool hasRecoverableSession();
    static bool restoreSession(CombineDocument& doc, QString* error);
    static void discardSession(const CombineDocument* keepAssetsOf = nullptr);

    // Project files (.folvio): document JSON; clipboard/converted images are embedded,
    // file sources are referenced (optionally embedded too).
    static bool saveProject(const CombineDocument& doc, const QString& path, bool embedAllImages, QString* error);
    static bool openProject(CombineDocument& doc, const QString& path, QStringList* warnings, QString* error);
    // Pages whose source file is missing or changed since import.
    static QStringList sourceProblems(const CombineDocument& doc);
};

} // namespace folvio::combine
