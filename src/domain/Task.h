#ifndef KYLINNOTE_TASK_H
#define KYLINNOTE_TASK_H

#include "../Common.h"

#include <QDateTime>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

namespace kn {

// 任务正文格式。数据库 content_format 列与该枚举的整数值一一对应，禁止重排。
enum class ContentFormat { Markdown = 0, Html = 1 };

struct ChecklistItem {
    QString text = QStringLiteral("");
    bool done = false;
};

struct TaskGroup {
    qint64 id = 0;
    QString name = QStringLiteral("");
};

// 待办核心领域对象。可选时间字段以“无效 QDateTime”表示未设置，
// 落库时为 NULL；不要把 invalid 当成需要报错的非法日期。
struct Task {
    qint64 id = 0;
    QString title = QStringLiteral("");
    QString description = QStringLiteral("");
    ContentFormat contentFormat = ContentFormat::Markdown;
    qint64 groupId = 0;                 // 0 = 未分组
    int priority = 0;                   // 0 普通 / 1 重要 / 2 紧急
    bool completed = false;
    bool favorite = false;
    bool pinned = false;
    bool deleted = false;               // 回收站（软删）
    QDateTime startAt;
    QDateTime dueAt;
    QDateTime reminderAt;
    QDateTime notifiedAt;               // 提醒去重标记：>= reminderAt 表示已通知
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime completedAt;
    QString recurrence = QStringLiteral("none"); // none / daily / weekly / monthly
    QStringList tags;
    QVector<ChecklistItem> checklist;
    NoteColor color = NoteColor::Yellow;
    QRect geometry;
    int zOrder = 0;
    NoteLayer layer = NoteLayer::Normal;
};

inline bool isValidRecurrence(const QString &recurrence)
{
    return recurrence == QStringLiteral("none")
        || recurrence == QStringLiteral("daily")
        || recurrence == QStringLiteral("weekly")
        || recurrence == QStringLiteral("monthly");
}

// 由“本次计划时间”推算下一次计划时间，而不是从当前真实时间推算，
// 这样逾期完成后不会跳周期。anchorDay 为月度重复的原始“日”(1..31)，
// 用于月末锚定：1/31 -> 2/28(或 2/29) -> 3/31，而不是一路停在 28 号。
// current 无效或 recurrence 为 none/非法时返回无效 QDateTime。
inline QDateTime nextRecurrence(const QDateTime &current, const QString &recurrence, int anchorDay)
{
    if (!current.isValid()) {
        return QDateTime();
    }
    if (recurrence == QStringLiteral("daily")) {
        return current.addDays(1);
    }
    if (recurrence == QStringLiteral("weekly")) {
        return current.addDays(7);
    }
    if (recurrence != QStringLiteral("monthly")) {
        return QDateTime();
    }

    // QDateTime::addMonths 会把月末钳制到目标月最后一天，并保留时分秒与时区规则。
    QDateTime next = current.addMonths(1);
    const int anchor = anchorDay > 0 ? anchorDay : current.date().day();
    QDate date = next.date();
    const int targetDay = qMin(anchor, date.daysInMonth());
    if (date.day() != targetDay) {
        date.setDate(date.year(), date.month(), targetDay);
        next.setDate(date);
    }
    return next;
}

} // namespace kn

#endif // KYLINNOTE_TASK_H
