#include "core/files/PathUtils.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace folvio {

namespace {
QString ensureDir(const QString& path)
{
    QDir().mkpath(path);
    return path;
}
} // namespace

QString PathUtils::appDataDir()
{
    QString base = qEnvironmentVariable("FOLVIO_DATA_DIR");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Folvio");
    return ensureDir(QDir::cleanPath(base));
}

QString PathUtils::backupDir() { return ensureDir(appDataDir() + QStringLiteral("/backups")); }
QString PathUtils::tempRoot() { return ensureDir(appDataDir() + QStringLiteral("/temp")); }
QString PathUtils::recoveryDir() { return ensureDir(appDataDir() + QStringLiteral("/recovery")); }
QString PathUtils::logDir() { return ensureDir(appDataDir() + QStringLiteral("/logs")); }

QString PathUtils::uniquePath(const QString& desiredPath)
{
    QFileInfo fi(desiredPath);
    if (!fi.exists())
        return desiredPath;
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QStringLiteral(".") + fi.suffix();
    for (int i = 1; i < 10000; ++i) {
        const QString candidate = QStringLiteral("%1/%2 (%3)%4").arg(dir, base).arg(i).arg(suffix);
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
    // Pathological case: fall back to timestamp.
    return QStringLiteral("%1/%2 %3%4").arg(dir, base, QString::number(QDateTime::currentMSecsSinceEpoch()), suffix);
}

QString PathUtils::sanitizeFileName(const QString& name)
{
    QString s = name;
    static const QRegularExpression illegal(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    s.replace(illegal, QStringLiteral("_"));
    while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    s = s.trimmed();
    static const QStringList reserved = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                         "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                         "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const QString stem = s.section(QLatin1Char('.'), 0, 0).toUpper();
    if (reserved.contains(stem))
        s.prepend(QLatin1Char('_'));
    // Keep names well below the 255 UTF-16 unit component limit (room for " (123).ext").
    if (s.size() > 180)
        s = s.left(180).trimmed();
    if (s.isEmpty())
        s = QStringLiteral("output");
    return s;
}

QString PathUtils::outputPathFor(const QString& inputPath, const QString& outputDir, const QString& extension)
{
    const QFileInfo fi(inputPath);
    const QString dir = outputDir.isEmpty() ? fi.absolutePath() : outputDir;
    const QString base = sanitizeFileName(fi.completeBaseName());
    return QDir(dir).filePath(base + QLatin1Char('.') + extension);
}

bool PathUtils::samePath(const QString& a, const QString& b)
{
    const QString ca = QFileInfo(a).canonicalFilePath();
    const QString cb = QFileInfo(b).canonicalFilePath();
    const QString pa = ca.isEmpty() ? QDir::cleanPath(QFileInfo(a).absoluteFilePath()) : ca;
    const QString pb = cb.isEmpty() ? QDir::cleanPath(QFileInfo(b).absoluteFilePath()) : cb;
#ifdef Q_OS_WIN
    return pa.compare(pb, Qt::CaseInsensitive) == 0;
#else
    return pa == pb;
#endif
}

QString PathUtils::toLongPathSafe(const QString& path)
{
#ifdef Q_OS_WIN
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (native.startsWith(QStringLiteral("\\\\?\\")))
        return native;
    if (native.startsWith(QStringLiteral("\\\\")))
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    return QStringLiteral("\\\\?\\") + native;
#else
    return path;
#endif
}

int PathUtils::cleanupStaleTemp(int maxAgeHours)
{
    int removed = 0;
    QDir root(tempRoot());
    const QDateTime limit = QDateTime::currentDateTime().addSecs(-3600LL * maxAgeHours);
    for (const QFileInfo& fi : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.lastModified() < limit) {
            if (QDir(fi.absoluteFilePath()).removeRecursively())
                ++removed;
        }
    }
    return removed;
}

} // namespace folvio
