#include "TaskFilter.h"
#include <QLocale>
namespace kn {
bool TaskFilter::matches(const Task& task, const QDateTime& now) const {
    if ((view == TaskView::Trash) != task.deleted)
        return false;
    if (view != TaskView::Trash && (view == TaskView::Completed) != task.completed)
        return false;
    const QDate today = now.toLocalTime().date();
    const QDate start = task.startAt.toLocalTime().date(), due = task.dueAt.toLocalTime().date();
    switch (view) {
        case TaskView::Today:
            if (!(due == today || start == today ||
                  (start.isValid() && due.isValid() && start <= today && due >= today)))
                return false;
            break;
        case TaskView::Upcoming: {
            const QDate date = due.isValid() ? due : start;
            if (!date.isValid() || date < today || date > today.addDays(7))
                return false;
            break;
        }
        case TaskView::Overdue:
            if (!task.dueAt.isValid() || task.dueAt >= now)
                return false;
            break;
        case TaskView::Important:
            if (!task.favorite && task.priority == 0)
                return false;
            break;
        case TaskView::Pinned:
            if (!task.pinned)
                return false;
            break;
        case TaskView::Group:
            if (task.groupId != groupId)
                return false;
            break;
        default:
            break;
    }
    if (search.trimmed().isEmpty())
        return true;
    QString haystack = task.title + QLatin1Char('\n') + task.description + QLatin1Char('\n') +
                       task.tags.join(QLatin1Char(' '));
    for (const auto& item : task.checklist)
        haystack += QLatin1Char('\n') + item.text;
    for (const QString& term : search.split(QLatin1Char(' '), QString::SkipEmptyParts))
        if (!haystack.contains(term, Qt::CaseInsensitive))
            return false;
    return true;
}
bool TaskFilter::lessThan(const Task& a, const Task& b) const {
    if (sort == TaskSort::Title) {
        const int c = QString::localeAwareCompare(a.title, b.title);
        if (c)
            return c < 0;
    }
    if (sort == TaskSort::Priority && a.priority != b.priority)
        return a.priority > b.priority;
    if (sort == TaskSort::Created && a.createdAt != b.createdAt)
        return a.createdAt > b.createdAt;
    if (sort == TaskSort::Schedule) {
        const QDateTime ad = a.dueAt.isValid() ? a.dueAt : a.startAt,
                        bd = b.dueAt.isValid() ? b.dueAt : b.startAt;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        if (ad != bd)
            return ad < bd;
        if (a.priority != b.priority)
            return a.priority > b.priority;
    }
    return a.id > b.id;
}
QString scheduleLabel(const Task& task) {
    const auto fmt = [](const QDateTime& value) {
        return value.toLocalTime().toString(QStringLiteral("M月d日 HH:mm"));
    };
    if (task.startAt.isValid() && task.dueAt.isValid())
        return fmt(task.startAt) + QStringLiteral(" — ") + fmt(task.dueAt);
    if (task.dueAt.isValid())
        return QStringLiteral("截止 ") + fmt(task.dueAt);
    if (task.startAt.isValid())
        return QStringLiteral("开始 ") + fmt(task.startAt);
    return QStringLiteral("未安排时间");
}
QString priorityLabel(int priority) {
    if (priority == 2)
        return QStringLiteral("紧急");
    if (priority == 1)
        return QStringLiteral("重要");
    return QStringLiteral("普通");
}
QString recurrenceLabel(const QString& recurrence) {
    if (recurrence == QLatin1String("daily"))
        return QStringLiteral("每天");
    if (recurrence == QLatin1String("weekly"))
        return QStringLiteral("每周");
    if (recurrence == QLatin1String("monthly"))
        return QStringLiteral("每月");
    return QStringLiteral("不重复");
}
}  // namespace kn
