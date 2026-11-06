#ifndef KYLINNOTE_DATABASE_H
#define KYLINNOTE_DATABASE_H

#include <QSqlDatabase>
#include <QString>

namespace kn {

// 主线程专用的 SQLite 连接。只有 pragma 与迁移全部成功后才视为打开。
class Database
{
public:
    static Database &instance();

    bool open(QString *error = nullptr);
    // 显式路径用于测试/受控工具；不改变 Common 中默认数据路径的规则。
    bool openAt(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QSqlDatabase connection() const;
    QString lastError() const { return m_lastError; }

private:
    Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool applyPragmas(QString *error);
    bool migrate(int from, int to, QString *error);
    bool fail(const QString &message, QString *error);

    static const char *kConnectionName;
    QString m_lastError;
    QString m_path;
    bool m_ready = false;
};

} // namespace kn

#endif // KYLINNOTE_DATABASE_H
