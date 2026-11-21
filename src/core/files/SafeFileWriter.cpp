#include "core/files/SafeFileWriter.h"

#include "core/files/PathUtils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <atomic>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace folvio {

namespace {
std::atomic<SafeFileWriter::FailurePoint> g_failure{SafeFileWriter::FailurePoint::None};

std::wstring wpath(const QString& p) { return PathUtils::toLongPathSafe(p).toStdWString(); }

bool copyVerified(const QString& from, const QString& to, QString* error)
{
    QFile::remove(to);
    if (!QFile::copy(from, to)) {
        if (error)
            *error = QObject::tr("Could not copy %1 to %2.").arg(from, to);
        return false;
    }
    if (sha256OfFile(from) != sha256OfFile(to)) {
        QFile::remove(to);
        if (error)
            *error = QObject::tr("Backup verification failed.");
        return false;
    }
    // QFile::copy keeps the read-only attribute of the source; backups must be deletable.
    QFile(to).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
    return true;
}
} // namespace

QByteArray sha256OfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result();
}

FileFingerprint FileFingerprint::of(const QString& path, bool withHash)
{
    FileFingerprint fp;
    QFileInfo fi(path);
    if (!fi.exists())
        return fp;
    fp.size = fi.size();
    fp.modified = fi.lastModified();
    if (withHash)
        fp.sha256 = sha256OfFile(path);
    return fp;
}

bool FileFingerprint::sameContentAs(const FileFingerprint& o) const
{
    if (size != o.size)
        return false;
    if (!sha256.isEmpty() && !o.sha256.isEmpty())
        return sha256 == o.sha256;
    return modified == o.modified;
}

void SafeFileWriter::setFailureInjection(FailurePoint p) { g_failure = p; }

bool SafeFileWriter::isWritable(const QString& path, QString* reason)
{
    QFileInfo fi(path);
    if (fi.exists()) {
#ifdef Q_OS_WIN
        const DWORD attrs = GetFileAttributesW(wpath(path).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            if (reason)
                *reason = QObject::tr("The file is marked read-only.");
            return false;
        }
#else
        // No write bit at all (chmod a-w) is an explicit "read-only" mark, like the Windows attribute;
        // respect it even when the process could write anyway (root).
        const auto perms = QFile::permissions(path);
        if (!(perms & (QFileDevice::WriteOwner | QFileDevice::WriteUser | QFileDevice::WriteGroup | QFileDevice::WriteOther))) {
            if (reason)
                *reason = QObject::tr("The file is marked read-only.");
            return false;
        }
#endif
        if (!fi.isWritable()) {
            if (reason)
                *reason = QObject::tr("You do not have permission to modify this file.");
            return false;
        }
        // Detect files locked by other programs (e.g. open in Acrobat).
        QFile f(path);
        if (!f.open(QIODevice::ReadWrite)) {
            if (reason)
                *reason = QObject::tr("The file is locked by another program: %1").arg(f.errorString());
            return false;
        }
        return true;
    }
    QFileInfo dir(fi.absolutePath());
    if (!dir.exists() || !dir.isWritable()) {
        if (reason)
            *reason = QObject::tr("The destination folder is not writable.");
        return false;
    }
    return true;
}

QString SafeFileWriter::makeTempPathNextTo(const QString& target)
{
    const QFileInfo fi(target);
    const QString rnd = QString::number(QRandomGenerator::global()->generate64(), 36);
    QString name = QStringLiteral(".~folvio-%1.tmp").arg(rnd);
    return QDir(fi.absolutePath()).filePath(name);
}

