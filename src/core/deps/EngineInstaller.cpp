#include "core/deps/EngineInstaller.h"

#include "core/deps/DependencyManager.h"
#include "core/process/ProcessRunner.h"

#include <QFile>
#include <QStandardPaths>

namespace folvio {

QStringList EngineInstaller::missingEngines()
{
    QStringList out;
    for (const char* id : {"ffmpeg", "soffice", "tesseract"})
        if (!DependencyManager::instance().available(QString::fromLatin1(id)))
            out << QString::fromLatin1(id);
    return out;
}

QString EngineInstaller::wingetId(const QString& engine)
{
    if (engine == "ffmpeg" || engine == "ffprobe")
        return QStringLiteral("Gyan.FFmpeg");
    if (engine == "soffice")
        return QStringLiteral("TheDocumentFoundation.LibreOffice");
    if (engine == "tesseract")
        return QStringLiteral("UB-Mannheim.TesseractOCR");
    return {};
}

namespace {
QString wingetExe()
{
#ifdef Q_OS_WIN
    QString p = QStandardPaths::findExecutable(QStringLiteral("winget"));
    if (p.isEmpty()) {
        const QString local = qEnvironmentVariable("LOCALAPPDATA");
        const QString candidate = local + "/Microsoft/WindowsApps/winget.exe";
        if (QFile::exists(candidate))
            p = candidate;
    }
    return p;
#else
    return {};
#endif
}
} // namespace

bool EngineInstaller::wingetAvailable()
{
    return !wingetExe().isEmpty();
}

bool EngineInstaller::installWithWinget(const QString& engine, QString* log, const std::atomic_bool* cancel)
{
    const QString id = wingetId(engine);
    const QString exe = wingetExe();
    if (id.isEmpty() || exe.isEmpty()) {
        if (log)
            *log = QObject::tr("winget is not available on this computer.");
        return false;
    }
    ProcessSpec spec;
    spec.program = exe;
    // --exact: only this package id. The installers may ask Windows for administrator rights (UAC).
    spec.arguments = {"install", "--id", id, "--exact", "--source", "winget", "--accept-package-agreements",
                      "--accept-source-agreements", "--disable-interactivity"};
    spec.timeoutMs = 30 * 60 * 1000;
    const ProcessResult r = ProcessRunner::run(spec, [cancel] { return cancel && cancel->load(); });
    if (log)
        *log = QString::fromLocal8Bit(r.stdOut + "\n" + r.stdErr).trimmed();
    // winget returns 0 on success and a specific code when the package is already installed.
    const bool already = r.exitCode == int(0x8A15002B) || r.stdOut.contains("already installed");
    DependencyManager::instance().refresh();
    return r.started && !r.cancelled && !r.timedOut && (r.exitCode == 0 || already);
}

QString EngineInstaller::downloadPage(const QString& engine)
{
    if (engine == "ffmpeg" || engine == "ffprobe")
        return QStringLiteral("https://ffmpeg.org/download.html");
    if (engine == "soffice")
        return QStringLiteral("https://www.libreoffice.org/download/download-libreoffice/");
    if (engine == "tesseract")
        return QStringLiteral("https://github.com/UB-Mannheim/tesseract/wiki");
    return {};
}

QString EngineInstaller::linuxDistributionId()
{
    QFile f(QStringLiteral("/etc/os-release"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QString id, like;
    for (const QByteArray& raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith("ID="))
            id = line.mid(3).remove('"');
        else if (line.startsWith("ID_LIKE="))
            like = line.mid(8).remove('"');
    }
    for (const QString& known : {"ubuntu", "debian", "fedora", "rhel", "arch", "opensuse", "suse"})
        if (id == known || like.split(' ').contains(known))
            return known == "rhel" ? QStringLiteral("fedora") : known == "suse" ? QStringLiteral("opensuse") : known;
    return id;
}

QString EngineInstaller::linuxInstallCommand(const QStringList& engines)
{
    const QString distro = linuxDistributionId();
    QStringList pkgs;
    for (const QString& e : engines) {
        if (distro == "ubuntu" || distro == "debian") {
            if (e == "ffmpeg") pkgs << "ffmpeg";
            if (e == "soffice") pkgs << "libreoffice-writer" << "libreoffice-calc" << "libreoffice-impress";
            if (e == "tesseract") pkgs << "tesseract-ocr";
        } else if (distro == "fedora") {
            if (e == "ffmpeg") pkgs << "ffmpeg-free";
            if (e == "soffice") pkgs << "libreoffice-writer" << "libreoffice-calc" << "libreoffice-impress";
            if (e == "tesseract") pkgs << "tesseract";
        } else if (distro == "arch") {
            if (e == "ffmpeg") pkgs << "ffmpeg";
            if (e == "soffice") pkgs << "libreoffice-fresh";
            if (e == "tesseract") pkgs << "tesseract";
        } else if (distro == "opensuse") {
            if (e == "ffmpeg") pkgs << "ffmpeg";
            if (e == "soffice") pkgs << "libreoffice-writer" << "libreoffice-calc" << "libreoffice-impress";
            if (e == "tesseract") pkgs << "tesseract-ocr";
        }
    }
    if (pkgs.isEmpty())
        return {};
    if (distro == "ubuntu" || distro == "debian")
        return "sudo apt install " + pkgs.join(' ');
    if (distro == "fedora")
        return "sudo dnf install " + pkgs.join(' ');
    if (distro == "arch")
        return "sudo pacman -S " + pkgs.join(' ');
    return "sudo zypper install " + pkgs.join(' ');
}

} // namespace folvio
