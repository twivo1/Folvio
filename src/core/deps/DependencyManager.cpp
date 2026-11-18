#include "core/deps/DependencyManager.h"

#include "core/files/PathUtils.h"
#include "core/process/ProcessRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>


namespace folvio {

DependencyManager& DependencyManager::instance()
{
    static DependencyManager mgr;
    return mgr;
}

void DependencyManager::setOverride(const QString& id, const QString& path)
{
    QMutexLocker lock(&m_mutex);
    if (path.isEmpty())
        m_overrides.remove(id);
    else
        m_overrides.insert(id, path);
    m_cache.remove(id);
}

void DependencyManager::refresh()
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
}

ToolInfo DependencyManager::tool(const QString& id)
{
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_cache.constFind(id);
        if (it != m_cache.constEnd())
            return *it;
    }
    ToolInfo info = detect(id);
    QMutexLocker lock(&m_mutex);
    m_cache.insert(id, info);
    return info;
}

QList<ToolInfo> DependencyManager::all()
{
    QList<ToolInfo> list;
    for (const char* id : {"pdfium", "soffice", "word", "ffmpeg", "ffprobe", "tesseract"})
        list.append(tool(QString::fromLatin1(id)));
    return list;
}

namespace {

QString firstExisting(const QStringList& candidates)
{
    for (const QString& c : candidates)
        if (!c.isEmpty() && QFileInfo(c).isFile())
            return QDir::cleanPath(c);
    return {};
}

QString appDir()
{
    return QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QDir::currentPath();
}

#ifndef Q_OS_WIN
// Expands "parent/pattern + tail", e.g. /opt/libreoffice*/program/soffice (newest first).
QStringList globDirs(const QString& parent, const QString& pattern, const QString& tail)
{
    QStringList out;
    for (const QFileInfo& fi : QDir(parent).entryInfoList({pattern}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed))
        out << fi.absoluteFilePath() + tail;
    return out;
}
#endif

QString envPath(const char* name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

QString versionOf(const QString& exe, const QStringList& args, const QString& pattern)
{
    ProcessSpec spec;
    spec.program = exe;
    spec.arguments = args;
    spec.timeoutMs = 20000;
    const ProcessResult r = ProcessRunner::run(spec);
    const QString text = QString::fromLocal8Bit(r.stdOut + "\n" + r.stdErr);
    const QRegularExpression re(pattern);
    const auto m = re.match(text);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

ToolInfo DependencyManager::detect(const QString& id)
{
    ToolInfo t;
    t.id = id;
    QString overridePath;
    {
        QMutexLocker lock(&m_mutex);
        overridePath = m_overrides.value(id);
    }
    const QString envOverride = envPath(("FOLVIO_" + id.toUpper().toLatin1()).constData());
    const QString bundled = PathUtils::resourceDir();
#ifdef Q_OS_WIN
    const QString pf = envPath("ProgramFiles");
    const QString pf86 = envPath("ProgramFiles(x86)");
    const QString local = envPath("LOCALAPPDATA");
#endif

    if (id == "pdfium") {
        t.displayName = QStringLiteral("PDFium");
        t.license = QStringLiteral("BSD-3-Clause / Apache-2.0");
        t.purpose = QObject::tr("PDF rendering, analysis and native editing (built in)");
        t.path = appDir();
        t.available = true; // linked into the application
        t.optional = false;
        t.version = QStringLiteral("bundled");
        return t;
    }
    if (id == "ffmpeg" || id == "ffprobe") {
        t.displayName = id == "ffmpeg" ? QStringLiteral("FFmpeg") : QStringLiteral("FFprobe");
        t.license = QStringLiteral("LGPL-2.1+ / GPL (depends on build), separate process");
        t.purpose = id == "ffmpeg" ? QObject::tr("Audio, video, HEIC and AVIF conversion")
                                   : QObject::tr("Media stream analysis");
#ifdef Q_OS_WIN
        t.installHint = QStringLiteral("winget install Gyan.FFmpeg");
        const QString exe = id + QStringLiteral(".exe");
        t.path = firstExisting({overridePath, envOverride, bundled + "/ffmpeg/" + exe, bundled + "/ffmpeg/bin/" + exe,
                                local + "/Microsoft/WinGet/Links/" + exe,
                                QStandardPaths::findExecutable(id)});
#else
        t.installHint = QObject::tr("Install FFmpeg from your distribution: sudo apt install ffmpeg (Ubuntu/Debian) "
                                    "or sudo dnf install ffmpeg-free (Fedora)");
        t.path = firstExisting({overridePath, envOverride, bundled + "/ffmpeg/" + id, QStandardPaths::findExecutable(id),
                                "/usr/bin/" + id, "/usr/local/bin/" + id, "/snap/bin/" + id});
#endif
        if (!t.path.isEmpty()) {
            // winget "Links" are symlinks; resolve to the real binary for QProcess.
            const QString target = QFileInfo(t.path).symLinkTarget();
            if (!target.isEmpty() && QFileInfo(target).isFile())
                t.path = target;
            t.version = versionOf(t.path, {"-version"}, QStringLiteral("version (\\S+)"));
        }
    } else if (id == "soffice") {
        t.displayName = QStringLiteral("LibreOffice");
        t.license = QStringLiteral("MPL-2.0, separate process");
        t.purpose = QObject::tr("Office document conversion (DOCX, ODT, RTF, XLSX, PPTX)");
#ifdef Q_OS_WIN
        t.installHint = QStringLiteral("winget install TheDocumentFoundation.LibreOffice");
        QStringList cands = {overridePath, envOverride, bundled + "/libreoffice/program/soffice.com",
                             pf + "/LibreOffice/program/soffice.com", pf86 + "/LibreOffice/program/soffice.com"};
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\LibreOffice\\UNO\\InstallPath"),
                      QSettings::NativeFormat);
        const QString regPath = reg.value(QStringLiteral(".")).toString();
        if (!regPath.isEmpty())
            cands << regPath + "/soffice.com";
        cands << QStandardPaths::findExecutable(QStringLiteral("soffice.com"));
        t.path = firstExisting(cands);
        if (!t.path.isEmpty()) {
            // soffice --version is slow; read the version from the install's version.ini instead.
            QSettings ini(QFileInfo(t.path).absolutePath() + "/version.ini", QSettings::IniFormat);
            t.version = ini.value(QStringLiteral("Version/MsiProductVersion")).toString();
            if (t.version.isEmpty()) {
                QSettings lo(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\LibreOffice\\LibreOffice"), QSettings::NativeFormat);
                const QStringList versions = lo.childGroups();
                if (!versions.isEmpty())
                    t.version = versions.last();
            }
        }
#else
        t.installHint = QObject::tr("Install LibreOffice from your distribution: sudo apt install libreoffice-writer "
                                    "libreoffice-calc libreoffice-impress (Ubuntu/Debian) or sudo dnf install libreoffice (Fedora)");
        QStringList cands = {overridePath, envOverride, QStandardPaths::findExecutable(QStringLiteral("soffice")),
                             QStandardPaths::findExecutable(QStringLiteral("libreoffice")),
                             "/usr/lib/libreoffice/program/soffice", "/usr/lib64/libreoffice/program/soffice",
                             "/snap/bin/libreoffice"};
        cands << globDirs("/opt", "libreoffice*", "/program/soffice");
        t.path = firstExisting(cands);
        if (!t.path.isEmpty()) {
            // Distribution wrappers (/usr/bin/soffice) link into the install; the version is in versionrc.
            // Wrappers may also be shell scripts (Fedora), so fall back to the standard install folders.
            const QString real = QFileInfo(t.path).canonicalFilePath();
            QString rcPath = QFileInfo(real).absolutePath() + "/versionrc";
            for (const QString& dir : {QStringLiteral("/usr/lib/libreoffice/program"), QStringLiteral("/usr/lib64/libreoffice/program")})
                if (!QFileInfo::exists(rcPath))
                    rcPath = dir + "/versionrc";
            QSettings rc(rcPath, QSettings::IniFormat);
            t.version = rc.value(QStringLiteral("Version/ProductKey")).toString().section(' ', -1); // "LibreOffice 24.2"
            if (t.version.isEmpty())
                t.version = rc.value(QStringLiteral("Version/ProductMajor")).toString();
            if (t.version.isEmpty()) { // Debian/Ubuntu builds: UpdateID=LibreOffice_24_en-US_...
                const auto m = QRegularExpression(QStringLiteral("LibreOffice_(\\d+(?:\\.\\d+)?)"))
                                   .match(rc.value(QStringLiteral("Version/UpdateID")).toString());
                if (m.hasMatch())
                    t.version = m.captured(1);
            }
        }
#endif
    } else if (id == "tesseract") {
        t.displayName = QStringLiteral("Tesseract OCR");
        t.license = QStringLiteral("Apache-2.0, separate process");
        t.purpose = QObject::tr("Text recognition for scanned PDFs and images");
#ifdef Q_OS_WIN
        t.installHint = QStringLiteral("winget install UB-Mannheim.TesseractOCR");
        t.path = firstExisting({overridePath, envOverride, bundled + "/tesseract/tesseract.exe",
                                pf + "/Tesseract-OCR/tesseract.exe", local + "/Programs/Tesseract-OCR/tesseract.exe",
                                QStandardPaths::findExecutable(QStringLiteral("tesseract"))});
#else
        t.installHint = QObject::tr("Install Tesseract from your distribution: sudo apt install tesseract-ocr (Ubuntu/Debian) "
                                    "or sudo dnf install tesseract (Fedora). Polish and English data are included with Folvio.");
        t.path = firstExisting({overridePath, envOverride, bundled + "/tesseract/tesseract",
                                QStandardPaths::findExecutable(QStringLiteral("tesseract")), "/usr/bin/tesseract",
                                "/usr/local/bin/tesseract"});
#endif
        if (!t.path.isEmpty())
            t.version = versionOf(t.path, {"--version"}, QStringLiteral("tesseract v?(\\S+)"));
    } else if (id == "word") {
        t.displayName = QStringLiteral("Microsoft Word");
        t.license = QObject::tr("Proprietary, uses the user's own Office licence (optional)");
        t.purpose = QObject::tr("High quality PDF to DOCX reconstruction");
#ifdef Q_OS_WIN
        t.installHint = QObject::tr("Optional. Install Microsoft 365 / Office.");
        QSettings clsid(QStringLiteral("HKEY_CLASSES_ROOT\\Word.Application\\CLSID"), QSettings::NativeFormat);
        const QString powershell = firstExisting({envPath("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe"});
        const QString script = firstExisting({bundled + "/office/word_convert.ps1"});
        if (!clsid.value(QStringLiteral(".")).toString().isEmpty() && !powershell.isEmpty() && !script.isEmpty()) {
            t.path = powershell;
            QSettings ver(QStringLiteral("HKEY_CLASSES_ROOT\\Word.Application\\CurVer"), QSettings::NativeFormat);
            t.version = ver.value(QStringLiteral(".")).toString();
        }
#else
        t.installHint = QObject::tr("Microsoft Word is only available on Windows. On Linux, LibreOffice is used "
                                    "for PDF to DOCX (the layout is reconstructed less faithfully).");
#endif
    }
    t.available = !t.path.isEmpty();
    return t;
}

QStringList DependencyManager::tesseractLanguages()
{
    QStringList langs;
    const QStringList dirs = {PathUtils::resourceDir() + "/tessdata", PathUtils::appDataDir() + "/tessdata",
                              QFileInfo(path("tesseract")).absolutePath() + "/tessdata"};
    for (const QString& d : dirs)
        for (const QFileInfo& fi : QDir(d).entryInfoList({"*.traineddata"}, QDir::Files))
            if (!langs.contains(fi.completeBaseName()))
                langs << fi.completeBaseName();
    return langs;
}

QString DependencyManager::tessdataDir(const QStringList& languages)
{
    const QStringList dirs = {PathUtils::resourceDir() + "/tessdata", PathUtils::appDataDir() + "/tessdata",
                              QFileInfo(path("tesseract")).absolutePath() + "/tessdata"};
    for (const QString& d : dirs) {
        bool all = QDir(d).exists();
        for (const QString& l : languages)
            all = all && QFileInfo::exists(d + "/" + l + ".traineddata");
        if (all)
            return d;
    }
    return {};
}

} // namespace folvio