bool SafeFileWriter::atomicSwap(const QString& target, const QString& temp, QString* error)
{
#ifdef Q_OS_WIN
    const std::wstring t = wpath(target), s = wpath(temp);
    if (QFileInfo::exists(target)) {
        // ReplaceFileW keeps the original's ACLs, attributes and creation time.
        if (ReplaceFileW(t.c_str(), s.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                         nullptr, nullptr))
            return true;
        const DWORD err = GetLastError();
        // Fall back for file systems without ReplaceFile support (some network shares).
        if (err != ERROR_UNABLE_TO_MOVE_REPLACEMENT_2
            && MoveFileExW(s.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        if (error)
            *error = QObject::tr("Windows could not replace the file (error %1).").arg(err);
        return false;
    }
    if (MoveFileExW(s.c_str(), t.c_str(), MOVEFILE_WRITE_THROUGH))
        return true;
    if (error)
        *error = QObject::tr("Windows could not move the file into place (error %1).").arg(GetLastError());
    return false;
#else
    // Keep the original's permission bits, flush the data, then rename (atomic on POSIX file systems).
    if (QFileInfo::exists(target))
        QFile::setPermissions(temp, QFile::permissions(target));
    {
        const int fd = ::open(QFile::encodeName(temp).constData(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
    if (std::rename(QFile::encodeName(temp).constData(), QFile::encodeName(target).constData()) == 0) {
        const int dirFd = ::open(QFile::encodeName(QFileInfo(target).absolutePath()).constData(), O_RDONLY | O_DIRECTORY);
        if (dirFd >= 0) {
            ::fsync(dirFd); // make the rename itself durable
            ::close(dirFd);
        }
        return true;
    }
    if (error)
        *error = QObject::tr("Could not move the new file into place: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
    return false;
#endif
}

SafeFileWriter::Result SafeFileWriter::replace(const QString& targetPath, const Producer& produce,
                                               const Validator& validate, const SafeSaveOptions& options)
{
    Result r;
    QString reason;
    if (!isWritable(targetPath, &reason)) {
        r.status = Status::ReadOnly;
        r.error = reason;
        return r;
    }
    const bool targetExists = QFileInfo::exists(targetPath);
    FileFingerprint before;
    if (targetExists) {
        before = FileFingerprint::of(targetPath);
        if (options.expectedOriginal.isValid() && !before.sameContentAs(options.expectedOriginal)) {
            r.status = Status::ExternallyModified;
            r.error = QObject::tr("The file was changed by another program after it was opened.");
            return r;
        }
    }

    const QString temp = makeTempPathNextTo(targetPath);
    struct TempGuard {
        QString path;
        ~TempGuard() { if (!path.isEmpty()) QFile::remove(path); }
    } guard{temp};

    QString err;
    if (!produce(temp, &err) || !QFileInfo::exists(temp) || QFileInfo(temp).size() == 0) {
        r.status = Status::ProducerFailed;
        r.error = err.isEmpty() ? QObject::tr("The new file could not be written.") : err;
        return r;
    }
#ifdef Q_OS_WIN
    SetFileAttributesW(wpath(temp).c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
    if (validate && !validate(temp, &err)) {
        r.status = Status::ValidationFailed;
        r.error = QObject::tr("The new file failed verification: %1").arg(err);
        return r;
    }
    const QByteArray newHash = sha256OfFile(temp);

    if (targetExists && options.createBackup) {
        const QString dir = options.backupDir.isEmpty() ? PathUtils::backupDir() : options.backupDir;
        QDir().mkpath(dir);
        const QFileInfo fi(targetPath);
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
        const QString backup = QDir(dir).filePath(
            PathUtils::sanitizeFileName(QStringLiteral("%1 [%2].%3").arg(fi.completeBaseName(), stamp, fi.suffix())));
        if (!copyVerified(targetPath, backup, &err)) {
            r.status = Status::BackupFailed;
            r.error = err;
            return r;
        }
        r.backupPath = backup;
    }

    if (g_failure == FailurePoint::BeforeSwap) {
        r.status = Status::ReplaceFailed;
        r.error = QStringLiteral("Injected failure before swap.");
        return r;
    }

#ifdef Q_OS_WIN
    SetFileAttributesW(wpath(temp).c_str(), FILE_ATTRIBUTE_NORMAL);
#endif
    if (!atomicSwap(targetPath, temp, &err)) {
        r.status = Status::ReplaceFailed;
        r.error = err;
        // The original is untouched when the swap fails, but verify anyway.
        if (targetExists && !FileFingerprint::of(targetPath).sameContentAs(before) && !r.backupPath.isEmpty()) {
            copyVerified(r.backupPath, targetPath, nullptr);
            r.status = Status::RestoredAfterFailure;
        }
        return r;
    }
    guard.path.clear();

    if (g_failure == FailurePoint::CorruptAfterSwap) {
        QFile f(targetPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write("corrupted");
    }

    if (sha256OfFile(targetPath) != newHash) {
        r.error = QObject::tr("The saved file does not match what was written.");
        r.status = Status::ReplaceFailed;
        if (!r.backupPath.isEmpty() && copyVerified(r.backupPath, targetPath, nullptr))
            r.status = Status::RestoredAfterFailure;
        return r;
    }
    return r;
}

SafeFileWriter::Result SafeFileWriter::writeNew(const QString& targetPath, const Producer& produce,
                                                const Validator& validate, bool allowOverwrite)
{
    if (QFileInfo::exists(targetPath)) {
        if (!allowOverwrite) {
            Result r;
            r.status = Status::TargetExists;
            r.error = QObject::tr("A file with this name already exists.");
            return r;
        }
        return replace(targetPath, produce, validate, {});
    }
    SafeSaveOptions o;
    o.createBackup = false;
    return replace(targetPath, produce, validate, o);
}

} // namespace folvio
