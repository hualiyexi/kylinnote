#ifndef KYLINNOTE_DATABASE_H
#define KYLINNOTE_DATABASE_H

#include <QSqlDatabase>
#include <QString>

namespace kn {

// SQLite 连接管理：单连接 + WAL 模式，全部操作在主线程完成
class Database
{
public:
    static Database &instance();

    bool open(QString *error = nullptr);
    void close();
    bool isOpen() const;

    // 主线程专用的连接；QSqlDatabase 连接不可跨线程共享
    QSqlDatabase connection() const;
    QString lastError() const { return m_lastError; }

private:
    Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool applyPragmas(QString *error);
    bool migrate(int from, int to, QString *error);

    static const char *kConnectionName;
    QString m_lastError;
};

} // namespace kn

#endif // KYLINNOTE_DATABASE_H
