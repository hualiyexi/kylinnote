#ifndef KYLINLIST_DATABASE_H
#define KYLINLIST_DATABASE_H

#include <QSqlDatabase>
#include <QString>

namespace kl {

// 主线程专用 QSQLITE 连接。只初始化空库，已有库必须匹配 KylinList application_id 和当前 schema。
// 开发阶段不提供旧版本迁移；不匹配的库在执行任何写入 PRAGMA 前拒绝，保留原文件与内容。
class Database
{
public:
    static Database &instance();
    static int schemaVersion() { return 10; }
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
    bool fail(const QString &message, QString *error);

    static const char *kConnectionName;
    QString m_lastError;
    QString m_path;
    bool m_ready = false;
};

} // namespace kl

#endif // KYLINLIST_DATABASE_H
