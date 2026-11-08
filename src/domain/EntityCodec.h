#ifndef KYLINLIST_ENTITYCODEC_H
#define KYLINLIST_ENTITYCODEC_H

#include "Entity.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimeZone>
#include <cmath>
#include <limits>

namespace kl {
namespace entitycodec {

inline bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

inline bool validDate(const QDateTime &date)
{
    return !date.isValid() || (date.date().year() >= 1 && date.date().year() <= 9999);
}
inline bool validAllDayDate(const QDate &date)
{
    return !date.isValid() || (date.year() >= 1 && date.year() <= 9999);
}
inline bool validTimeZone(const QString &name)
{
    return !name.isEmpty() && QTimeZone(name.toUtf8()).isValid();
}

inline bool readInteger(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)
        || number < -9007199254740991.0 || number > 9007199254740991.0) return false;
    *out = static_cast<qint64>(number);
    return true;
}

inline QJsonValue dateJson(const QDateTime &date)
{
    return date.isValid() ? QJsonValue(static_cast<double>(utcMs(date))) : QJsonValue(QJsonValue::Null);
}
inline bool readDate(const QJsonValue &value, QDateTime *out, QString *error)
{
    if (value.isNull()) { *out = QDateTime(); return true; }
    qint64 ms = 0;
    if (!readInteger(value, std::numeric_limits<qint64>::min(), std::numeric_limits<qint64>::max(), &ms))
        return fail(error, QStringLiteral("时间必须为 UTC 毫秒整数或 null"));
    const QDateTime parsed = fromUtcMs(ms);
    if (!parsed.isValid() || !validDate(parsed)) return fail(error, QStringLiteral("备份时间超出有效范围"));
    *out = parsed;
    return true;
}
inline QJsonValue allDayJson(const QDate &date)
{
    return date.isValid() ? QJsonValue(date.toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
}
inline bool readAllDay(const QJsonValue &value, QDate *out, QString *error)
{
    if (value.isNull()) { *out = QDate(); return true; }
    if (!value.isString()) return fail(error, QStringLiteral("全天日期必须为 ISO 日期或 null"));
    const QString text = value.toString();
    const QDate date = QDate::fromString(text, Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != text)
        return fail(error, QStringLiteral("全天日期无效"));
    *out = date;
    return true;
}

inline bool validate(const Entity &task, QString *error)
{
    if (!task.id.isEmpty() && !isUuid(task.id)) return fail(error, QStringLiteral("任务 id 必须为 UUID"));
    if (entityKindName(task.kind).isEmpty()) return fail(error, QStringLiteral("实体 kind 非法"));
    const QString ids[] = {task.folderId, task.listId, task.sectionId, task.parentId,
                           task.recurrenceSeriesId};
    for (const QString &id : ids)
        if (!id.isEmpty() && !isUuid(id)) return fail(error, QStringLiteral("任务引用 id 必须为 UUID"));
    if (task.title.trimmed().isEmpty()) return fail(error, QStringLiteral("任务标题不能为空"));
    if (task.sortOrder < 0) return fail(error, QStringLiteral("实体排序必须为非负整数"));
    if (task.priority < 0 || task.priority > 3) return fail(error, QStringLiteral("优先级必须为 0 到 3"));
    if (taskStatusName(task.status).isEmpty()) return fail(error, QStringLiteral("任务状态非法"));
    if (task.completed != (task.status == TaskStatus::Completed))
        return fail(error, QStringLiteral("completed 与任务状态不一致"));
    if (task.completed != task.completedAt.isValid()
        || (task.status == TaskStatus::Abandoned) != task.abandonedAt.isValid())
        return fail(error, QStringLiteral("状态与完成/放弃时间不一致"));
    if (task.deleted != task.deletedAt.isValid()) return fail(error, QStringLiteral("回收站状态与时间不一致"));
    if (task.parentId == task.id && !task.id.isEmpty()) return fail(error, QStringLiteral("任务不能以自身为父任务"));
    if (task.kind == EntityKind::Note && !task.parentId.isEmpty()) return fail(error, QStringLiteral("笔记不能有父实体"));
    const QDateTime dates[] = {task.deletedAt, task.startAt, task.dueAt, task.createdAt, task.updatedAt,
                               task.completedAt, task.abandonedAt, task.reminderAt, task.notifiedAt, task.pinnedAt};
    for (const QDateTime &date : dates)
        if (!validDate(date)) return fail(error, QStringLiteral("任务时间超出有效范围"));
    if (!validAllDayDate(task.allDayDate) || !validTimeZone(task.timeZone))
        return fail(error, QStringLiteral("全天日期或 IANA 时区非法"));
    if (task.startAt.isValid() && task.dueAt.isValid() && task.startAt > task.dueAt)
        return fail(error, QStringLiteral("开始时间不能晚于截止时间"));
    const QString normalizedRule = recurrenceRRule(task.rrule.isEmpty() ? task.recurrence : task.rrule);
    if ((task.rrule.isEmpty() ? task.recurrence : task.rrule) != QStringLiteral("none")
        && !(task.rrule.isEmpty() ? task.recurrence : task.rrule).isEmpty() && normalizedRule.isEmpty())
        return fail(error, QStringLiteral("重复规则非法或包含未支持的扩展"));
    if (!normalizedRule.isEmpty()) {
        if (task.recurrenceSeriesId.isEmpty() || task.occurrenceKey.isEmpty())
            return fail(error, QStringLiteral("重复任务必须关联 series 和唯一 occurrence"));
        if (!task.dueAt.isValid() && !task.startAt.isValid() && !task.allDayDate.isValid())
            return fail(error, QStringLiteral("重复任务必须有计划时间或全天日期"));
    }
    for (const ChecklistItem &item : task.checklist) {
        if (!isUuid(item.id) || item.entityId != task.id || item.sortOrder < 0 || !validDate(item.createdAt)
            || !validDate(item.updatedAt)) return fail(error, QStringLiteral("检查清单项非法"));
    }
    for (const TaskReminder &reminder : task.reminders) {
        if (!isUuid(reminder.id) || reminder.entityId != task.id || !reminder.triggerAt.isValid()
            || !validDate(reminder.triggerAt) || !validDate(reminder.deliveredAt) || reminder.sortOrder < 0)
            return fail(error, QStringLiteral("提醒非法"));
    }
    if (error) error->clear();
    return true;
}

inline QJsonObject checklistJson(const ChecklistItem &item)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), item.id);
    object.insert(QStringLiteral("entityId"), item.entityId);
    object.insert(QStringLiteral("text"), item.text);
    object.insert(QStringLiteral("done"), item.done);
    object.insert(QStringLiteral("sortOrder"), item.sortOrder);
    object.insert(QStringLiteral("createdAtUtcMs"), dateJson(item.createdAt));
    object.insert(QStringLiteral("updatedAtUtcMs"), dateJson(item.updatedAt));
    return object;
}
inline QJsonObject reminderJson(const TaskReminder &reminder)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), reminder.id);
    object.insert(QStringLiteral("entityId"), reminder.entityId);
    object.insert(QStringLiteral("triggerAtUtcMs"), dateJson(reminder.triggerAt));
    object.insert(QStringLiteral("sortOrder"), reminder.sortOrder);
    object.insert(QStringLiteral("createdAtUtcMs"), dateJson(reminder.createdAt));
    object.insert(QStringLiteral("updatedAtUtcMs"), dateJson(reminder.updatedAt));
    return object;
}

