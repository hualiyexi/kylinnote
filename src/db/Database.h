#ifndef KYLINLIST_DATABASE_H
#define KYLINLIST_DATABASE_H

#include <QSqlDatabase>
#include <QString>

namespace kl {

// 主线程专用 QSQLITE 连接。只接受空库或 KylinList application_id + user_version∈{5,6,7}；
// v5/v6 在打开时于各自单事务内升级到 v7（v5: lists 增加 icon/color/archived_at_utc_ms；
// v5/v6 均再给 folders 增加 icon）。失败整体回滚并拒绝打开。
class Database
{
public:
    static Database &instance();
    static int schemaVersion() { return 7; }
    static int applicationId() { return 0x4B4C5354; } // "KLST"

    bool open(QString *error = nullptr);
    bool openAt(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QSqlDatabase connection() const;
    QString path() const { return m_path; }
    QString lastError() const { return m_lastError; }

private:
    Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    bool applyPragmas(QString *error);
    bool initializeSchema(QString *error);
    bool migrateFromV5(QString *error);
    bool migrateFromV6(QString *error);
    bool fail(const QString &message, QString *error);

    static const char *kConnectionName;
    QString m_lastError;
    QString m_path;
    bool m_ready = false;
};

} // namespace kl

#endif // KYLINLIST_DATABASE_H
