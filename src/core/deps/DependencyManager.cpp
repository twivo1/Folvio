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
    const QString pf = envPath("ProgramFiles");
    const QString pf86 = envPath("ProgramFiles(x86)");
    const QString local = envPath("LOCALAPPDATA");
    const QString bundled = appDir() + QStringLiteral("/engines");

    if (id == "pdfium") {
        t.displayName = QStringLiteral("PDFium");
        t.license = QStringLiteral("BSD-3-Clause / Apache-2.0");
        t.purpose = QObject::tr("PDF rendering, analysis and native editing (built in)");
        t.path = appDir() + QStringLiteral("/pdfium.dll");
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
        t.installHint = QStringLiteral("winget install Gyan.FFmpeg");
        const QString exe = id + QStringLiteral(".exe");
        t.path = firstExisting({overridePath, envOverride, bundled + "/ffmpeg/" + exe, bundled + "/ffmpeg/bin/" + exe,
                                local + "/Microsoft/WinGet/Links/" + exe,
                                QStandardPaths::findExecutable(id)});
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
        }
    } else if (id == "tesseract") {
        t.displayName = QStringLiteral("Tesseract OCR");
        t.license = QStringLiteral("Apache-2.0, separate process");
        t.purpose = QObject::tr("Text recognition for scanned PDFs and images");
        t.installHint = QStringLiteral("winget install UB-Mannheim.TesseractOCR");
        t.path = firstExisting({overridePath, envOverride, bundled + "/tesseract/tesseract.exe",
                                pf + "/Tesseract-OCR/tesseract.exe", local + "/Programs/Tesseract-OCR/tesseract.exe",
                                QStandardPaths::findExecutable(QStringLiteral("tesseract"))});
        if (!t.path.isEmpty())
            t.version = versionOf(t.path, {"--version"}, QStringLiteral("tesseract v?(\\S+)"));
    } else if (id == "word") {
        t.displayName = QStringLiteral("Microsoft Word");
        t.license = QObject::tr("Proprietary, uses the user's own Office licence (optional)");
        t.purpose = QObject::tr("High quality PDF to DOCX reconstruction");
        t.installHint = QObject::tr("Optional. Install Microsoft 365 / Office.");
        QSettings clsid(QStringLiteral("HKEY_CLASSES_ROOT\\Word.Application\\CLSID"), QSettings::NativeFormat);
        const QString powershell = firstExisting({envPath("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe"});
        const QString script = firstExisting({bundled + "/office/word_convert.ps1"});
        if (!clsid.value(QStringLiteral(".")).toString().isEmpty() && !powershell.isEmpty() && !script.isEmpty()) {
            t.path = powershell;
            QSettings ver(QStringLiteral("HKEY_CLASSES_ROOT\\Word.Application\\CurVer"), QSettings::NativeFormat);
            t.version = ver.value(QStringLiteral(".")).toString();
        }
    }
    t.available = !t.path.isEmpty();
    return t;
}

QStringList DependencyManager::tesseractLanguages()
{
    QStringList langs;
    const QStringList dirs = {appDir() + "/engines/tessdata", PathUtils::appDataDir() + "/tessdata",
                              QFileInfo(path("tesseract")).absolutePath() + "/tessdata"};
    for (const QString& d : dirs)
        for (const QFileInfo& fi : QDir(d).entryInfoList({"*.traineddata"}, QDir::Files))
            if (!langs.contains(fi.completeBaseName()))
                langs << fi.completeBaseName();
    return langs;
}

QString DependencyManager::tessdataDir(const QStringList& languages)
{
    const QStringList dirs = {appDir() + "/engines/tessdata", PathUtils::appDataDir() + "/tessdata",
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