inline QJsonObject toJson(const Entity &task)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), task.id);
    object.insert(QStringLiteral("kind"), entityKindName(task.kind));
    object.insert(QStringLiteral("title"), task.title);
    object.insert(QStringLiteral("bodyMarkdown"), task.body);
    object.insert(QStringLiteral("folderId"), task.folderId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.folderId));
    object.insert(QStringLiteral("listId"), task.listId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.listId));
    object.insert(QStringLiteral("sectionId"), task.sectionId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.sectionId));
    object.insert(QStringLiteral("parentId"), task.parentId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.parentId));
    object.insert(QStringLiteral("sortOrder"), static_cast<double>(task.sortOrder));
    object.insert(QStringLiteral("priority"), task.priority);
    object.insert(QStringLiteral("status"), taskStatusName(task.status));
    object.insert(QStringLiteral("pinnedAtUtcMs"), dateJson(task.pinnedAt));
    object.insert(QStringLiteral("deletedAtUtcMs"), dateJson(task.deletedAt));
    object.insert(QStringLiteral("startAtUtcMs"), dateJson(task.startAt));
    object.insert(QStringLiteral("dueAtUtcMs"), dateJson(task.dueAt));
    object.insert(QStringLiteral("allDayDate"), allDayJson(task.allDayDate));
    object.insert(QStringLiteral("timeZone"), task.timeZone);
    object.insert(QStringLiteral("recurrenceSeriesId"), task.recurrenceSeriesId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.recurrenceSeriesId));
    object.insert(QStringLiteral("occurrenceKey"), task.occurrenceKey.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.occurrenceKey));
    object.insert(QStringLiteral("rrule"), task.rrule.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(task.rrule));
    object.insert(QStringLiteral("createdAtUtcMs"), dateJson(task.createdAt));
    object.insert(QStringLiteral("updatedAtUtcMs"), dateJson(task.updatedAt));
    object.insert(QStringLiteral("completedAtUtcMs"), dateJson(task.completedAt));
    object.insert(QStringLiteral("abandonedAtUtcMs"), dateJson(task.abandonedAt));
    QJsonArray tags;
    for (const QString &tag : task.tags) tags.append(tag);
    object.insert(QStringLiteral("tags"), tags);
    QJsonArray checklist;
    for (const ChecklistItem &item : task.checklist) checklist.append(checklistJson(item));
    object.insert(QStringLiteral("checklist"), checklist);
    QJsonArray reminders;
    for (const TaskReminder &reminder : task.reminders) reminders.append(reminderJson(reminder));
    object.insert(QStringLiteral("reminders"), reminders);
    return object;
}

