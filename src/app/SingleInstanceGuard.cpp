#include "SingleInstanceGuard.h"

#include <QDir>
#include <QFileInfo>

namespace kl {

SingleInstanceGuard::SingleInstanceGuard(const QString &lockFilePath)
    : m_lock(lockFilePath),
      m_lockPath(lockFilePath)
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    release();
}

SingleInstanceGuard::Result SingleInstanceGuard::acquire(int staleLockMs)
{
    QDir dir = QFileInfo(m_lockPath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return Failed;
    }

    m_lock.setStaleLockTime(staleLockMs);
    if (m_lock.tryLock(0)) {
        m_locked = true;
        return Acquired;
    }

    switch (m_lock.error()) {
    case QLockFile::LockFailedError:
        return AlreadyRunning;
    default:
        return Failed;
    }
}

void SingleInstanceGuard::release()
{
    if (m_locked) {
        m_lock.unlock();
        m_locked = false;
    }
}

} // namespace kl
