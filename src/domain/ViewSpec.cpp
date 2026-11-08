#include "ViewSpec.h"

#include <algorithm>

namespace kl {
namespace {

bool timeScope(ViewScope scope)
{
    return scope == ViewScope::Today || scope == ViewScope::Next7Days || scope == ViewScope::Overdue;
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
        if (a.pinnedAt != b.pinnedAt) return a.pinnedAt > b.pinnedAt;
        return a.id < b.id;
    });
}
void sortByDate(QVector<Entity> *items)
{
    std::sort(items->begin(), items->end(), [](const Entity &a, const Entity &b) {
        const QDate ad = effectiveDate(a), bd = effectiveDate(b);
        if (ad != bd) return ad < bd;
        return a.id < b.id;
    });
}
void sortByCreated(QVector<Entity> *items)
{
    std::sort(items->begin(), items->end(), [](const Entity &a, const Entity &b) {
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
void appendSection(QVector<ViewSection> *sections, const QString &title, const QVector<Entity> &items)
{
    if (!items.isEmpty()) sections->append(ViewSection{title, items});
}

} // namespace

QDate effectiveDate(const Entity &entity)
{
    if (entity.dueAt.isValid()) return toAppTime(entity.dueAt).date();
    if (entity.allDayDate.isValid()) return entity.allDayDate;
    return toAppTime(entity.startAt).date();
}

bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today)
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
        if (entity.kind == EntityKind::Note) {
            // 笔记保留的 status 不影响普通清单可见性，也永不进入任务时间/归档视图。
            if (spec.scope != ViewScope::AllLists && spec.scope != ViewScope::List) return false;
        } else if (entity.status != TaskStatus::Active) return false;
    }
    if (spec.scope == ViewScope::List && entity.listId != spec.listId) return false;
    if (timeScope(spec.scope)) {
        if (entity.kind != EntityKind::Task) return false;
        const QDate date = effectiveDate(entity);
        if (!date.isValid()) return false;
        if (spec.scope == ViewScope::Today && date > today) return false;
        if (spec.scope == ViewScope::Next7Days && (date < today || date > today.addDays(7))) return false;
        if (spec.scope == ViewScope::Overdue && date >= today) return false;
    }
    return searchMatches(entity, spec.search);
}

QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today)
{
    QVector<ViewSection> sections;
    QVector<Entity> filtered;
    for (const Entity &entity : entities) if (contains(entity, spec, today)) filtered.append(entity);
    if (archiveScope(spec.scope)) {
        std::sort(filtered.begin(), filtered.end(), [&spec](const Entity &a, const Entity &b) {
            const QDateTime ad = archiveTime(a, spec.scope), bd = archiveTime(b, spec.scope);
            if (ad != bd) {
                if (ad.isValid() != bd.isValid()) return ad.isValid();
                return ad > bd;
            }
            return a.id < b.id;
        });
        appendSection(&sections, archiveTitle(spec.scope), filtered);
        return sections;
    }

    QVector<Entity> pinned, overdue, upcoming, undated, notes;
    for (const Entity &entity : filtered) {
        if (entity.pinnedAt.isValid()) pinned.append(entity);
        else if (entity.kind == EntityKind::Note) notes.append(entity);
        else {
            const QDate date = effectiveDate(entity);
            if (!date.isValid()) undated.append(entity);
            else if (date < today) overdue.append(entity);
            else upcoming.append(entity);
        }
    }
    sortPinned(&pinned); sortByDate(&overdue); sortByDate(&upcoming); sortByCreated(&undated); sortByCreated(&notes);
    appendSection(&sections, QStringLiteral("置顶"), pinned);
    appendSection(&sections, QStringLiteral("已过期"), overdue);
    appendSection(&sections, QStringLiteral("未过期"), upcoming);
    if (spec.renderer == ViewRenderer::List) {
        appendSection(&sections, QStringLiteral("无日期"), undated);
        appendSection(&sections, QStringLiteral("笔记"), notes);
    }
    return sections;
}

QString scheduleLabel(const Entity &entity)
{
    const auto format = [](const QDateTime &value) { return toAppTime(value).toString(QStringLiteral("M月d日 HH:mm")); };
    if (entity.startAt.isValid() && entity.dueAt.isValid()) return format(entity.startAt) + QStringLiteral(" — ") + format(entity.dueAt);
    if (entity.dueAt.isValid()) return QStringLiteral("截止 ") + format(entity.dueAt);
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
