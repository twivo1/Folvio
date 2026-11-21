#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

namespace folvio {

// Installs the optional external engines (FFmpeg, LibreOffice, Tesseract) through the operating
// system's package manager, with the user's consent. Folvio never redistributes these engines
// itself: their licences (GPL/LGPL/MPL) would require shipping the corresponding sources.
class EngineInstaller {
public:
    // Engines that can be installed this way and are currently missing.
    static QStringList missingEngines();

    // Windows: winget package id for an engine ("ffmpeg", "soffice", "tesseract"), or empty.
    static QString wingetId(const QString& engine);
    static bool wingetAvailable();
    // Runs "winget install" for one engine. Returns true on success; 'log' receives the output.
    static bool installWithWinget(const QString& engine, QString* log, const std::atomic_bool* cancel = nullptr);
    // Official download page, used when winget is not available.
    static QString downloadPage(const QString& engine);

    // Linux: the distribution's package manager command installing the given engines,
    // e.g. "sudo apt install ffmpeg tesseract-ocr". Empty if the distribution is unknown.
    static QString linuxInstallCommand(const QStringList& engines);
    static QString linuxDistributionId(); // ID from /etc/os-release ("ubuntu", "fedora", ...)
};

} // namespace folvio
