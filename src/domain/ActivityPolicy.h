#ifndef KYLINLIST_ACTIVITYPOLICY_H
#define KYLINLIST_ACTIVITYPOLICY_H

#include "Entity.h"

namespace kl {

// 业务活动的单一白名单：写入端与时间线共用。历史查询可以保留旧原始事件，
// 但排序、置顶、section/文件夹等纯展示变化不再生成或展示为业务活动。
inline bool isVisibleActivityEvent(const EntityEvent &event)
{
    const QString &type = event.type;
    if (type == QStringLiteral("created") || type == QStringLiteral("trashed")
        || type == QStringLiteral("restored") || type == QStringLiteral("deleted")
        || type == QStringLiteral("kind_changed") || type == QStringLiteral("attachment_added")
        || type == QStringLiteral("attachment_removed") || type == QStringLiteral("reminder_added")
        || type == QStringLiteral("reminder_removed")) return true;
    if (type != QStringLiteral("changed")) return false;

    const QString &field = event.field;
    return field == QStringLiteral("title") || field == QStringLiteral("body")
        || field == QStringLiteral("status") || field == QStringLiteral("due")
        || field == QStringLiteral("start") || field == QStringLiteral("allDayDate")
        || field == QStringLiteral("allDayEndDate") || field == QStringLiteral("hasScheduleRange")
        || field == QStringLiteral("recurrenceRule")
        || field == QStringLiteral("priority") || field == QStringLiteral("list")
        || field == QStringLiteral("parent") || field == QStringLiteral("timeZone")
        || field == QStringLiteral("recurrence") || field == QStringLiteral("tags")
        || field == QStringLiteral("listArchived") || field == QStringLiteral("reminder")
        || field.startsWith(QStringLiteral("reminder.")) || field == QStringLiteral("attachment")
        || field.startsWith(QStringLiteral("attachment."));
}

} // namespace kl

#endif // KYLINLIST_ACTIVITYPOLICY_H
