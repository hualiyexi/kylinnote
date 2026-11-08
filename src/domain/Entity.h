#ifndef KYLINLIST_ENTITY_H
#define KYLINLIST_ENTITY_H

#include "../Common.h"

#include <QDate>
#include <QDateTime>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace kl {

enum class EntityKind { Task = 0, Note = 1 };
enum class TaskStatus { Active = 0, Completed = 1, Abandoned = 2 };

inline QString entityKindName(EntityKind kind)
{
    switch (kind) {
    case EntityKind::Task: return QStringLiteral("task");
    case EntityKind::Note: return QStringLiteral("note");
    }
    return QString();
}
inline EntityKind entityKindFromName(const QString &name)
{
    return name == QStringLiteral("note") ? EntityKind::Note : EntityKind::Task;
}

inline QString taskStatusName(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Active: return QStringLiteral("active");
    case TaskStatus::Completed: return QStringLiteral("completed");
    case TaskStatus::Abandoned: return QStringLiteral("abandoned");
    }
    return QString();
}
inline TaskStatus taskStatusFromName(const QString &name)
{
    if (name == QStringLiteral("completed")) return TaskStatus::Completed;
    if (name == QStringLiteral("abandoned")) return TaskStatus::Abandoned;
    return TaskStatus::Active;
}

struct Folder {
    QString id;
    QString name;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct TaskList {
    QString id;
    QString folderId;
    EntityKind kind = EntityKind::Task;
    QString name;
    bool inbox = false;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};
using TaskGroup = TaskList;

struct Section {
    QString id;
    QString listId;
    QString name;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct ChecklistItem {
    QString id;
    QString entityId;
    QString text;
    bool done = false;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct Tag {
    QString id;
    QString name;
    QString normalizedName;
    QString color;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};
struct TaskTag {
    QString id;
    QString entityId;
    QString tagId;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct TaskReminder {
    QString id;
    QString entityId;
    QDateTime triggerAt;
    QDateTime deliveredAt;          // 当前 installation 的瞬态门面；真实值在 reminder_deliveries
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct ReminderDelivery {
    QString reminderId;
    QString installationId;
    QDateTime scheduledAt;
    QDateTime deliveredAt;
};

struct RecurrenceSeries {
    QString id;
    QString rrule;
    int anchorDay = 0;
    QString timeZone = defaultTimeZone();
    QDateTime dtstartAt;
    QDate dtstartDate;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct Entity {
    QString id;
    EntityKind kind = EntityKind::Task;
    QString title;
    QString body;
    QString folderId;
    QString listId;
    QString sectionId;
    QString parentId;                // 真正的父子任务关系
    qint64 sortOrder = 0;
    int priority = 0;                // 0 无 / 1 低 / 2 中 / 3 高
    TaskStatus status = TaskStatus::Active;
    bool completed = false;          // UI 门面，始终对应 status == Completed
    bool deleted = false;            // 回收站门面；与业务状态完全分离
    QDateTime pinnedAt;
    QDateTime deletedAt;
    QDateTime startAt;               // 绝对时刻，数据库为 UTC ms
    QDateTime dueAt;
    QDate allDayDate;                // 全天仅保存日期，不把午夜伪造成截止时刻
    QString timeZone = defaultTimeZone();
    QString recurrenceSeriesId;
    QString occurrenceKey;
    QString rrule;                   // RFC 5545 风格 FREQ=DAILY/WEEKLY/MONTHLY
    QString recurrence = QStringLiteral("none"); // UI 门面：none/daily/weekly/monthly 或 RRULE
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime completedAt;
    QDateTime abandonedAt;

    QStringList tags;
    QVector<ChecklistItem> checklist;
    QVector<TaskReminder> reminders;

    // 旧单提醒 UI 门面。实际数据保存在 reminders/reminder_deliveries。
    QDateTime reminderAt;
    QDateTime notifiedAt;

    // 旧桌面便签 UI 门面。实际数据保存在当前 device_id 的 desktop_notes 行。
    NoteColor color = NoteColor::Yellow;
    QRect geometry;
    int zOrder = 0;
    NoteLayer layer = NoteLayer::Normal;
};

struct EntityEvent {
    QString id;
    QString entityId;               // 故意不作为数据库外键，永久删除后日志仍存在
    EntityKind entityKind = EntityKind::Task;
    QString entityTitle;
    QString batchId;
    QString type;
    QString field;
    QVariant oldValue;              // null 与空字符串必须区分
    QVariant newValue;
    QDateTime occurredAt;
};

inline int primaryReminderIndex(const Entity &task)
{
    int result = -1;
    // 优先最早的未投递提醒；全部已投递时仍保留最早一条供详情页显示。
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < task.reminders.size(); ++i) {
            const TaskReminder &reminder = task.reminders.at(i);
            if (!reminder.triggerAt.isValid() || (pass == 0 && reminder.deliveredAt.isValid())) continue;
            if (result < 0 || reminder.triggerAt < task.reminders.at(result).triggerAt) result = i;
        }
        if (result >= 0) break;
    }
    return result;
}

struct DesktopNote {
    QString id;
    QString entityId;
    QString deviceId;
    NoteColor color = NoteColor::Yellow;
    QRect geometry;
    int zOrder = 0;
    NoteLayer layer = NoteLayer::Normal;
    bool open = true;
    QDateTime createdAt;
    QDateTime updatedAt;
};

struct ViewDef {
    QString id;
    QString name;
    QString specJson;
    int sortOrder = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};
struct SyncBinding {
    QString id;
    QString entityType;
    QString entityId;
    QString remoteId;
    QDateTime updatedAt;
};
struct LocalMeta {
    QString key;
    QString value;
};
struct Tombstone {
    QString id;
    QString entityType;
    QString entityId;
    QDateTime deletedAt;
};
struct OutboxItem {
    QString id;
    QString batchId;
    qint64 baseServerRevision = 0;
    QString entityType;
    QString entityId;
    QString operation;
    QString payloadJson = QStringLiteral("{}");
    QDateTime createdAt;
};

inline bool parseRecurrence(const QString &rule, QString *frequency, int *interval)
{
    QString text = rule.trimmed();
    if (text.startsWith(QStringLiteral("RRULE:"), Qt::CaseInsensitive)) text = text.mid(6);
    if (text == QStringLiteral("daily") || text == QStringLiteral("weekly") || text == QStringLiteral("monthly")) {
        if (frequency) *frequency = text.toUpper();
        if (interval) *interval = 1;
        return true;
    }
    if (text.isEmpty() || text == QStringLiteral("none")) {
        if (frequency) frequency->clear();
        if (interval) *interval = 1;
        return true;
    }
    QString freq;
    int step = 1;
    const QStringList parts = text.split(QLatin1Char(';'), QString::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString &part : parts) {
        const int equals = part.indexOf(QLatin1Char('='));
        if (equals <= 0 || equals == part.size() - 1) return false;
        const QString key = part.left(equals).toUpper();
        const QString value = part.mid(equals + 1).toUpper();
        if (key == QStringLiteral("FREQ")) {
            if (!freq.isEmpty()) return false;
            freq = value;
        } else if (key == QStringLiteral("INTERVAL")) {
            bool ok = false;
            const int parsed = value.toInt(&ok);
            if (!ok || parsed < 1 || parsed > 999 || QString::number(parsed) != value) return false;
            step = parsed;
        } else {
            // 当前本地引擎明确拒绝未实现的 RRULE 扩展，避免静默错算。
            return false;
        }
    }
    if (freq != QStringLiteral("DAILY") && freq != QStringLiteral("WEEKLY")
        && freq != QStringLiteral("MONTHLY")) return false;
    if (frequency) *frequency = freq;
    if (interval) *interval = step;
    return true;
}

inline bool isValidRecurrence(const QString &rule)
{
    return parseRecurrence(rule, nullptr, nullptr);
}

inline QString recurrenceRRule(const QString &rule)
{
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rule, &frequency, &interval) || frequency.isEmpty()) return QString();
    return QStringLiteral("FREQ=%1;INTERVAL=%2").arg(frequency).arg(interval);
}
inline QString recurrenceName(const QString &rrule)
{
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rrule, &frequency, &interval) || frequency.isEmpty()) return QStringLiteral("none");
    if (interval == 1) return frequency.toLower();
    return recurrenceRRule(rrule);
}