inline bool optionalId(const QJsonObject &object, const QString &key, QString *out)
{
    const QJsonValue value = object.value(key);
    if (value.isNull()) { out->clear(); return true; }
    if (!value.isString() || !isUuid(value.toString())) return false;
    *out = value.toString();
    return true;
}

inline bool readChecklist(const QJsonObject &object, ChecklistItem *out, QString *error)
{
    const QStringList keys = {QStringLiteral("id"), QStringLiteral("entityId"), QStringLiteral("text")};
    for (const QString &key : keys) if (!object.value(key).isString()) return fail(error, QStringLiteral("清单字段类型非法"));
    if (!isUuid(object.value(QStringLiteral("id")).toString()) || !isUuid(object.value(QStringLiteral("entityId")).toString())
        || !object.value(QStringLiteral("done")).isBool()) return fail(error, QStringLiteral("清单 id/done 非法"));
    qint64 order = 0;
    if (!readInteger(object.value(QStringLiteral("sortOrder")), 0, 1000000, &order))
        return fail(error, QStringLiteral("清单排序非法"));
    out->id = object.value(QStringLiteral("id")).toString();
    out->entityId = object.value(QStringLiteral("entityId")).toString();
    out->text = object.value(QStringLiteral("text")).toString();
    out->done = object.value(QStringLiteral("done")).toBool();
    out->sortOrder = static_cast<int>(order);
    return readDate(object.value(QStringLiteral("createdAtUtcMs")), &out->createdAt, error)
        && readDate(object.value(QStringLiteral("updatedAtUtcMs")), &out->updatedAt, error);
}
inline bool readReminder(const QJsonObject &object, TaskReminder *out, QString *error)
{
    if (!object.value(QStringLiteral("id")).isString() || !object.value(QStringLiteral("entityId")).isString()
        || !isUuid(object.value(QStringLiteral("id")).toString()) || !isUuid(object.value(QStringLiteral("entityId")).toString()))
        return fail(error, QStringLiteral("提醒 id 非法"));
    qint64 order = 0;
    if (!readInteger(object.value(QStringLiteral("sortOrder")), 0, 1000000, &order))
        return fail(error, QStringLiteral("提醒排序非法"));
    out->id = object.value(QStringLiteral("id")).toString();
    out->entityId = object.value(QStringLiteral("entityId")).toString();
    out->sortOrder = static_cast<int>(order);
    return readDate(object.value(QStringLiteral("triggerAtUtcMs")), &out->triggerAt, error)
        && readDate(object.value(QStringLiteral("createdAtUtcMs")), &out->createdAt, error)
        && readDate(object.value(QStringLiteral("updatedAtUtcMs")), &out->updatedAt, error)
        && out->triggerAt.isValid();
}

