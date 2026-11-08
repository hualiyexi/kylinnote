#ifndef KYLINLIST_INTERNALLINK_H
#define KYLINLIST_INTERNALLINK_H

#include <QString>
#include <QStringList>

namespace kl {

// 接受裸 UUID 或 kl:UUID；其他 scheme、非小写规范 UUID 均不是有效内链目标。
bool isValidLinkTarget(const QString &target);
QStringList internalTargets(const QString &body);

} // namespace kl

#endif // KYLINLIST_INTERNALLINK_H
