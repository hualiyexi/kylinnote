#include "ViewSpec.h"
#include "TaskGrouping.h"

#include <QTimeZone>
#include <algorithm>
#include <functional>

namespace kl {
namespace {

bool timeScope(ViewScope scope)
{
    return scope == ViewScope::Today || scope == ViewScope::Tomorrow
        || scope == ViewScope::Next7Days || scope == ViewScope::Overdue;
}

bool searchMatches(const Entity &entity, const QString &search)
{
    const QString simplified = search.simplified();
    if (simplified.isEmpty()) return true;
    QString haystack = entity.title + QLatin1Char('\n') + entity.body + QLatin1Char('\n') + entity.tags.join(QLatin1Char(' '));
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const auto skipEmpty = Qt::SkipEmptyParts;
#else
    const auto skipEmpty = QString::SkipEmptyParts;
#endif
    const QStringList terms = simplified.split(QLatin1Char(' '), skipEmpty);
    for (const QString &term : terms) if (!haystack.contains(term, Qt::CaseInsensitive)) return false;
    return true;
}

// 把分组结果按父子层级展平成兼容 ViewSection（items 含子级，按显示顺序）。
QVector<Entity> flattenSectionItems(const QVector<Entity> &roots,
                                    const QHash<QString, QVector<Entity>> &children)
{
    QVector<Entity> rows;
    QSet<QString> placed;
    std::function<void(const Entity &)> append = [&](const Entity &entity) {
        if (placed.contains(entity.id)) return;
        placed.insert(entity.id);
        rows.append(entity);
        for (const Entity &child : children.value(entity.id)) append(child);
    };
    for (const Entity &root : roots) append(root);
    return rows;
}

} // namespace

QDate scheduleStartDate(const Entity &entity)
{
    if (entity.allDayDate.isValid()) return entity.allDayDate;
    const QDateTime value = entity.hasScheduleRange ? entity.startAt : entity.dueAt;
    return value.isValid() ? value.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).date() : QDate();
}
QDate scheduleEndDate(const Entity &entity)
{
    if (entity.allDayDate.isValid())
        return entity.hasScheduleRange ? entity.allDayEndDate : entity.allDayDate;
    return entity.dueAt.isValid() ? entity.dueAt.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).date() : QDate();
}
QDate effectiveDate(const Entity &entity)
{
    return scheduleEndDate(entity);
}
bool scheduleIntersects(const Entity &entity, const QDate &first, const QDate &last)
{
    const QDate start = scheduleStartDate(entity), end = scheduleEndDate(entity);
    return first.isValid() && last.isValid() && first <= last
        && start.isValid() && end.isValid() && start <= last && end >= first;
}

bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today, const QSet<QString> &archivedListIds,
              const QSet<QString> &folderListIds)
{
    if (!today.isValid()) return false;
    if (spec.tasksOnly && entity.kind != EntityKind::Task) return false;
    // 注意：renderer 不再影响筛选资格（分组与筛选正交；renderer 仅保留为旧展示标识）。
    if (spec.scope == ViewScope::Trash) {
        if (!entity.deleted) return false;
    } else {
        if (entity.deleted) return false;
        if (spec.scope == ViewScope::Completed)
            return entity.kind == EntityKind::Task && entity.status == TaskStatus::Completed && searchMatches(entity, spec.search);
        if (spec.scope == ViewScope::Abandoned)
            return entity.kind == EntityKind::Task && entity.status == TaskStatus::Abandoned && searchMatches(entity, spec.search);
        // 活动聚合（含 Folder）排除归档清单；仅显式 List 与历史作用域保留可见性。
        if (spec.scope != ViewScope::List && archivedListIds.contains(entity.listId)) return false;
        if (entity.kind == EntityKind::Note) {
            // 笔记保留的 status 不影响普通清单可见性，也永不进入任务时间/归档视图。
            if (spec.scope != ViewScope::AllLists && spec.scope != ViewScope::List
                && spec.scope != ViewScope::Folder) return false;
        } else if (entity.status != TaskStatus::Active) {
            // showCompleted 同时放行 Completed 与 Abandoned 两组终态。
            const bool visibleTerminal = spec.showCompleted
                && (entity.status == TaskStatus::Completed || entity.status == TaskStatus::Abandoned)
                && (spec.scope == ViewScope::AllLists || spec.scope == ViewScope::List
                    || spec.scope == ViewScope::Folder);
            if (!visibleTerminal) return false;
        }
    }
    if (spec.scope == ViewScope::List && entity.listId != spec.listId) return false;
    if (spec.scope == ViewScope::Folder && !folderListIds.contains(entity.listId)) return false;
    if (timeScope(spec.scope)) {
        if (entity.kind != EntityKind::Task) return false;
        const QDate end = effectiveDate(entity);
        if (!end.isValid()) return false;
        if (spec.scope == ViewScope::Today && end > today && !scheduleIntersects(entity, today, today)) return false;
        if (spec.scope == ViewScope::Tomorrow && !scheduleIntersects(entity, today.addDays(1), today.addDays(1))) return false;
        // 最近 7 天包含全部逾期，以及与 today..today+6 相交的单日或时间段。
        if (spec.scope == ViewScope::Next7Days && end >= today
            && !scheduleIntersects(entity, today, today.addDays(6))) return false;
        if (spec.scope == ViewScope::Overdue && end >= today) return false;
    }
    return searchMatches(entity, spec.search);
}

QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today,
                                   const QSet<QString> &archivedListIds)
{
    // 兼容包装：无清单表可用（List 组标题回退），分组/继承/排序全部由 groupEntities 决定。
    const Grouping grouping = groupEntities(entities, QVector<TaskList>(), spec, today, archivedListIds);
    QVector<ViewSection> sections;
    for (const GroupedSection &section : grouping.sections) {
        ViewSection view;
        view.key = section.group.stableId();
        view.title = section.title;
        view.items = flattenSectionItems(section.items, grouping.children);
        if (!view.items.isEmpty()) sections.append(view);
    }
    return sections;
}

