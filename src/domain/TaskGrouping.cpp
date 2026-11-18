#include "TaskGrouping.h"

#include "ViewSpec.h"

#include <algorithm>
#include <functional>

namespace kl {
namespace {

// 全局固定顺序：置顶 → 模式组(List/Time/Created/Priority/Flat) → 笔记 → Completed → Abandoned → Trash。
int kindOrder(GroupKey::Kind kind)
{
    switch (kind) {
    case GroupKey::Pinned: return 1;
    case GroupKey::List: return 2;
    case GroupKey::Time: return 3;
    case GroupKey::Created: return 4;
    case GroupKey::Priority: return 5;
    case GroupKey::Flat: return 6;
    case GroupKey::Notes: return 7;
    case GroupKey::Completed: return 8;
    case GroupKey::Abandoned: return 9;
    case GroupKey::Trash: return 10;
    case GroupKey::Invalid: break;
    }
    return 0;
}

// 显示组继承的“类别”：同类别且同清单时子级才跟随父级的显示组。
// 终态（Completed/Abandoned/Trash）与笔记不与 active 任务互相跟随。
enum class GroupClass { Active, Note, Completed, Abandoned, Trash };

GroupClass classOf(const Entity &entity)
{
    if (entity.deleted) return GroupClass::Trash;
    if (entity.kind == EntityKind::Note) return GroupClass::Note;
    switch (entity.status) {
    case TaskStatus::Completed: return GroupClass::Completed;
    case TaskStatus::Abandoned: return GroupClass::Abandoned;
    case TaskStatus::Active: break;
    }
    return GroupClass::Active;
}

QDateTime terminalTime(const Entity &entity, GroupClass groupClass)
{
    switch (groupClass) {
    case GroupClass::Completed: return entity.completedAt;
    case GroupClass::Abandoned: return entity.abandonedAt;
    case GroupClass::Trash: return entity.deletedAt;
    case GroupClass::Active:
    case GroupClass::Note: break;
    }
    return QDateTime();
}

// 类别定序：终态之间、或活动任务/笔记同 sortOrder 时的确定回退。
// 活动任务与笔记混合的置顶组须先共用 sortOrder，再按类别选择自然字段，保持严格弱序。
int classRank(GroupClass groupClass)
{
    switch (groupClass) {
    case GroupClass::Active: return 0;
    case GroupClass::Note: return 1;
    case GroupClass::Completed: return 2;
    case GroupClass::Abandoned: return 3;
    case GroupClass::Trash: return 4;
    }
    return 5;
}

// 比较结果 <0 / 0 / >0；无效值永远排最后，与升降序无关。
int compareDateInvalidLast(const QDate &a, const QDate &b)
{
    if (a == b) return 0;
    if (!a.isValid()) return 1;
    if (!b.isValid()) return -1;
    return a < b ? -1 : 1;
}
int compareDateTimeInvalidLast(const QDateTime &a, const QDateTime &b)
{
    if (a == b) return 0;
    if (!a.isValid()) return 1;
    if (!b.isValid()) return -1;
    return a < b ? -1 : 1;
}

QString weekdaySuffix(const QDate &date)
{
    if (!date.isValid()) return QString();
    return QStringLiteral(" 周") + QStringLiteral("一二三四五六日").mid(date.dayOfWeek() - 1, 1);
}

// 显示组继承：沿 parentId 上溯，链上每一步都要求“同清单 + 同类别”；
// 终态子级因此永远不会跨终态跟随 active 父组，反之亦然。visited 防坏数据环。
GroupKey resolveGroup(const Entity &entity, const QHash<QString, Entity> &visible,
                      GroupMode mode, const QDate &today, ViewClass view)
{
    const GroupClass groupClass = classOf(entity);
    GroupKey group = groupOf(entity, mode, today, view);
    const Entity *current = &entity;
    QSet<QString> visited;
    visited.insert(entity.id);
    while (true) {
        const QString parentId = current->parentId;
        if (parentId.isEmpty() || visited.contains(parentId)) break;
        const auto it = visible.find(parentId);
        if (it == visible.end()) break;
        if (it->listId != entity.listId || classOf(it.value()) != groupClass) break;
        visited.insert(parentId);
        current = &it.value();
        group = groupOf(*current, mode, today, view);
    }
    return group;
}

} // namespace

// ---------------------------------------------------------------- GroupKey --

QString GroupKey::stableId() const
{
    switch (kind) {
    case Pinned: return QStringLiteral("pinned");
    case List: return QStringLiteral("list:") + listId;
    case Time:
        switch (timeBucket()) {
        case TimeBucket::Overdue: return QStringLiteral("time:overdue");
        case TimeBucket::Today: return QStringLiteral("time:today");
        case TimeBucket::Tomorrow: return QStringLiteral("time:tomorrow");
        case TimeBucket::Next7Days: return QStringLiteral("time:next7days");
        case TimeBucket::Later: return QStringLiteral("time:later");
        case TimeBucket::Undated: return QStringLiteral("time:undated");
        }
        break;
    case Created:
        switch (createdBucket()) {
        case CreatedBucket::Last7Days: return QStringLiteral("created:last7");
        case CreatedBucket::Last30Days: return QStringLiteral("created:last30");
        case CreatedBucket::Earlier: return QStringLiteral("created:earlier");
        case CreatedBucket::Unknown: return QStringLiteral("created:unknown");
        }
        break;
    case Priority: return QStringLiteral("priority:%1").arg(bucket);
    case Flat: return QStringLiteral("none");
    case Notes: return QStringLiteral("notes");
    case Completed: return QStringLiteral("completed");
    case Abandoned: return QStringLiteral("abandoned");
    case Trash: return QStringLiteral("trash");
    case Invalid: break;
    }
    return QString();
}

GroupKey GroupKey::fromStableId(const QString &id)
{
    if (id == QStringLiteral("pinned")) return pinned();
    if (id == QStringLiteral("none")) return flat();
    if (id == QStringLiteral("notes")) return notes();
    if (id == QStringLiteral("completed")) return completed();
    if (id == QStringLiteral("abandoned")) return abandoned();
    if (id == QStringLiteral("trash")) return trash();
    if (id.startsWith(QStringLiteral("list:"))) return forList(id.mid(5));
    if (id.startsWith(QStringLiteral("priority:"))) {
        bool ok = false;
        const int priority = id.mid(9).toInt(&ok);
        if (ok && priority >= 0 && priority <= 3) return forPriority(priority);
        return GroupKey();
    }
    if (id == QStringLiteral("time:overdue")) return forTime(TimeBucket::Overdue);
    if (id == QStringLiteral("time:today")) return forTime(TimeBucket::Today);
    if (id == QStringLiteral("time:tomorrow")) return forTime(TimeBucket::Tomorrow);
    if (id == QStringLiteral("time:next7days")) return forTime(TimeBucket::Next7Days);
    if (id == QStringLiteral("time:later")) return forTime(TimeBucket::Later);
    if (id == QStringLiteral("time:undated")) return forTime(TimeBucket::Undated);
    if (id == QStringLiteral("created:last7")) return forCreated(CreatedBucket::Last7Days);
    if (id == QStringLiteral("created:last30")) return forCreated(CreatedBucket::Last30Days);
    if (id == QStringLiteral("created:earlier")) return forCreated(CreatedBucket::Earlier);
    if (id == QStringLiteral("created:unknown")) return forCreated(CreatedBucket::Unknown);
    return GroupKey();
}

QString GroupKey::title(const QDate &today) const
{
    switch (kind) {
    case Pinned: return QStringLiteral("已置顶");
    case Time:
        switch (timeBucket()) {
        case TimeBucket::Overdue: return QStringLiteral("已过期");
        case TimeBucket::Today: return QStringLiteral("今天") + weekdaySuffix(today);
        case TimeBucket::Tomorrow: return QStringLiteral("明天") + weekdaySuffix(today.isValid() ? today.addDays(1) : QDate());
        case TimeBucket::Next7Days: return QStringLiteral("最近7天");
        case TimeBucket::Later: return QStringLiteral("稍后");
        case TimeBucket::Undated: return QStringLiteral("无日期");
        }
        break;
    case Created:
        switch (createdBucket()) {
        case CreatedBucket::Last7Days: return QStringLiteral("过去7天");
        case CreatedBucket::Last30Days: return QStringLiteral("过去30天");
        case CreatedBucket::Earlier: return QStringLiteral("更早");
        case CreatedBucket::Unknown: return QStringLiteral("未知时间");
        }
        break;
    case Priority: return priorityLabel(bucket);
    case Flat: return QString();
    case Notes: return QStringLiteral("笔记");
    case Completed: return QStringLiteral("已完成");
    case Abandoned: return QStringLiteral("已放弃");
    case Trash: return QStringLiteral("回收站");
    case List:
    case Invalid: break;
    }
    return QString();
}

QString GroupKey::title(const QVector<TaskList> &lists, const QDate &today) const
{
    if (kind == List) {
        for (const TaskList &list : lists)
            if (list.id == listId) return list.name;
        return QStringLiteral("清单");
    }
    return title(today);
}

bool GroupKey::operator==(const GroupKey &other) const
{
    if (kind != other.kind) return false;
    switch (kind) {
    case List: return listId == other.listId;
    case Time:
    case Created:
    case Priority: return bucket == other.bucket;
    case Pinned:
    case Flat:
    case Notes:
    case Completed:
    case Abandoned:
    case Trash:
    case Invalid: break;
    }
    return true;
}

bool GroupKey::operator<(const GroupKey &other) const
{
    const int orderA = kindOrder(kind), orderB = kindOrder(other.kind);
    if (orderA != orderB) return orderA < orderB;
    switch (kind) {
    case Time:
    case Created:
        if (bucket != other.bucket) return bucket < other.bucket;
        break;
    case Priority:
        if (bucket != other.bucket) return bucket > other.bucket;   // 高 → 低
        break;
    case List:
        if (listId != other.listId) return listId < other.listId;   // 展示顺序由构建器覆盖
        break;
    case Pinned:
    case Flat:
    case Notes:
    case Completed:
    case Abandoned:
    case Trash:
    case Invalid: break;
    }
    return false;
}

// --------------------------------------------------------------- 纯规则 --

ViewClass viewClassOf(ViewScope scope)
{
    return scope == ViewScope::Completed || scope == ViewScope::Abandoned
        || scope == ViewScope::Trash ? ViewClass::History : ViewClass::Active;
}

TimeBucket timeBucketOf(const Entity &entity, const QDate &today)
{
    // 智能筛选按范围相交；互斥展示桶统一按截止／范围结束日，保证拖放目标与排序稳定。
    const QDate end = effectiveDate(entity);
    if (!end.isValid() || !today.isValid()) return TimeBucket::Undated;
    if (end < today) return TimeBucket::Overdue;
    if (end == today) return TimeBucket::Today;
    if (end == today.addDays(1)) return TimeBucket::Tomorrow;
    if (end <= today.addDays(6)) return TimeBucket::Next7Days;
    return TimeBucket::Later;
}

CreatedBucket createdBucketOf(const Entity &entity, const QDate &today)
{
    if (!entity.createdAt.isValid() || !today.isValid()) return CreatedBucket::Unknown;
    const QDate created = toAppTime(entity.createdAt).date();
    if (!created.isValid() || created > today) return CreatedBucket::Unknown;   // 未来/未知兜底
    const int age = created.daysTo(today);
    if (age <= 6) return CreatedBucket::Last7Days;       // 0..6 天前
    if (age <= 29) return CreatedBucket::Last30Days;     // 7..29 天前
    return CreatedBucket::Earlier;                       // ≥30 天前
}

GroupKey groupOf(const Entity &entity, GroupMode mode, const QDate &today, ViewClass view)
{
    if (view == ViewClass::History) {
        // 历史视角：只做显示分类，不克隆实体、不改数据；置顶/笔记/终态不参与分组。
        switch (mode) {
        case GroupMode::List: return GroupKey::forList(entity.listId);
        case GroupMode::Time: return GroupKey::forTime(timeBucketOf(entity, today));
        case GroupMode::Created: return GroupKey::forCreated(createdBucketOf(entity, today));
        case GroupMode::Priority:
            return GroupKey::forPriority(entity.kind == EntityKind::Task
                ? qBound(0, entity.priority, 3) : 0);
        case GroupMode::None: return GroupKey::flat();
        }
        return GroupKey::flat();
    }
    if (entity.deleted) return GroupKey::trash();
    if (entity.kind == EntityKind::Note)
        return entity.pinnedAt.isValid() ? GroupKey::pinned() : GroupKey::notes();
    switch (entity.status) {
    case TaskStatus::Completed: return GroupKey::completed();
    case TaskStatus::Abandoned: return GroupKey::abandoned();
    case TaskStatus::Active: break;
    }
    if (entity.pinnedAt.isValid()) return GroupKey::pinned();
    switch (mode) {
    case GroupMode::List: return GroupKey::forList(entity.listId);
    case GroupMode::Time: return GroupKey::forTime(timeBucketOf(entity, today));
    case GroupMode::Created: return GroupKey::forCreated(createdBucketOf(entity, today));
    case GroupMode::Priority: return GroupKey::forPriority(qBound(0, entity.priority, 3));
    case GroupMode::None: return GroupKey::flat();
    }
    return GroupKey::flat();
}

bool groupLessThan(const Entity &a, const Entity &b, GroupMode mode, const QDate &today)
{
    Q_UNUSED(today);
    const GroupClass classA = classOf(a);
    const GroupClass classB = classOf(b);
    const bool manualA = classA == GroupClass::Active || classA == GroupClass::Note;
    const bool manualB = classB == GroupClass::Active || classB == GroupClass::Note;
    // 置顶组可混合活动任务与笔记：两者必须共享手动顺序，类别仅作同 rank 回退。
    if (manualA && manualB && a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
    if (classA != classB) return classRank(classA) < classRank(classB);
    // 终态组例外：终态时间降序 → createdAt 降序 → id（终态不可手动排序，sortOrder 无意义）。
    if (classA != GroupClass::Active && classA != GroupClass::Note) {
        const int terminal = compareDateTimeInvalidLast(terminalTime(a, classA), terminalTime(b, classB));
        if (terminal != 0) return terminal > 0;
        const int created = compareDateTimeInvalidLast(a.createdAt, b.createdAt);
        if (created != 0) return created > 0;
        return a.id < b.id;
    }
    if (classA == GroupClass::Note) {
        const int created = compareDateTimeInvalidLast(a.createdAt, b.createdAt);
        if (created != 0) return created > 0;   // 笔记按创建时间新→旧
        return a.id < b.id;
    }
    if (mode == GroupMode::Created) {
        const int created = compareDateTimeInvalidLast(a.createdAt, b.createdAt);
        if (created != 0) return created > 0;   // Created 按创建时间新→旧
        return a.id < b.id;
    }
    // Time/Priority/List/None/Pinned：开始日期升序 → createdAt 降序 → id；逾期由结束日另行判断。
    const int date = compareDateInvalidLast(scheduleStartDate(a), scheduleStartDate(b));
    if (date != 0) return date < 0;
    const int created = compareDateTimeInvalidLast(a.createdAt, b.createdAt);
    if (created != 0) return created > 0;
    return a.id < b.id;
}

QVector<TaskList> groupingLists(const QVector<TaskList> &lists, const ViewSpec &spec)
{
    const bool history = viewClassOf(spec.scope) == ViewClass::History;
    QVector<TaskList> result;
    for (const TaskList &list : lists) {
        if (spec.scope == ViewScope::List && list.id != spec.listId) continue;   // 单清单视图只含自身
        if (!history && list.archivedAtUtcMs > 0) continue;   // 历史视角可含归档清单
        if (spec.scope == ViewScope::Folder && list.folderId != spec.folderId) continue;
        result.append(list);
    }
    std::sort(result.begin(), result.end(), [](const TaskList &a, const TaskList &b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });
    return result;
}

GroupMode defaultGrouping(ViewScope scope)
{
    return scope == ViewScope::Folder ? GroupMode::List : GroupMode::Time;
}

QString groupModeName(GroupMode mode)
{
    switch (mode) {
    case GroupMode::List: return QStringLiteral("list");
    case GroupMode::Time: return QStringLiteral("time");
    case GroupMode::Created: return QStringLiteral("created");
    case GroupMode::Priority: return QStringLiteral("priority");
    case GroupMode::None: return QStringLiteral("none");
    }
    return QStringLiteral("time");
}

GroupMode groupModeFromName(const QString &name, GroupMode fallback)
{
    if (name == QStringLiteral("list")) return GroupMode::List;
    if (name == QStringLiteral("time")) return GroupMode::Time;
    if (name == QStringLiteral("created")) return GroupMode::Created;
    if (name == QStringLiteral("priority")) return GroupMode::Priority;
    if (name == QStringLiteral("none")) return GroupMode::None;
    return fallback;
}

// ------------------------------------------------------------- 分组构建 --

Grouping groupEntities(const QVector<Entity> &entities, const QVector<TaskList> &lists,
                       const ViewSpec &spec, const QDate &today,
                       const QSet<QString> &archivedListIds)
{
    Grouping result;
    const ViewClass viewClass = viewClassOf(spec.scope);
    // 文件夹成员资格与分组模式无关；即使调用者未另传归档集合，也不能由 leftovers
    // 把归档清单加回活动聚合（buildFolderSections 的旧缺省调用同样遵守此口径）。
    const QSet<QString> folderMembers = spec.scope == ViewScope::Folder && !spec.folderId.isEmpty()
        ? folderListIds(groupingLists(lists, spec), spec.folderId) : QSet<QString>();

    QVector<Entity> filtered;
    QHash<QString, Entity> visible;
    for (const Entity &entity : entities) {
        if (!contains(entity, spec, today, archivedListIds, folderMembers)) continue;
        filtered.append(entity);
        visible.insert(entity.id, entity);
    }
    if (filtered.isEmpty()) return result;

    // 显示组解析（含同状态同清单的父子继承）。
    QHash<QString, GroupKey> resolved;
    for (const Entity &entity : filtered)
        resolved.insert(entity.id, resolveGroup(entity, visible, spec.grouping, today, viewClass));

    // 直接子级映射：仅当父子“同清单 + 同类别”时挂接（与解析规则一致）。
    QSet<QString> nested;
    for (const Entity &entity : filtered) {
        if (entity.parentId.isEmpty()) continue;
        const auto it = visible.find(entity.parentId);
        if (it == visible.end() || it->listId != entity.listId
            || classOf(it.value()) != classOf(entity)) continue;
        result.children[entity.parentId].append(entity);
        nested.insert(entity.id);
    }
    for (auto it = result.children.begin(); it != result.children.end(); ++it) {
        std::sort(it->begin(), it->end(), [&spec, &today](const Entity &a, const Entity &b) {
            return groupLessThan(a, b, spec.grouping, today);
        });
    }

    // 分桶 + 组内排序。
    QHash<QString, GroupKey> buckets;           // stableId -> GroupKey
    QHash<QString, QVector<Entity>> members;    // stableId -> 顶层实体
    for (const Entity &entity : filtered) {
        const GroupKey group = resolved.value(entity.id);
        const QString key = group.stableId();
        if (!buckets.contains(key)) buckets.insert(key, group);
        if (!nested.contains(entity.id)) members[key].append(entity);
    }
    // 坏数据环兜底：互相成环的实体既不是根也不可达，提升为根，保证不丢实体。
    {
        QSet<QString> reachable;
        std::function<void(const Entity &)> mark = [&](const Entity &entity) {
            if (reachable.contains(entity.id)) return;
            reachable.insert(entity.id);
            for (const Entity &child : result.children.value(entity.id)) mark(child);
        };
        for (auto it = members.constBegin(); it != members.constEnd(); ++it)
            for (const Entity &root : it.value()) mark(root);
        for (const Entity &entity : filtered) {
            if (reachable.contains(entity.id)) continue;
            members[resolved.value(entity.id).stableId()].append(entity);
            mark(entity);
        }
    }
    for (auto it = members.begin(); it != members.end(); ++it) {
        std::sort(it->begin(), it->end(), [&spec, &today](const Entity &a, const Entity &b) {
            return groupLessThan(a, b, spec.grouping, today);
        });
    }

    // 段顺序：Active 视图 = 置顶 → 模式组 → 笔记 → Completed → Abandoned（→ Trash 兜底）；
    // History 视图 = 只有模式组（List 可含归档清单），没有置顶/笔记/终态页脚。
    QVector<GroupKey> ordered;
    QSet<QString> placedKeys;
    const auto addIfPresent = [&](const GroupKey &group) {
        const QString key = group.stableId();
        if (members.contains(key) && !placedKeys.contains(key)) {
            ordered.append(group);
            placedKeys.insert(key);
        }
    };
    if (viewClass == ViewClass::Active) addIfPresent(GroupKey::pinned());
    switch (spec.grouping) {
    case GroupMode::List:
        for (const TaskList &list : groupingLists(lists, spec)) addIfPresent(GroupKey::forList(list.id));
        break;
    case GroupMode::Time:
        for (int bucket = 0; bucket <= static_cast<int>(TimeBucket::Undated); ++bucket)
            addIfPresent(GroupKey::forTime(static_cast<TimeBucket>(bucket)));
        break;
    case GroupMode::Created:
        for (int bucket = 0; bucket <= static_cast<int>(CreatedBucket::Unknown); ++bucket)
            addIfPresent(GroupKey::forCreated(static_cast<CreatedBucket>(bucket)));
        break;
    case GroupMode::Priority:
        for (int priority = 3; priority >= 0; --priority) addIfPresent(GroupKey::forPriority(priority));
        break;
    case GroupMode::None:
        addIfPresent(GroupKey::flat());
        break;
    }
    if (viewClass == ViewClass::Active) {
        addIfPresent(GroupKey::notes());
        addIfPresent(GroupKey::completed());
        addIfPresent(GroupKey::abandoned());
    }
    // 兜底：孤儿清单等未按上面顺序收录的组，按 stableId 追加，保证不丢实体。
    QVector<QString> leftovers;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it)
        if (!placedKeys.contains(it.key())) leftovers.append(it.key());
    std::sort(leftovers.begin(), leftovers.end());
    for (const QString &key : leftovers) {
        ordered.append(buckets.value(key));
        placedKeys.insert(key);
    }

    for (const GroupKey &group : ordered) {
        GroupedSection section;
        section.group = group;
        section.title = group.title(lists, today);
        section.items = members.value(group.stableId());
        result.sections.append(section);
    }
    return result;
}

} // namespace kl
