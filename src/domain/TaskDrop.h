#ifndef KYLINLIST_TASKDROP_H
#define KYLINLIST_TASKDROP_H

#include <QDate>
#include <QString>

namespace kl {
namespace TaskDrop {

// 拖放目标分区的纯规则（无状态、无 IO），供 TaskStore 与 UI 共享：
// key 是时间视图的机器标识（与 TaskPanel 侧栏项 id、Settings 默认视图一致），
// 不根据中文标题解析。实现保持 inline，便于 Store、视图和测试共享同一规则。

// 是否为可承载日期语义的时间分区：已过期 / 今天 / 明天 / 最近7天。
inline bool isTimeSection(const QString &key)
{
    return key == QStringLiteral("overdue") || key == QStringLiteral("today")
        || key == QStringLiteral("tomorrow") || key == QStringLiteral("next7days");
}

// 分区到目标日期的固定映射：overdue=today-1（昨天）、today=today、
// tomorrow=today+1、next7days=today+2（后天，最近7天视图的默认落点）。
// key 非法或 today 无效时返回无效 QDate。
inline QDate dateForSection(const QString &key, const QDate &today)
{
    if (!today.isValid()) return QDate();
    if (key == QStringLiteral("overdue")) return today.addDays(-1);
    if (key == QStringLiteral("today")) return today;
    if (key == QStringLiteral("tomorrow")) return today.addDays(1);
    if (key == QStringLiteral("next7days")) return today.addDays(2);
    return QDate();
}

} // namespace TaskDrop
} // namespace kl

#endif // KYLINLIST_TASKDROP_H