inline QDateTime nextRecurrence(const QDateTime &current, const QString &rule, int anchorDay)
{
    if (!current.isValid()) return QDateTime();
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rule, &frequency, &interval) || frequency.isEmpty()) return QDateTime();
    if (frequency == QStringLiteral("DAILY")) return current.addDays(interval);
    if (frequency == QStringLiteral("WEEKLY")) return current.addDays(7 * interval);
    QDateTime next = current.addMonths(interval);
    const int anchor = anchorDay > 0 ? anchorDay : current.date().day();
    QDate date = next.date();
    const int targetDay = qMin(anchor, date.daysInMonth());
    if (date.day() != targetDay) {
        date.setDate(date.year(), date.month(), targetDay);
        next.setDate(date);
    }
    return next;
}

inline QDate nextRecurrenceDate(const QDate &current, const QString &rule, int anchorDay)
{
    if (!current.isValid()) return QDate();
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rule, &frequency, &interval) || frequency.isEmpty()) return QDate();
    if (frequency == QStringLiteral("DAILY")) return current.addDays(interval);
    if (frequency == QStringLiteral("WEEKLY")) return current.addDays(7 * interval);
    const QDate shifted = current.addMonths(interval);
    return QDate(shifted.year(), shifted.month(), qMin(anchorDay > 0 ? anchorDay : current.day(), shifted.daysInMonth()));
}

} // namespace kl

#endif // KYLINLIST_ENTITY_H
