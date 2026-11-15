#include "ViewSpec.h"

#include <algorithm>

namespace kl {
namespace {

bool timeScope(ViewScope scope)
{
    return scope == ViewScope::Today || scope == ViewScope::Tomorrow
        || scope == ViewScope::Next7Days || scope == ViewScope::Overdue;
}
bool archiveScope(ViewScope scope)
{
    return scope == ViewScope::Completed || scope == ViewScope::Abandoned || scope == ViewScope::Trash;
}
bool searchMatches(const Entity &entity, const QString &search)
{
    const QString simplified = search.simplified();
    if (simplified.isEmpty()) return true;
    QString haystack = entity.title + QLatin1Char('\n') + entity.body + QLatin1Char('\n') + entity.tags.join(QLatin1Char(' '));
    for (const ChecklistItem &item : entity.checklist) haystack += QLatin1Char('\n') + item.text;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const auto skipEmpty = Qt::SkipEmptyParts;
#else
    const auto skipEmpty = QString::SkipEmptyParts;
#endif
    const QStringList terms = simplified.split(QLatin1Char(' '), skipEmpty);
    for (const QString &term : terms) if (!haystack.contains(term, Qt::CaseInsensitive)) return false;
    return true;
}
void sortPinned(QVector<Entity> *items)
{
    std::sort(items->begin(), items->end(), [](const Entity &a, const Entity &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        if (a.pinnedAt != b.pinnedAt) return a.pinnedAt > b.pinnedAt;
        return a.id < b.id;
    });
}
void sortByDate(QVector<Entity> *items)
{
    std::sort(items->begin(), items->end(), [](const Entity &a, const Entity &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        const QDate ad = effectiveDate(a), bd = effectiveDate(b);
        if (ad != bd) return ad < bd;
        return a.id < b.id;
    });
}
void sortByCreated(QVector<Entity> *items)
{
    std::sort(items->begin(), items->end(), [](const Entity &a, const Entity &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        if (a.createdAt != b.createdAt) return a.createdAt > b.createdAt;
        return a.id < b.id;
    });
}
QDateTime archiveTime(const Entity &entity, ViewScope scope)
{
    if (scope == ViewScope::Completed) return entity.completedAt;
    if (scope == ViewScope::Abandoned) return entity.abandonedAt;
    return entity.deletedAt;
}
QString archiveTitle(ViewScope scope)
{
    if (scope == ViewScope::Completed) return QStringLiteral("已完成");
    if (scope == ViewScope::Abandoned) return QStringLiteral("已放弃");
    return QStringLiteral("回收站");
}
QString archiveKey(ViewScope scope)
{
    if (scope == ViewScope::Completed) return QStringLiteral("completed");
    if (scope == ViewScope::Abandoned) return QStringLiteral("abandoned");
    return QStringLiteral("trash");
}
void appendSection(QVector<ViewSection> *sections, const QString &key, const QString &title, const QVector<Entity> &items)
{
    if (!items.isEmpty()) sections->append(ViewSection{key, title, items});
}

} // namespace

QDate effectiveDate(const Entity &entity)
{
    if (entity.dueAt.isValid()) return toAppTime(entity.dueAt).date();
    if (entity.allDayDate.isValid()) return entity.allDayDate;
    return toAppTime(entity.startAt).date();
}

bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today, const QSet<QString> &archivedListIds,
              const QSet<QString> &folderListIds)
{
    if (!today.isValid()) return false;
    if (spec.renderer == ViewRenderer::Time && !timeScope(spec.scope)) return false;
    if (spec.scope == ViewScope::Trash) {
        if (!entity.deleted) return false;
    } else {
        if (entity.deleted) return false;
        if (spec.scope == ViewScope::Completed)
            return entity.kind == EntityKind::Task && entity.status == TaskStatus::Completed && searchMatches(entity, spec.search);
        if (spec.scope == ViewScope::Abandoned)
            return entity.kind == EntityKind::Task && entity.status == TaskStatus::Abandoned && searchMatches(entity, spec.search);
        // 活动视图（AllLists/Today/Tomorrow/Next7Days/Overdue）排除归档清单的活动实体；
        // 显式 List 作用域允许查看归档清单，Completed/Abandoned/Trash 保留历史可见性。
        if (spec.scope != ViewScope::List && spec.scope != ViewScope::Folder
            && !archivedListIds.isEmpty() && archivedListIds.contains(entity.listId)) return false;
        if (entity.kind == EntityKind::Note) {
            // 笔记保留的 status 不影响普通清单可见性，也永不进入任务时间/归档视图。
            if (spec.scope != ViewScope::AllLists && spec.scope != ViewScope::List
                && spec.scope != ViewScope::Folder) return false;
        } else if (entity.status != TaskStatus::Active) {
            const bool visibleCompleted = spec.showCompleted
                && entity.status == TaskStatus::Completed
                && (spec.scope == ViewScope::AllLists || spec.scope == ViewScope::List
                    || spec.scope == ViewScope::Folder);
            if (!visibleCompleted) return false;
        }
    }
    if (spec.scope == ViewScope::List && entity.listId != spec.listId) return false;
    if (spec.scope == ViewScope::Folder && !folderListIds.contains(entity.listId)) return false;
    if (timeScope(spec.scope)) {
        if (entity.kind != EntityKind::Task) return false;
        const QDate date = effectiveDate(entity);
        if (!date.isValid()) return false;
        if (spec.scope == ViewScope::Today && date > today) return false;
        if (spec.scope == ViewScope::Tomorrow && date != today.addDays(1)) return false;
        // 最近 7 天包含全部逾期，以及今天起的 7 个日历日（today..today+6）。
        if (spec.scope == ViewScope::Next7Days && date > today.addDays(6)) return false;
        if (spec.scope == ViewScope::Overdue && date >= today) return false;
    }
    return searchMatches(entity, spec.search);
}

QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today,
                                   const QSet<QString> &archivedListIds)
{
    QVector<ViewSection> sections;
    QVector<Entity> filtered;
    for (const Entity &entity : entities) if (contains(entity, spec, today, archivedListIds)) filtered.append(entity);
    if (archiveScope(spec.scope)) {
        std::sort(filtered.begin(), filtered.end(), [&spec](const Entity &a, const Entity &b) {
            const QDateTime ad = archiveTime(a, spec.scope), bd = archiveTime(b, spec.scope);
            if (ad != bd) {
                if (ad.isValid() != bd.isValid()) return ad.isValid();
                return ad > bd;
            }
            return a.id < b.id;
        });
        appendSection(&sections, archiveKey(spec.scope), archiveTitle(spec.scope), filtered);
        return sections;
    }

    QVector<Entity> pinned, overdue, upcoming, undated, notes, completed;
    for (const Entity &entity : filtered) {
        if (entity.kind == EntityKind::Task && entity.status == TaskStatus::Completed)
            completed.append(entity);
        else if (entity.pinnedAt.isValid()) pinned.append(entity);
        else if (entity.kind == EntityKind::Note) notes.append(entity);
        else {
            const QDate date = effectiveDate(entity);
            if (!date.isValid()) undated.append(entity);
            else if (date < today) overdue.append(entity);
            else upcoming.append(entity);
        }
    }
    sortPinned(&pinned); sortByDate(&overdue); sortByDate(&upcoming); sortByCreated(&undated);
    sortByCreated(&notes); sortByCreated(&completed);
    appendSection(&sections, QStringLiteral("pinned"), QStringLiteral("置顶"), pinned);
    appendSection(&sections, QStringLiteral("overdue"), QStringLiteral("已过期"), overdue);
    appendSection(&sections, QStringLiteral("upcoming"), QStringLiteral("未过期"), upcoming);
    if (spec.renderer == ViewRenderer::List) {
        appendSection(&sections, QStringLiteral("undated"), QStringLiteral("无日期"), undated);
        appendSection(&sections, QStringLiteral("notes"), QStringLiteral("笔记"), notes);
        appendSection(&sections, QStringLiteral("completed"), QStringLiteral("已完成"), completed);
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
    QVector<ViewSection> result;
    if (spec.scope != ViewScope::Folder || spec.folderId.isEmpty()) return result;
    const QSet<QString> members = folderListIds(lists, spec.folderId);
    QVector<Entity> pinned;
    for (const Entity &entity : entities)
        if (entity.pinnedAt.isValid() && contains(entity, spec, today, archivedListIds, members))
            pinned.append(entity);
    sortPinned(&pinned);
    appendSection(&result, QStringLiteral("pinned"), QStringLiteral("已置顶"), pinned);

    for (const TaskList &list : lists) {
        if (list.folderId != spec.folderId || list.archivedAtUtcMs > 0) continue;
        ViewSpec listSpec = spec;
        listSpec.scope = ViewScope::List;
        listSpec.listId = list.id;
        listSpec.folderId.clear();
        listSpec.renderer = ViewRenderer::List;
        const QVector<ViewSection> sections = buildSections(entities, listSpec, today, archivedListIds);
        QVector<Entity> items;
        for (const ViewSection &section : sections) {
            if (section.key == QLatin1String("pinned")) continue;
            items.append(section.items);
        }
        appendSection(&result, QStringLiteral("list:") + list.id, list.name, items);
    }
    return result;
}

QString scheduleLabel(const Entity &entity)
{
    const auto format = [](const QDateTime &value) { return toAppTime(value).toString(QStringLiteral("M月d日 HH:mm")); };
    if (entity.startAt.isValid() && entity.dueAt.isValid()) return format(entity.startAt) + QStringLiteral(" — ") + format(entity.dueAt);
    if (entity.dueAt.isValid()) return QStringLiteral("截止 ") + format(entity.dueAt);
    if (entity.allDayDate.isValid()) return entity.allDayDate.toString(QStringLiteral("M月d日"));
    if (entity.startAt.isValid()) return QStringLiteral("开始 ") + format(entity.startAt);
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
    return QStringLiteral("不重复");
}

} // namespace kl
