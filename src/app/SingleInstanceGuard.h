#ifndef KYLINLIST_SINGLEINSTANCEGUARD_H
#define KYLINLIST_SINGLEINSTANCEGUARD_H

#include <QLockFile>
#include <QString>

namespace kl {

// 进程级独占锁，与 D-Bus 配合使用；锁文件过期时间用于处理崩溃残留
class SingleInstanceGuard
{
public:
    enum Result {
        Acquired,       // 本进程获得锁，可继续启动
        AlreadyRunning, // 锁被其他实例持有
        Failed          // 无法创建/访问锁文件
    };

    explicit SingleInstanceGuard(const QString &lockFilePath);
    ~SingleInstanceGuard();

    Result acquire(int staleLockMs = 30000);
    void release();
    bool isLocked() const { return m_locked; }

private:
    QLockFile m_lock;
    QString m_lockPath;   // QLockFile 不提供路径访问，自行保存
    bool m_locked = false;
};

} // namespace kl

#endif // KYLINLIST_SINGLEINSTANCEGUARD_H
