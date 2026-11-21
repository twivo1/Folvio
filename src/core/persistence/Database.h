#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QVariant>

namespace folvio {

struct HistoryEntry {
    qint64 id = 0;
    QDateTime when;
    QString inputPath;
    QString inputFormat;
    QString outputPath;
    QString outputFormat;
    QString status;   // "ok", "failed", "cancelled"
    qint64 durationMs = 0;
    QString message;
};

struct RecentFile {
    QString path;
    QString format;
    QString mode;     // "convert" or "edit"
    QDateTime lastOpened;
};

// Local SQLite store (%LOCALAPPDATA%\Folvio\folvio.db). Stores metadata only:
// file paths, formats, status. Document contents are never stored here.
// Must be used from the GUI thread.
class Database {
public:
    static Database& instance();
    bool open(const QString& path = {});
    void close();
    bool isOpen() const { return m_open; }

    void addHistory(const HistoryEntry& e);
    QList<HistoryEntry> history(int limit = 200) const;
    void clearHistory();

    void touchRecent(const QString& path, const QString& format, const QString& mode);
    QList<RecentFile> recentFiles(int limit = 30) const;
    void removeRecent(const QString& path);
    void clearRecent();

    QVariant setting(const QString& key, const QVariant& def = {}) const;
    void setSetting(const QString& key, const QVariant& value);

private:
    Database() = default;
    QString m_connection = QStringLiteral("folvio-main");
    bool m_open = false;
};

} // namespace folvio
