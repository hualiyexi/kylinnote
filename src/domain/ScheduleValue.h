#pragma once

#include "Entity.h"
#include "RecurrenceRule.h"

namespace kl {

// 日期弹窗与 Store 之间的完整调度值；正文、标签、优先级等不在此结构中。
struct ScheduleValue {
    QDateTime startAt;               // UTC；范围开始时必填
    QDateTime dueAt;                 // UTC；单点或范围结束
    QDate allDayDate;                // 单日或包含开始日
    QDate allDayEndDate;             // 全天范围包含结束日
    bool hasScheduleRange = false;
    QString timeZone = defaultTimeZone();
    QVector<TaskReminder> reminders; // 保留 UUID；ruleJson 为相对锚点的权威配置
    bool repeating = false;
    RecurrenceSpec recurrence;
};

inline ScheduleValue scheduleFromEntity(const Entity& entity) {
    ScheduleValue value;
    value.startAt = entity.startAt;
    value.dueAt = entity.dueAt;
    value.allDayDate = entity.allDayDate;
    value.allDayEndDate = entity.allDayEndDate;
    value.hasScheduleRange = entity.hasScheduleRange;
    value.timeZone = entity.timeZone;
    value.reminders = entity.reminders;
    return value;
}

inline void applyScheduleToEntity(Entity* entity, const ScheduleValue& value) {
    if (!entity) return;
    entity->startAt = value.startAt;
    entity->dueAt = value.dueAt;
    entity->allDayDate = value.allDayDate;
    entity->allDayEndDate = value.allDayEndDate;
    entity->hasScheduleRange = value.hasScheduleRange;
    entity->timeZone = value.timeZone;
    entity->reminders = value.reminders;
}

inline bool sameScheduleBaseline(const ScheduleValue& a, const ScheduleValue& b) {
    if (a.startAt != b.startAt || a.dueAt != b.dueAt || a.allDayDate != b.allDayDate
        || a.allDayEndDate != b.allDayEndDate || a.hasScheduleRange != b.hasScheduleRange
        || a.timeZone != b.timeZone || a.repeating != b.repeating
        || a.reminders.size() != b.reminders.size()) return false;
    if (a.repeating && recurrenceRuleText(a.recurrence) != recurrenceRuleText(b.recurrence)) return false;
    for (int i = 0; i < a.reminders.size(); ++i) {
        const TaskReminder& left = a.reminders.at(i);
        const TaskReminder& right = b.reminders.at(i);
        if (left.id != right.id || left.ruleJson != right.ruleJson
            || left.triggerAt != right.triggerAt || left.sortOrder != right.sortOrder) return false;
    }
    return true;
}

} // namespace kl
