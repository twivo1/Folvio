#include "core/persistence/Database.h"

#include "core/files/PathUtils.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace folvio {

Database& Database::instance()
{
    static Database db;
    return db;
}

bool Database::open(const QString& path)
{
    if (m_open)
        return true;
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path.isEmpty() ? PathUtils::appDataDir() + QStringLiteral("/folvio.db") : path);
    if (!db.open()) {
        qWarning("Folvio: cannot open database: %s", qPrintable(db.lastError().text()));
        return false;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS history (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, "
                          "input_path TEXT, input_format TEXT, output_path TEXT, output_format TEXT, status TEXT, "
                          "duration_ms INTEGER, message TEXT)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS recent (path TEXT PRIMARY KEY, format TEXT, mode TEXT, ts INTEGER)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)"));
    q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)"));
    q.exec(QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES('schema', '1')"));
    m_open = true;
    return true;
}

void Database::close()
{
    if (!m_open)
        return;
    QSqlDatabase::database(m_connection).close();
    m_open = false;
    QSqlDatabase::removeDatabase(m_connection);
}

void Database::addHistory(const HistoryEntry& e)
{
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("INSERT INTO history(ts, input_path, input_format, output_path, output_format, status, "
                             "duration_ms, message) VALUES(?,?,?,?,?,?,?,?)"));
    q.addBindValue(e.when.isValid() ? e.when.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(e.inputPath);
    q.addBindValue(e.inputFormat);
    q.addBindValue(e.outputPath);
    q.addBindValue(e.outputFormat);
    q.addBindValue(e.status);
    q.addBindValue(e.durationMs);
    q.addBindValue(e.message.left(500));
    q.exec();
    // Keep the table bounded.
    q.exec(QStringLiteral("DELETE FROM history WHERE id NOT IN (SELECT id FROM history ORDER BY id DESC LIMIT 2000)"));
}

QList<HistoryEntry> Database::history(int limit) const
{
    QList<HistoryEntry> list;
    if (!m_open)
        return list;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("SELECT id, ts, input_path, input_format, output_path, output_format, status, duration_ms, "
                             "message FROM history ORDER BY id DESC LIMIT ?"));
    q.addBindValue(limit);
    q.exec();
    while (q.next()) {
        HistoryEntry e;
        e.id = q.value(0).toLongLong();
        e.when = QDateTime::fromMSecsSinceEpoch(q.value(1).toLongLong());
        e.inputPath = q.value(2).toString();
        e.inputFormat = q.value(3).toString();
        e.outputPath = q.value(4).toString();
        e.outputFormat = q.value(5).toString();
        e.status = q.value(6).toString();
        e.durationMs = q.value(7).toLongLong();
        e.message = q.value(8).toString();
        list << e;
    }
    return list;
}

void Database::clearHistory()
{
    if (m_open)
        QSqlQuery(QSqlDatabase::database(m_connection)).exec(QStringLiteral("DELETE FROM history"));
}

void Database::touchRecent(const QString& path, const QString& format, const QString& mode)
{
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("INSERT INTO recent(path, format, mode, ts) VALUES(?,?,?,?) "
                             "ON CONFLICT(path) DO UPDATE SET format=excluded.format, mode=excluded.mode, ts=excluded.ts"));
    q.addBindValue(path);
    q.addBindValue(format);
    q.addBindValue(mode);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    q.exec(QStringLiteral("DELETE FROM recent WHERE path NOT IN (SELECT path FROM recent ORDER BY ts DESC LIMIT 100)"));
}

QList<RecentFile> Database::recentFiles(int limit) const
{
    QList<RecentFile> list;
    if (!m_open)
        return list;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("SELECT path, format, mode, ts FROM recent ORDER BY ts DESC LIMIT ?"));
    q.addBindValue(limit);
    q.exec();
    while (q.next())
        list << RecentFile{q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                           QDateTime::fromMSecsSinceEpoch(q.value(3).toLongLong())};
    return list;
}

void Database::removeRecent(const QString& path)
{
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("DELETE FROM recent WHERE path = ?"));
    q.addBindValue(path);
    q.exec();
}

void Database::clearRecent()
{
    if (m_open)
        QSqlQuery(QSqlDatabase::database(m_connection)).exec(QStringLiteral("DELETE FROM recent"));
}

QVariant Database::setting(const QString& key, const QVariant& def) const
{
    if (!m_open)
        return def;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next())
        return q.value(0);
    return def;
}

void Database::setSetting(const QString& key, const QVariant& value)
{
    if (!m_open)
        return;
    QSqlQuery q(QSqlDatabase::database(m_connection));
    q.prepare(QStringLiteral("INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value.toString());
    q.exec();
}

} // namespace folvio
