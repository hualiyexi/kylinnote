#include "Database.h"

#include "../Common.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace kn {

const char *Database::kConnectionName = "kylinnote_main";

Database::Database() = default;

Database &Database::instance()
{
    static Database db;
    return db;
}

bool Database::open(QString *error)
{
    if (isOpen()) {
        return true;
    }

    const QString dir = ensureDataDir();
    if (dir.isEmpty()) {
        m_lastError = QStringLiteral("无法创建数据目录: ") + dataDirPath();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    QSqlDatabase conn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                  QLatin1String(kConnectionName));
    conn.setDatabaseName(databasePath());
    if (!conn.open()) {
        m_lastError = conn.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    if (!applyPragmas(error)) {
        return false;
    }

    QSqlQuery q(conn);
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        m_lastError = QStringLiteral("读取 user_version 失败: ") + q.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    const int version = q.value(0).toInt();

    if (!migrate(version, 1, error)) {
        return false;
    }

    m_lastError.clear();
    return true;
}

void Database::close()
{
    if (QSqlDatabase::contains(QLatin1String(kConnectionName))) {
        {
            QSqlDatabase conn = QSqlDatabase::database(QLatin1String(kConnectionName), false);
            if (conn.isOpen()) {
                conn.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
                conn.close();
            }
        }
        QSqlDatabase::removeDatabase(QLatin1String(kConnectionName));
    }
}

bool Database::isOpen() const
{
    if (!QSqlDatabase::contains(QLatin1String(kConnectionName))) {
        return false;
    }
    QSqlDatabase conn = QSqlDatabase::database(QLatin1String(kConnectionName), false);
    return conn.isOpen();
}

QSqlDatabase Database::connection() const
{
    return QSqlDatabase::database(QLatin1String(kConnectionName), false);
}

bool Database::applyPragmas(QString *error)
{
    QSqlDatabase conn = connection();
    const char *pragmas[] = {
        "PRAGMA journal_mode = WAL",
        "PRAGMA synchronous = NORMAL",
        "PRAGMA busy_timeout = 5000",
        "PRAGMA foreign_keys = ON"
    };
    for (const char *sql : pragmas) {
        QSqlQuery q(conn);
        if (!q.exec(QLatin1String(sql))) {
            m_lastError = QStringLiteral("执行 %1 失败: %2")
                              .arg(QLatin1String(sql), q.lastError().text());
            if (error) {
                *error = m_lastError;
            }
            return false;
        }
    }
    return true;
}

bool Database::migrate(int from, int to, QString *error)
{
    QSqlDatabase conn = connection();
    Q_UNUSED(from);

    // 首次建库（表结构按需求固定，后续升级在此追加版本分支）
    QSqlQuery q(conn);
    const QString createTable = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS notes ("
        " id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        " title      TEXT NOT NULL DEFAULT '',"
        " content    TEXT NOT NULL DEFAULT '',"
        " color      INTEGER NOT NULL DEFAULT 0,"
        " geometry   TEXT NOT NULL DEFAULT '',"
        " z_order    INTEGER NOT NULL DEFAULT 0,"
        " created_at TEXT NOT NULL DEFAULT '',"
        " updated_at TEXT NOT NULL DEFAULT '',"
        " deleted    INTEGER NOT NULL DEFAULT 0"
        ")");
    if (!q.exec(createTable)) {
        m_lastError = QStringLiteral("建表失败: ") + q.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_notes_deleted ON notes(deleted)"))) {
        m_lastError = QStringLiteral("创建索引失败: ") + q.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }

    if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(to))) {
        m_lastError = QStringLiteral("写入 user_version 失败: ") + q.lastError().text();
        if (error) {
            *error = m_lastError;
        }
        return false;
    }
    return true;
}

} // namespace kn