QSet<QString> folderListIds(const QVector<TaskList> &lists, const QString &folderId)
{
    QSet<QString> result;
    for (const TaskList &list : lists)
        if (list.folderId == folderId) result.insert(list.id);
    return result;
}

QVector<ViewSection> buildFolderSections(const QVector<Entity> &entities, const QVector<TaskList> &lists,
                                         const ViewSpec &spec, const QDate &today,
                                         const QSet<QString> &archivedListIds)
{
    const Grouping grouping = groupEntities(entities, lists, spec, today, archivedListIds);
    QVector<ViewSection> sections;
    for (const GroupedSection &section : grouping.sections) {
        ViewSection view;
        view.key = section.group.stableId();
        view.title = section.title;
        view.items = flattenSectionItems(section.items, grouping.children);
        if (!view.items.isEmpty()) sections.append(view);
    }
    return sections;
}

DetailScheduleCaption detailScheduleCaption(const Entity &entity, const QDateTime &now,
                                            Qt::DayOfWeek firstDay)
{
    const QDate first = scheduleStartDate(entity), last = scheduleEndDate(entity);
    if (!first.isValid() || !last.isValid()) return {QStringLiteral("设置日期"), false, false};

    const QDate today = toAppTime(now).date();
    const auto dateText = [&today](const QDate &date) {
        return date.toString(date.year() == today.year() ? QStringLiteral("M月d日")
                                                       : QStringLiteral("yyyy/MM/dd"));
    };
    const QTimeZone zone(entity.timeZone.toUtf8());
    const auto timeText = [&zone](const QDateTime &dateTime) {
        return dateTime.isValid() ? QStringLiteral(", ") + dateTime.toTimeZone(zone).toString(QStringLiteral("HH:mm"))
                                  : QString();
    };
    const QString separator = QStringLiteral(" - ");
    const bool timed = !entity.allDayDate.isValid();
    QString text = dateText(first);
    if (timed) text += timeText(entity.hasScheduleRange ? entity.startAt : entity.dueAt);
    if (entity.hasScheduleRange) {
        if (first != last) {
            text += separator + dateText(last);
            if (timed) text += timeText(entity.dueAt);
        } else if (timed) {
            text += separator + entity.dueAt.toTimeZone(zone).toString(QStringLiteral("HH:mm"));
        }
    }

    if (!today.isValid()) return {text, true, false};

    // 整个时间段的相对说明以结束日为准，不能把已开始但尚未结束的任务标成逾期。
    const qint64 days = today.daysTo(last);
    const bool active = !entity.deleted
        && (entity.kind == EntityKind::Note || entity.status == TaskStatus::Active);
    const bool showDayCount = active && entity.kind == EntityKind::Task;
    const bool overdue = active && (days < 0 || (timed && entity.dueAt < now));
    QString suffix;
    if (days == 0) suffix = QStringLiteral("今天");
    else if (days == 1) suffix = QStringLiteral("明天");
    else if (days == 2) suffix = QStringLiteral("后天");
    else if (days == -1) suffix = QStringLiteral("昨天");
    else if (days < -1 && showDayCount) suffix = QStringLiteral("延期%1天").arg(-days);
    else if (days > 2) {
        const int daysIntoWeek = (today.dayOfWeek() - firstDay + 7) % 7;
        const int daysUntilWeekEnd = 6 - daysIntoWeek;
        if (days <= daysUntilWeekEnd + 7) {
            suffix = days <= daysUntilWeekEnd ? QStringLiteral("周") : QStringLiteral("下周");
            suffix += QStringLiteral("一二三四五六日").at(last.dayOfWeek() - 1);
        } else if (showDayCount) suffix = QStringLiteral("剩余%1天").arg(days);
    }
    if (!suffix.isEmpty()) text += QStringLiteral(", ") + suffix;
    return {text, true, overdue};
}

QString scheduleLabel(const Entity &entity)
{
    const QTimeZone zone(entity.timeZone.toUtf8());
    const auto format = [&zone](const QDateTime &value) {
        return value.toTimeZone(zone).toString(QStringLiteral("M月d日 HH:mm"));
    };
    if (entity.hasScheduleRange && entity.allDayDate.isValid())
        return entity.allDayDate.toString(QStringLiteral("M月d日")) + QStringLiteral(" — ")
            + entity.allDayEndDate.toString(QStringLiteral("M月d日"));
    if (entity.hasScheduleRange && entity.startAt.isValid() && entity.dueAt.isValid())
        return format(entity.startAt) + QStringLiteral(" — ") + format(entity.dueAt);
    if (entity.dueAt.isValid()) return QStringLiteral("截止 ") + format(entity.dueAt);
    if (entity.allDayDate.isValid()) return entity.allDayDate.toString(QStringLiteral("M月d日"));
    return QStringLiteral("未安排时间");
}
QString priorityLabel(int priority)
{
    if (priority == 3) return QStringLiteral("高");
    if (priority == 2) return QStringLiteral("中");
    if (priority == 1) return QStringLiteral("低");
    return QStringLiteral("无");
}
QString recurrenceLabel(const QString &recurrence)
{
    if (recurrence == QLatin1String("daily")) return QStringLiteral("每天");
    if (recurrence == QLatin1String("weekly")) return QStringLiteral("每周");
    if (recurrence == QLatin1String("monthly")) return QStringLiteral("每月");
    if (recurrence.isEmpty() || recurrence == QLatin1String("none")) return QStringLiteral("不重复");
    return recurrence;
}

} // namespace kl
