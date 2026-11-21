#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <functional>

namespace folvio {

// Identity of a file on disk at a point in time. Used to detect modifications
// made by other programs between "open" and "save".
struct FileFingerprint {
    qint64 size = -1;
    QDateTime modified;
    QByteArray sha256;

    bool isValid() const { return size >= 0; }
    static FileFingerprint of(const QString& path, bool withHash = true);
    bool sameContentAs(const FileFingerprint& other) const;
};

QByteArray sha256OfFile(const QString& path);

struct SafeSaveOptions {
    bool createBackup = true;
    QString backupDir;              // empty: PathUtils::backupDir()
    FileFingerprint expectedOriginal; // if valid, save aborts when the file changed on disk
};

// Writes files without ever leaving the target in a broken state.
//
// replace(): 1) checks writability and external modification, 2) copies a
// verified backup, 3) lets the producer write a temp file in the same folder,
// 4) runs the validator on the temp file, 5) atomically swaps it in with
// ReplaceFileW/MoveFileExW, 6) verifies the result and restores the backup on
// any failure.
class SafeFileWriter {
public:
    using Producer = std::function<bool(const QString& tempPath, QString* error)>;
    using Validator = std::function<bool(const QString& tempPath, QString* error)>;

    enum class Status { Ok, ReadOnly, ExternallyModified, ProducerFailed, ValidationFailed, BackupFailed, ReplaceFailed, RestoredAfterFailure, TargetExists };

    struct Result {
        Status status = Status::Ok;
        QString error;
        QString backupPath;
        bool ok() const { return status == Status::Ok; }
    };

    static Result replace(const QString& targetPath, const Producer& produce, const Validator& validate,
                          const SafeSaveOptions& options = {});

    // Creates a new file (Save As / export). Refuses to overwrite unless allowed;
    // when overwriting it behaves like replace() with a backup.
    static Result writeNew(const QString& targetPath, const Producer& produce, const Validator& validate,
                           bool allowOverwrite);

    static bool isWritable(const QString& path, QString* reason = nullptr);

    // Test hooks: simulate failures at specific points.
    enum class FailurePoint { None, BeforeSwap, CorruptAfterSwap };
    static void setFailureInjection(FailurePoint p);

private:
    static QString makeTempPathNextTo(const QString& target);
    static bool atomicSwap(const QString& target, const QString& temp, QString* error);
};

} // namespace folvio