inline bool fromJson(const QJsonObject &object, Entity *out, QString *error)
{
    Entity task;
    if (!object.value(QStringLiteral("id")).isString() || !isUuid(object.value(QStringLiteral("id")).toString())
        || !object.value(QStringLiteral("kind")).isString() || !object.value(QStringLiteral("title")).isString()
        || !object.value(QStringLiteral("bodyMarkdown")).isString()
        || !object.value(QStringLiteral("status")).isString() || !object.value(QStringLiteral("timeZone")).isString())
        return fail(error, QStringLiteral("任务基本字段缺失或类型非法"));
    task.id = object.value(QStringLiteral("id")).toString();
    task.kind = entityKindFromName(object.value(QStringLiteral("kind")).toString());
    if (entityKindName(task.kind) != object.value(QStringLiteral("kind")).toString())
        return fail(error, QStringLiteral("实体 kind 非法"));
    task.title = object.value(QStringLiteral("title")).toString();
    task.body = object.value(QStringLiteral("bodyMarkdown")).toString();
    task.status = taskStatusFromName(object.value(QStringLiteral("status")).toString());
    if (taskStatusName(task.status) != object.value(QStringLiteral("status")).toString())
        return fail(error, QStringLiteral("任务状态非法"));
    task.completed = task.status == TaskStatus::Completed;
    if (!optionalId(object, QStringLiteral("folderId"), &task.folderId)
        || !optionalId(object, QStringLiteral("listId"), &task.listId)
        || !optionalId(object, QStringLiteral("sectionId"), &task.sectionId)
        || !optionalId(object, QStringLiteral("parentId"), &task.parentId)
        || !optionalId(object, QStringLiteral("recurrenceSeriesId"), &task.recurrenceSeriesId))
        return fail(error, QStringLiteral("任务引用 id 非法"));
    qint64 number = 0;
    if (!readInteger(object.value(QStringLiteral("sortOrder")), 0, std::numeric_limits<qint64>::max(), &task.sortOrder)
        || !readInteger(object.value(QStringLiteral("priority")), 0, 3, &number))
        return fail(error, QStringLiteral("实体排序或任务优先级非法"));
    task.priority = static_cast<int>(number);
    const struct { const char *key; QDateTime Entity::*field; } dates[] = {
        {"deletedAtUtcMs", &Entity::deletedAt}, {"startAtUtcMs", &Entity::startAt}, {"dueAtUtcMs", &Entity::dueAt},
        {"createdAtUtcMs", &Entity::createdAt}, {"updatedAtUtcMs", &Entity::updatedAt},
        {"completedAtUtcMs", &Entity::completedAt}, {"abandonedAtUtcMs", &Entity::abandonedAt},
        {"pinnedAtUtcMs", &Entity::pinnedAt}
    };
    for (const auto &entry : dates)
        if (!readDate(object.value(QLatin1String(entry.key)), &(task.*entry.field), error)) return false;
    task.deleted = task.deletedAt.isValid();
    if (!readAllDay(object.value(QStringLiteral("allDayDate")), &task.allDayDate, error)) return false;
    task.timeZone = object.value(QStringLiteral("timeZone")).toString();
    const QJsonValue occurrence = object.value(QStringLiteral("occurrenceKey"));
    if (!occurrence.isNull() && !occurrence.isString()) return fail(error, QStringLiteral("occurrenceKey 非法"));
    task.occurrenceKey = occurrence.toString();
    const QJsonValue rrule = object.value(QStringLiteral("rrule"));
    if (!rrule.isNull() && !rrule.isString()) return fail(error, QStringLiteral("rrule 非法"));
    task.rrule = rrule.toString();
    task.recurrence = recurrenceName(task.rrule);
    const QJsonValue tags = object.value(QStringLiteral("tags"));
    const QJsonValue checklist = object.value(QStringLiteral("checklist"));
    const QJsonValue reminders = object.value(QStringLiteral("reminders"));
    if (!tags.isArray() || !checklist.isArray() || !reminders.isArray())
        return fail(error, QStringLiteral("tags/checklist/reminders 必须为数组"));
    for (const QJsonValue &tag : tags.toArray()) {
        if (!tag.isString()) return fail(error, QStringLiteral("标签必须为字符串"));
        task.tags.append(tag.toString());
    }
    for (const QJsonValue &value : checklist.toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("清单项必须为对象"));
        ChecklistItem item;
        if (!readChecklist(value.toObject(), &item, error)) return false;
        task.checklist.append(item);
    }
    for (const QJsonValue &value : reminders.toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("提醒必须为对象"));
        TaskReminder reminder;
        if (!readReminder(value.toObject(), &reminder, error)) return false;
        task.reminders.append(reminder);
        if (!task.reminderAt.isValid() || reminder.triggerAt < task.reminderAt) task.reminderAt = reminder.triggerAt;
    }
    if (!validate(task, error)) return false;
    *out = task;
    return true;
}

} // namespace entitycodec
} // namespace kl

#endif // KYLINLIST_ENTITYCODEC_H
