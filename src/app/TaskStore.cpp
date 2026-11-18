#include "TaskStore.h"

#include "EntityTextSession.h"

#include "../domain/ActivityPolicy.h"
#include "../domain/AttachmentCodec.h"
#include "../domain/BackupCodec.h"
#include "../domain/EntityCodec.h"
#include "../domain/RecurrenceEngine.h"
#include "../domain/ReminderRule.h"
#include "../domain/ViewSpec.h"

#include <QFile>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <algorithm>

namespace kl {
namespace {

QDateTime plannedAt(const Entity &task) {
    return task.hasScheduleRange && task.startAt.isValid() ? task.startAt : task.dueAt;
}
QDate plannedLocalDate(const Entity& task) {
    if (task.allDayDate.isValid()) return task.allDayDate;
    const QDateTime plan = plannedAt(task);
    return plan.isValid() ? plan.toTimeZone(QTimeZone(task.timeZone.toUtf8())).date() : QDate();
}
QString occurrenceKey(const Entity &task)
{
    if (task.allDayDate.isValid()) return task.allDayDate.toString(Qt::ISODate);
    const QDateTime plan = plannedAt(task);
    return plan.isValid() ? QString::number(utcMs(plan)) : QString();
}
const RecurrenceSeries* seriesFor(const EntitySnapshot& snapshot, const QString& id) {
    if (id.isEmpty()) return nullptr;
    for (const RecurrenceSeries& series : snapshot.recurrenceSeries) if (series.id == id) return &series;
    return nullptr;
}
bool simpleRecurrence(const Entity& value, RecurrenceSpec* spec, bool* enabled) {
    QString rule = value.rrule.isEmpty() ? value.recurrence : value.rrule;
    if (rule.isEmpty() || rule == QLatin1String("none")) {
        *enabled = false;
        return true;
    }
    QString frequency;
    int interval = 1;
    if (!parseRecurrence(rule, &frequency, &interval) || frequency.isEmpty()) return false;
    spec->kind = RecurrenceSpec::Kind::Gregorian;
    spec->basis = RecurrenceSpec::Basis::DueDate;
    spec->frequency = frequency == QLatin1String("DAILY") ? RecurrenceSpec::Frequency::Daily
        : frequency == QLatin1String("WEEKLY") ? RecurrenceSpec::Frequency::Weekly
        : RecurrenceSpec::Frequency::Monthly;
    spec->interval = interval;
    spec->lunarLeapMonthRepeat = false;
    if (spec->frequency == RecurrenceSpec::Frequency::Monthly)
        spec->monthDays.append(plannedLocalDate(value).day());
    *enabled = true;
    return isValidRecurrence(*spec);
}
bool sameReminder(const TaskReminder &a, const TaskReminder &b)
{
    return a.triggerAt == b.triggerAt && a.sortOrder == b.sortOrder && a.ruleJson == b.ruleJson;
}
TaskTag makeTaskTag(const QString &entityId, const QString &tagId)
{
    TaskTag link; link.id = newUuid(); link.entityId = entityId; link.tagId = tagId; return link;
}
void queueSeriesSave(EntityChanges* changes, const RecurrenceSeries& series)
{
    for (RecurrenceSeries& pending : changes->seriesSaves) if (pending.id == series.id) {
        pending = series;
        return;
    }
    changes->seriesSaves.append(series);
}
// 顶层“平铺清单 + 文件夹”混合序列的稳定顺序：sortOrder 优先；同序号时平铺清单在文件夹前，再按 id。
struct TopLevelRow {
    const TaskList *list = nullptr;
    const Folder *folder = nullptr;
    int sortOrder() const { return list ? list->sortOrder : folder->sortOrder; }
    bool isFolder() const { return folder != nullptr; }
    QString id() const { return list ? list->id : folder->id; }
};
bool topLevelLess(const TopLevelRow &a, const TopLevelRow &b)
{
    if (a.sortOrder() != b.sortOrder()) return a.sortOrder() < b.sortOrder();
    if (a.isFolder() != b.isFolder()) return !a.isFolder();   // 同序号：清单在文件夹前
    return a.id() < b.id();
}
void addEvent(EntityChanges *changes, const Entity &entity, const QString &type, const QString &field,
              const QVariant &oldValue, const QVariant &newValue, const QDateTime &occurredAt,
              const QJsonObject &context = QJsonObject())
{
    EntityEvent event; event.entityId = entity.id; event.entityKind = entity.kind; event.entityTitle = entity.title;
    event.type = type; event.field = field; event.oldValue = oldValue; event.newValue = newValue;
    event.occurredAt = occurredAt; event.context = context;
    if (isVisibleActivityEvent(event)) changes->eventSaves.append(event);
}
QVariant msValue(const QDateTime &date)
{
    return date.isValid() ? QVariant(QString::number(utcMs(date))) : QVariant();
}
QVariant idValue(const QString &id)
{
    return id.isEmpty() ? QVariant() : QVariant(id);
}
QString createdSnapshot(const Entity &entity)
{
    QJsonObject root; root.insert(QStringLiteral("title"), entity.title); root.insert(QStringLiteral("body"), entity.body);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}
QString tagSnapshot(const QStringList &tags)
{
    QStringList normalized;
    for (const QString &tag : tags) if (!tag.trimmed().isEmpty()) normalized.append(tag.trimmed());
    std::sort(normalized.begin(), normalized.end(), [](const QString &a, const QString &b) {
        const int result = QString::compare(a, b, Qt::CaseInsensitive); return result != 0 ? result < 0 : a < b;
    });
    QJsonArray array; for (const QString &tag : normalized) array.append(tag);
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}
const Entity *snapshotEntity(const EntitySnapshot &snapshot, const QString &id)
{
    for (const StoredEntity &row : snapshot.entities) if (row.entity.id == id) return &row.entity;
    return nullptr;
}
const TaskList *snapshotList(const EntitySnapshot &snapshot, const QString &id)
{
    for (const TaskList &row : snapshot.lists) if (row.id == id) return &row;
    return nullptr;
}
void completeActivityContexts(EntityChanges *changes, const EntitySnapshot &snapshot)
{
    const auto currentEntity = [&](const QString &id) -> const Entity * {
        for (const StoredEntity &row : changes->saves) if (row.entity.id == id) return &row.entity;
        return snapshotEntity(snapshot, id);
    };
    const auto parentTitle = [&](const QString &id, bool current) {
        const Entity *parent = current ? currentEntity(id) : snapshotEntity(snapshot, id);
        return parent ? parent->title : QString();
    };
    const auto listName = [&](const QString &id, bool current) {
        if (current) for (const TaskList &row : changes->groupSaves) if (row.id == id) return row.name;
        const TaskList *list = snapshotList(snapshot, id);
        return list ? list->name : QString();
    };
    for (EntityEvent &event : changes->eventSaves) {
        const Entity *entity = currentEntity(event.entityId);
        if (event.type == QStringLiteral("changed") && event.field == QStringLiteral("parent")) {
            event.context.insert(QStringLiteral("oldParentTitle"), parentTitle(event.oldValue.toString(), false));
            event.context.insert(QStringLiteral("newParentTitle"), parentTitle(event.newValue.toString(), true));
        } else if (entity && !entity->parentId.isEmpty()) {
            event.context.insert(QStringLiteral("newParentTitle"), parentTitle(entity->parentId, true));
        }
        if (event.type == QStringLiteral("changed") && event.field == QStringLiteral("list")) {
            event.context.insert(QStringLiteral("oldListName"), listName(event.oldValue.toString(), false));
            event.context.insert(QStringLiteral("newListName"), listName(event.newValue.toString(), true));
        }
        if (entity && !event.context.contains(QStringLiteral("listName")))
            event.context.insert(QStringLiteral("listName"), listName(entity->listId, true));
        if (event.type == QStringLiteral("attachment_added") || event.type == QStringLiteral("attachment_removed")) {
            const QString id = event.type == QStringLiteral("attachment_added")
                ? event.newValue.toString() : event.oldValue.toString();
            const Attachment *attachment = nullptr;
            for (const Attachment &row : changes->attachmentSaves) if (row.id == id) attachment = &row;
            for (const Attachment &row : changes->attachmentRemovals) if (row.id == id) attachment = &row;
            if (!attachment) for (const Attachment &row : snapshot.attachments) if (row.id == id) attachment = &row;
            if (attachment) event.context.insert(QStringLiteral("attachmentName"), attachment->originalName);
        }
    }
}

// DropPlan 只保存经完整快照验证后的最终实体值；可见顺序不参与子树闭包计算。
struct DropPlan {
    QVector<Entity> updates;
};
struct DropSnapshot {
    explicit DropSnapshot(const EntitySnapshot &snapshot)
    {
        for (const StoredEntity &row : snapshot.entities) {
            entities.insert(row.entity.id, row.entity);
            children[row.entity.parentId].append(row.entity.id);
        }
        for (const TaskList &list : snapshot.lists) lists.insert(list.id, list);
    }
    QHash<QString, Entity> entities;
    QHash<QString, QStringList> children;
    QHash<QString, TaskList> lists;
};
bool dropFailure(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
bool availableList(const DropSnapshot &snapshot, const QString &id)
{
    const auto list = snapshot.lists.constFind(id);
    return list != snapshot.lists.constEnd() && list->archivedAtUtcMs == 0;
}
bool activeDropEntity(const Entity &entity, const DropSnapshot &snapshot)
{
    return !entity.deleted && (entity.kind == EntityKind::Task || entity.kind == EntityKind::Note)
        && (entity.kind == EntityKind::Note || entity.status == TaskStatus::Active)
        && availableList(snapshot, entity.listId);
}
bool groupMatchesMode(const GroupKey &group, GroupMode mode, const DropSnapshot &snapshot)
{
    switch (group.kind) {
    case GroupKey::Pinned:
    case GroupKey::Notes: return true;
    case GroupKey::List: return mode == GroupMode::List && isUuid(group.listId) && availableList(snapshot, group.listId);
    case GroupKey::Time: return mode == GroupMode::Time && group.bucket >= static_cast<int>(TimeBucket::Overdue)
            && group.bucket <= static_cast<int>(TimeBucket::Undated);
    case GroupKey::Priority: return mode == GroupMode::Priority && group.bucket >= 0 && group.bucket <= 3;
    case GroupKey::Flat: return mode == GroupMode::None;
    default: return false; // Created、终态和未知组均不接受拖放，绝不仅排序伪成功。
    }
}
bool validateAncestors(const DropSnapshot &snapshot, const QString &id, const QString &forbiddenId, QString *error)
{
    QSet<QString> visited;
    QString current = id;
    while (!current.isEmpty()) {
        if (current == forbiddenId) return dropFailure(error, QStringLiteral("不能把任务移动到自身或其后代下"));
        if (visited.contains(current)) return dropFailure(error, QStringLiteral("任务层级存在循环"));
        visited.insert(current);
        const auto entity = snapshot.entities.constFind(current);
        if (entity == snapshot.entities.constEnd()) return dropFailure(error, QStringLiteral("任务祖先链不完整"));
        current = entity->parentId;
    }
    return true;
}
bool collectSubtree(const DropSnapshot &snapshot, const QString &sourceId, QSet<QString> *ids, QString *error)
{
    QStringList pending{sourceId};
    for (int index = 0; index < pending.size(); ++index) {
        const QString id = pending.at(index);
        if (ids->contains(id)) return dropFailure(error, QStringLiteral("任务子树存在循环"));
        if (!snapshot.entities.contains(id)) return dropFailure(error, QStringLiteral("任务子树不完整"));
        ids->insert(id);
        pending.append(snapshot.children.value(id));
    }
    return true;
}
bool canShowInGroup(const Entity &entity, const DropSnapshot &snapshot, const GroupKey &group,
                    GroupMode mode, const QDate &today)
{
    // 筛选或折叠可能隐藏父项；显示组可以取自身或连续同状态同清单祖先的属性组。
    // 此处只验证可能性，不能把 orderedIds 当成完整可见树，更不能当成真实子树。
    const Entity *current = &entity;
    QSet<QString> visited;
    while (current && !visited.contains(current->id)) {
        visited.insert(current->id);
        if (groupOf(*current, mode, today) == group) return true;
        const auto parent = snapshot.entities.constFind(current->parentId);
        if (parent == snapshot.entities.constEnd() || parent->kind != entity.kind
            || parent->status != entity.status || parent->deleted != entity.deleted
            || parent->listId != entity.listId) break;
        current = &parent.value();
    }
    return false;
}
bool moveSubtreeToList(DropSnapshot *snapshot, const QString &sourceId, const QString &targetListId,
                       const QSet<QString> &subtree, QString *error)
{
    if (!availableList(*snapshot, targetListId)) return dropFailure(error, QStringLiteral("目标清单不存在或已归档"));
    const Entity source = snapshot->entities.value(sourceId);
    bool crossing = false;
    for (const QString &id : subtree) {
        const Entity &entity = snapshot->entities.value(id);
        if (entity.kind != source.kind) return dropFailure(error, QStringLiteral("子树实体类型不一致"));
        crossing = crossing || entity.listId != targetListId;
    }
    if (!crossing) return true;
    for (const QString &id : subtree) {
        Entity &entity = snapshot->entities[id];
        entity.listId = targetListId;
        entity.sectionId.clear();
    }
    // 独立移动子项不拖走其祖先，旧父项不在本次闭包中，必须脱父。
    snapshot->entities[sourceId].parentId.clear();
    return true;
}
bool changedByDrop(const Entity &before, const Entity &after)
{
    return before.sortOrder != after.sortOrder || before.parentId != after.parentId
        || before.pinnedAt != after.pinnedAt || before.listId != after.listId
        || before.sectionId != after.sectionId || before.priority != after.priority
        || before.allDayDate != after.allDayDate || before.allDayEndDate != after.allDayEndDate
        || before.hasScheduleRange != after.hasScheduleRange
        || before.startAt != after.startAt || before.dueAt != after.dueAt;
}
void finishDropPlan(const EntitySnapshot &snapshot, const DropSnapshot &planned, DropPlan *plan)
{
    for (const StoredEntity &row : snapshot.entities) {
        const Entity value = planned.entities.value(row.entity.id);
        if (changedByDrop(row.entity, value)) plan->updates.append(value);
    }
}
void reorderDropSiblings(DropSnapshot *snapshot, const TaskDrop::Intent &intent, const QDate &today,
                         bool sourceWasSibling)
{
    const Entity source = snapshot->entities.value(intent.sourceId);
    QStringList siblings;
    QSet<QString> siblingIds;
    for (const Entity &entity : snapshot->entities) {
        // 扁平提示中的后代和其他父级的行不是 source 的排序同级；终态/回收站/归档
        // 也不参与活动排序。根行可跨清单汇总，子级只和同清单的真正兄弟比较。
        if (entity.parentId != source.parentId
            || (!source.parentId.isEmpty() && entity.listId != source.listId)
            || !activeDropEntity(entity, *snapshot)
            || !canShowInGroup(entity, *snapshot, intent.targetGroup, intent.mode, today)) continue;
        siblings.append(entity.id);
        siblingIds.insert(entity.id);
    }
    std::sort(siblings.begin(), siblings.end(), [&](const QString &a, const QString &b) {
        return groupLessThan(snapshot->entities.constFind(a).value(), snapshot->entities.constFind(b).value(),
                             intent.mode, today);
    });

    QStringList visibleOrder, currentVisibleOrder;
    QSet<QString> visibleIds;
    for (const QString &id : intent.orderedIds) if (siblingIds.contains(id)) {
        visibleOrder.append(id);
        visibleIds.insert(id);
    }
    for (const QString &id : siblings) if (visibleIds.contains(id)) currentVisibleOrder.append(id);
    // 搜索/折叠后的同组手势没有改变可见同级顺序时，保留原 rank（含非连续值），
    // 不因规范化而推进 revision、扰动隐藏项或生成无意义的写入。
    if (sourceWasSibling && currentVisibleOrder == visibleOrder) return;

    QStringList merged = siblings;
    merged.removeAll(intent.sourceId);
    int insertAt = merged.size();
    if ((intent.placement == TaskDrop::Placement::Before || intent.placement == TaskDrop::Placement::After)
        && siblingIds.contains(intent.anchorId)) {
        insertAt = merged.indexOf(intent.anchorId)
            + (intent.placement == TaskDrop::Placement::After ? 1 : 0);
    } else if (intent.placement != TaskDrop::Placement::IntoGroup) {
        // 参照行可能是其他根行的后代；只把可见提示投影为真正同级的落点。
        // 除 source 外的成员始终沿用最新快照里的逻辑顺序，不能被陈旧提示互换。
        const int sourceIndex = visibleOrder.indexOf(intent.sourceId);
        bool found = false;
        for (int index = sourceIndex + 1; index < visibleOrder.size(); ++index) {
            insertAt = merged.indexOf(visibleOrder.at(index));
            if (insertAt >= 0) { found = true; break; }
        }
        if (!found) for (int index = sourceIndex - 1; index >= 0; --index) {
            const int previous = merged.indexOf(visibleOrder.at(index));
            if (previous >= 0) { insertAt = previous + 1; found = true; break; }
        }
        if (!found) insertAt = intent.placement == TaskDrop::Placement::AsChild ? 0 : merged.size();
    }
    merged.insert(insertAt, intent.sourceId);
    if (merged == siblings) return;
    // 只有真正移动了 source 才规范化完整目标同级序列；隐藏但 rank 改变的成员也会
    // 经 finishDropPlan 进入同一次 GroupMove 保存与仓储 CAS，失败整批回滚。
    for (int index = 0; index < merged.size(); ++index)
        snapshot->entities[merged.at(index)].sortOrder = index;
}

bool buildDropPlan(const TaskDrop::Intent &intent, const EntitySnapshot &snapshot, const QDate &today,
                   DropPlan *plan, QString *error)
{
    DropSnapshot planned(snapshot);
    if (!intent.isValid()) return dropFailure(error, QStringLiteral("拖放意图结构非法"));
    if (intent.mode != GroupMode::List && intent.mode != GroupMode::Time
        && intent.mode != GroupMode::Priority && intent.mode != GroupMode::None)
        return dropFailure(error, QStringLiteral("当前分组不允许拖放"));
    if (!groupMatchesMode(intent.sourceGroup, intent.mode, planned)
        || !groupMatchesMode(intent.targetGroup, intent.mode, planned))
        return dropFailure(error, QStringLiteral("源组或目标组不可用，或与分组模式不一致"));
    const auto sourceFound = planned.entities.constFind(intent.sourceId);
    if (sourceFound == planned.entities.constEnd() || !activeDropEntity(sourceFound.value(), planned))
        return dropFailure(error, QStringLiteral("源实体必须是未删除且清单可用的进行中任务或笔记"));
    const Entity originalSource = sourceFound.value();
    if (!validateAncestors(planned, intent.sourceId, QString(), error)) return false;
    const bool matchesSource = canShowInGroup(originalSource, planned, intent.sourceGroup, intent.mode, today);
    const bool alreadyInTarget = canShowInGroup(originalSource, planned, intent.targetGroup, intent.mode, today);
    if (!matchesSource && !alreadyInTarget) return dropFailure(error, QStringLiteral("源实体已不在拖动时的显示组，请重试"));
    QSet<QString> subtree;
    if (!collectSubtree(planned, intent.sourceId, &subtree, error)) return false;

    const QDate mappedDate = TaskDrop::dateForGroup(intent.targetGroup, today);
    if (intent.targetDate.isValid() && (intent.targetGroup.kind != GroupKey::Time || intent.targetDate != mappedDate))
        return dropFailure(error, QStringLiteral("目标日期与时间组不一致"));
    const bool crossing = intent.sourceGroup != intent.targetGroup && matchesSource;
    if (intent.placement == TaskDrop::Placement::AsChild) {
        const auto parent = planned.entities.constFind(intent.targetParentId);
        if (originalSource.kind != EntityKind::Task || parent == planned.entities.constEnd()
            || parent->kind != EntityKind::Task || !activeDropEntity(parent.value(), planned)
            || parent->listId != originalSource.listId)
            return dropFailure(error, QStringLiteral("设父需要同清单、未删除的进行中任务"));
        if (!validateAncestors(planned, intent.targetParentId, intent.sourceId, error)) return false;
        if (!canShowInGroup(parent.value(), planned, intent.targetGroup, intent.mode, today))
            return dropFailure(error, QStringLiteral("父任务不属于目标显示组"));
        planned.entities[intent.sourceId].parentId = intent.targetParentId;
    } else {
        if (!intent.targetParentId.isEmpty()) return dropFailure(error, QStringLiteral("仅设父意图可指定父任务"));
        if (originalSource.kind == EntityKind::Note && intent.targetGroup.kind != GroupKey::Pinned
            && intent.targetGroup.kind != GroupKey::Notes)
            return dropFailure(error, QStringLiteral("笔记不能拖入任务属性组"));
        if (originalSource.kind == EntityKind::Task && intent.targetGroup.kind == GroupKey::Notes)
            return dropFailure(error, QStringLiteral("拖放不能隐式把任务转换为笔记"));
        Entity &source = planned.entities[intent.sourceId];
        if (crossing) {
            source.parentId.clear();
        } else if (intent.placement == TaskDrop::Placement::Before || intent.placement == TaskDrop::Placement::After) {
            const auto anchor = planned.entities.constFind(intent.anchorId);
            if (anchor == planned.entities.constEnd() || !activeDropEntity(anchor.value(), planned)
                || subtree.contains(intent.anchorId)
                || !canShowInGroup(anchor.value(), planned, intent.targetGroup, intent.mode, today))
                return dropFailure(error, QStringLiteral("参照行不可用或属于源子树"));
            const QString siblingParentId = anchor->parentId;
            if (siblingParentId != source.parentId) {
                if (!siblingParentId.isEmpty()) {
                    const auto parent = planned.entities.constFind(siblingParentId);
                    if (source.kind != EntityKind::Task || parent == planned.entities.constEnd()
                        || parent->kind != EntityKind::Task || !activeDropEntity(parent.value(), planned)
                        || parent->listId != source.listId || anchor->listId != source.listId)
                        return dropFailure(error, QStringLiteral("同级落点需要同清单、未删除的进行中父任务"));
                    if (!validateAncestors(planned, siblingParentId, intent.sourceId, error)) return false;
                }
                source.parentId = siblingParentId;
            }
        }
        // 展示组相同不代表层级相同：子项落到独立根行的前后同样会脱父。
        // 仅脱父后才需用真实属性匹配目标；普通同父排序与设父都不能暗改日期/置顶。
        const bool detached = source.parentId.isEmpty() && source.parentId != originalSource.parentId;
        if (crossing || detached) {
            if (intent.targetGroup.kind == GroupKey::Pinned) {
                if (!source.pinnedAt.isValid()) source.pinnedAt = utcNow();
            } else source.pinnedAt = QDateTime();
            if (intent.targetGroup.kind == GroupKey::Time
                && (crossing || timeBucketOf(source, today) != intent.targetGroup.timeBucket())) {
                if (intent.targetGroup.timeBucket() == TimeBucket::Undated) {
                    source.allDayDate = QDate(); source.allDayEndDate = QDate();
                    source.startAt = QDateTime(); source.dueAt = QDateTime(); source.hasScheduleRange = false;
                } else {
                    if (!mappedDate.isValid() || intent.targetDate != mappedDate)
                        return dropFailure(error, QStringLiteral("跨时间组或脱父拖放必须指定匹配的目标日期"));
                    const QDate from = effectiveDate(source);
                    if (!from.isValid()) source.allDayDate = mappedDate;
                    else {
                        const qint64 days = from.daysTo(mappedDate);
                        if (source.allDayDate.isValid()) {
                            source.allDayDate = source.allDayDate.addDays(days);
                            if (source.allDayEndDate.isValid()) source.allDayEndDate = source.allDayEndDate.addDays(days);
                        }
                        const QTimeZone zone(source.timeZone.toUtf8());
                        if (source.startAt.isValid()) source.startAt = source.startAt.toTimeZone(zone).addDays(days).toUTC();
                        if (source.dueAt.isValid()) source.dueAt = source.dueAt.toTimeZone(zone).addDays(days).toUTC();
                    }
                }
            } else if (intent.targetGroup.kind == GroupKey::Priority) source.priority = TaskDrop::priorityForGroup(intent.targetGroup);
        }
        if (intent.targetGroup.kind == GroupKey::List
            && !moveSubtreeToList(&planned, intent.sourceId, intent.targetGroup.listId, subtree, error)) return false;
    }

    QSet<QString> seen;
    for (const QString &id : intent.orderedIds) {
        const auto entity = planned.entities.constFind(id);
        if (seen.contains(id) || entity == planned.entities.constEnd() || !activeDropEntity(entity.value(), planned))
            return dropFailure(error, QStringLiteral("目标顺序有重复、缺失或不可用实体"));
        seen.insert(id);
        if (!canShowInGroup(entity.value(), planned, intent.targetGroup, intent.mode, today))
            return dropFailure(error, QStringLiteral("顺序提示中的实体不属于目标显示组"));
    }
    if (!seen.contains(intent.sourceId)) return dropFailure(error, QStringLiteral("目标顺序必须包含源实体"));
    if (intent.placement == TaskDrop::Placement::Before || intent.placement == TaskDrop::Placement::After) {
        if (!seen.contains(intent.anchorId) || subtree.contains(intent.anchorId))
            return dropFailure(error, QStringLiteral("参照行不存在于目标顺序，或属于源子树"));
        const int sourceIndex = intent.orderedIds.indexOf(intent.sourceId), anchorIndex = intent.orderedIds.indexOf(intent.anchorId);
        if ((intent.placement == TaskDrop::Placement::Before && sourceIndex >= anchorIndex)
            || (intent.placement == TaskDrop::Placement::After && sourceIndex <= anchorIndex))
            return dropFailure(error, QStringLiteral("顺序提示与参照行落点不一致"));
    } else if (intent.placement != TaskDrop::Placement::IntoGroup && intent.placement != TaskDrop::Placement::AsChild) {
        return dropFailure(error, QStringLiteral("拖放落点类型非法"));
    }
    const Entity &source = planned.entities.constFind(intent.sourceId).value();
    const bool sourceWasSibling = alreadyInTarget && source.parentId == originalSource.parentId
        && source.listId == originalSource.listId;
    reorderDropSiblings(&planned, intent, today, sourceWasSibling);
    finishDropPlan(snapshot, planned, plan);
    return true;
}

class RestoreFiles
{
public:
    RestoreFiles(const QString &dataDir, const QString &attachmentRoot)
        : m_root(attachmentRoot), m_workspace(QDir(dataDir).filePath(QStringLiteral(".restore-%1").arg(newUuid()))),
          m_staging(QDir(m_workspace).filePath(QStringLiteral("staging"))),
          m_backup(QDir(m_workspace).filePath(QStringLiteral("backup"))) {}
    QString stagingPath(const Attachment &attachment) const { return QDir(m_staging).filePath(attachment.relativePath); }
    bool create(QString *error)
    {
        if (m_root.isEmpty() || !QDir().mkpath(m_staging) || !QDir().mkpath(m_backup))
            return fail(error, QStringLiteral("无法创建独立附件暂存目录"));
        return true;
    }
    bool swap(QString *error)
    {
        if (!QDir().mkpath(m_root)) return rollbackWithError(error, QStringLiteral("无法创建附件根目录"));
        const QFileInfoList oldFiles = QDir(m_root).entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks);
        for (const QFileInfo &file : oldFiles) {
            const QString source = file.absoluteFilePath(), backup = QDir(m_backup).filePath(file.fileName());
            if (!move(source, backup)) return rollbackWithError(error, QStringLiteral("无法备份本机旧附件: ") + file.fileName());
        }
        const QFileInfoList stagedFiles = QDir(m_staging).entryInfoList(QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo &file : stagedFiles) {
            const QString source = file.absoluteFilePath(), target = QDir(m_root).filePath(file.fileName());
            if (!move(source, target)) return rollbackWithError(error, QStringLiteral("无法将暂存附件移动到最终路径: ") + file.fileName());
        }
        return true;
    }
    bool rollback(QString *error)
    {
        bool ok = true; QString firstError;
        for (int i = m_moves.size() - 1; i >= 0; --i) {
            const Move &move = m_moves.at(i);
            if (!QFile::rename(move.second, move.first)) {
                ok = false;
                if (firstError.isEmpty()) firstError = move.second;
            }
        }
        if (!ok) return fail(error, QStringLiteral("附件交换回滚失败，恢复目录已保留: ") + firstError);
        m_moves.clear();
        if (!removeWorkspace()) return fail(error, QStringLiteral("附件已回滚，但暂存目录清理失败: ") + m_workspace);
        return true;
    }
    bool cleanup(QString *error)
    {
        m_moves.clear();
        if (!removeWorkspace()) return fail(error, QStringLiteral("导入已提交，但旧附件备份清理失败: ") + m_workspace);
        return true;
    }
private:
    using Move = QPair<QString, QString>;
    bool removeWorkspace()
    {
        // Qt 5.12/Windows 对只含空子目录的根目录 removeRecursively 不稳定，分别清理并核实结果。
        QDir staging(m_staging), backup(m_backup), workspace(m_workspace);
        staging.removeRecursively(); backup.removeRecursively(); workspace.removeRecursively();
        return !QDir(m_staging).exists() && !QDir(m_backup).exists() && !QDir(m_workspace).exists();
    }
    static bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
    bool move(const QString &source, const QString &target)
    {
        if (!QFile::rename(source, target)) return false;
        m_moves.append(qMakePair(source, target)); return true;
    }
    bool rollbackWithError(QString *error, const QString &message)
    {
        QString rollbackError; rollback(&rollbackError);
        return fail(error, rollbackError.isEmpty() ? message : message + QStringLiteral("；") + rollbackError);
    }
    QString m_root, m_workspace, m_staging, m_backup;
    QVector<Move> m_moves;
};
}

TaskStore::TaskStore(const QSqlDatabase &database, QObject *parent) : QObject(parent), m_repository(database) {}

TaskStore::~TaskStore()
{
    destroyTextSessions();
}

void TaskStore::destroyTextSessions()
{
    // 会话以本 store 为 QObject 父，但父类析构删子对象发生在成员销毁之后；这里必须先
    // 主动清空注册表再删除，避免 destroyed 钩子触碰已销毁的 QHash 成员。
    QHash<QString, EntityTextSession *> sessions;
    m_textSessions.swap(sessions);
    qDeleteAll(sessions);
}

bool TaskStore::fail(const QString &message, QString *error)
{
    m_lastError = message;
    if (error) *error = message;
    emit errorOccurred(message);
    return false;
}
bool TaskStore::ensureLoaded() { return m_loaded ? true : fail(QStringLiteral("请先加载任务数据")); }

bool TaskStore::load(QString *error)
{
    if (!m_attachmentStore) m_attachmentStore.reset(new AttachmentStore(dataDirPath()));
    EntitySnapshot snapshot; QString failure;
    if (!m_repository.load(&snapshot, &failure)) return fail(failure, error);
    m_snapshot = snapshot; m_loaded = true; m_lastError.clear();
    if (error) error->clear();
    emit groupsChanged(); emit tasksReset(); return true;
}

QVector<Entity> TaskStore::entities() const
{
    QVector<Entity> result; result.reserve(m_snapshot.entities.size());
    for (const StoredEntity &stored : m_snapshot.entities) {
        Entity value = stored.entity; applyDesktopFacade(&value); result.append(value);
    }
    return result;
}
QVector<TaskGroup> TaskStore::groups() const
{
    QVector<TaskGroup> result;
    for (const TaskList &row : m_snapshot.lists) if (!row.inbox) result.append(row);
    return result;
}
QVector<TaskList> TaskStore::lists() const { return m_snapshot.lists; }
QVector<Folder> TaskStore::folders() const { return m_snapshot.folders; }
QVector<Section> TaskStore::sections() const { return m_snapshot.sections; }
const StoredEntity *TaskStore::find(const QString &id) const
{
    for (const StoredEntity &stored : m_snapshot.entities) if (stored.entity.id == id) return &stored;
    return nullptr;
}
const TaskList *TaskStore::findList(const QString &id) const
{
    for (const TaskList &row : m_snapshot.lists) if (row.id == id) return &row;
    return nullptr;
}
const RecurrenceLink *TaskStore::linkFor(const QString &sourceId) const
{
    for (const RecurrenceLink &link : m_snapshot.recurrenceLinks) if (link.sourceId == sourceId) return &link;
    return nullptr;
}
const Attachment *TaskStore::findAttachment(const QString &id) const
{
    for (const Attachment &row : m_snapshot.attachments) if (row.id == id && find(row.entityId)) return &row;
    return nullptr;
}
const TaskReminder *TaskStore::findReminder(const QString &id) const
{
    for (const TaskReminder &row : m_snapshot.reminders) if (row.id == id && find(row.entityId)) return &row;
    return nullptr;
}
bool TaskStore::hasGroup(const QString &id) const
{
    if (id.isEmpty()) return true;
    for (const TaskList &row : m_snapshot.lists) if (row.id == id) return true;
    return false;
}
bool TaskStore::hasFolder(const QString &id) const
{
    if (id.isEmpty()) return true;
    for (const Folder &row : m_snapshot.folders) if (row.id == id) return true;
    return false;
}
bool TaskStore::hasSection(const QString &id) const
{
    if (id.isEmpty()) return true;
    for (const Section &row : m_snapshot.sections) if (row.id == id) return true;
    return false;
}
Entity TaskStore::entity(const QString &id) const
{
    const StoredEntity *stored = find(id); if (!stored) return Entity(); Entity value = stored->entity; applyDesktopFacade(&value); return value;
}

QVector<Attachment> TaskStore::attachments(const QString &entityId) const
{
    QVector<Attachment> result; if (!find(entityId)) return result;
    for (const Attachment &row : m_snapshot.attachments) if (row.entityId == entityId) result.append(row);
    return result;
}
QSet<QString> TaskStore::attachmentEntityIds() const
{
    QSet<QString> result;
    for (const Attachment& row : m_snapshot.attachments) result.insert(row.entityId);
    return result;
}
QString TaskStore::attachmentFilePath(const Attachment &attachment) const
{
    if (!m_attachmentStore) return QString();
    const Attachment *known = findAttachment(attachment.id);
    if (!known || known->relativePath != attachment.relativePath) return QString();
    const QString path = m_attachmentStore->filePath(attachment);
    return !path.isEmpty() && QFile::exists(path) && QFileInfo(path).isFile() ? path : QString();
}
int TaskStore::nextAttachmentSortOrder(const QString &entityId) const
{
    int order = 0;
    for (const Attachment &row : m_snapshot.attachments) if (row.entityId == entityId) order = qMax(order, row.sortOrder + 1);
    return order;
}
QVector<TaskReminder> TaskStore::reminders(const QString &entityId) const { return entity(entityId).reminders; }

QString TaskStore::installationId() const { return localMeta(QStringLiteral("installation_id")); }
QString TaskStore::deviceId() const { return localMeta(QStringLiteral("device_id")); }
QString TaskStore::inboxListId() const { return localMeta(QStringLiteral("inbox_list_id")); }
QString TaskStore::localMeta(const QString &key) const
{
    for (const LocalMeta &row : m_snapshot.localMeta) if (row.key == key) return row.value;
    return QString();
}

bool TaskStore::commit(EntityChanges *changes, bool reset, const std::function<void()> &beforeSignals)
{
    // 只过滤活动事件，不以“无事件”为由跳过排序、置顶或其他数据持久化。
    auto &events = changes->eventSaves;
    events.erase(std::remove_if(events.begin(), events.end(), [](const EntityEvent &event) {
        return !isVisibleActivityEvent(event);
    }), events.end());
    completeActivityContexts(changes, m_snapshot);
    EntitySnapshot snapshot; QString failure; QSet<QString> changedIds, desktopChangedIds;
    for (const DesktopNote &note : changes->desktopNoteSaves) { changedIds.insert(note.entityId); desktopChangedIds.insert(note.entityId); }
    for (const QString &id : changes->desktopNoteRemovals) {
        for (const DesktopNote &note : m_snapshot.desktopNotes) if (note.id == id) {
            changedIds.insert(note.entityId); desktopChangedIds.insert(note.entityId);
        }
    }
    for (const StoredEntity &row : changes->removals) changedIds.insert(row.entity.id);
    for (const StoredEntity &row : changes->touches) changedIds.insert(row.entity.id);
    for (const StoredEntity &row : changes->saves) changedIds.insert(row.entity.id);
    for (const TaskReminder &row : changes->reminderSaves) changedIds.insert(row.entityId);
    for (const ReminderDelivery &row : changes->deliverySaves) {
        const TaskReminder *reminder = findReminder(row.reminderId); if (reminder) changedIds.insert(reminder->entityId);
    }
    for (const QString &id : changes->reminderRemovals) {
        const TaskReminder *reminder = findReminder(id); if (reminder) changedIds.insert(reminder->entityId);
    }
    QSet<QString> removedTasks;
    for (const StoredEntity &row : changes->removals) removedTasks.insert(row.entity.id);
    // 同一批里完整保存的实体不再重复 touch；多个孩子变更也只 touch 父级一次。
    // 否则先推进父级 revision，随后按旧 revision 保存父级会让正常拖放 CAS 失败。
    QSet<QString> savedIds, touchedIds;
    for (const StoredEntity &row : changes->saves) savedIds.insert(row.entity.id);
    for (int index = changes->touches.size() - 1; index >= 0; --index) {
        const QString id = changes->touches.at(index).entity.id;
        if (savedIds.contains(id) || touchedIds.contains(id)) changes->touches.remove(index);
        else touchedIds.insert(id);
    }
    // 父任务永久删除会级联真正子任务，文件清理必须覆盖整个子树。
    bool grew = true;
    while (grew) {
        grew = false;
        for (const StoredEntity &row : m_snapshot.entities)
            if (!removedTasks.contains(row.entity.id) && removedTasks.contains(row.entity.parentId)) {
                removedTasks.insert(row.entity.id); changedIds.insert(row.entity.id); grew = true;
            }
    }
    QStringList cleanupPaths;
    for (const Attachment &row : m_snapshot.attachments)
        if (removedTasks.contains(row.entityId)) cleanupPaths.append(row.relativePath);
    for (const Attachment &row : changes->attachmentRemovals) cleanupPaths.append(row.relativePath);
    for (const QString &id : changes->groupRemovals)
        for (const StoredEntity &row : m_snapshot.entities) if (row.entity.listId == id) changedIds.insert(row.entity.id);
    for (const QString &id : changes->sectionRemovals)
        for (const StoredEntity &row : m_snapshot.entities) if (row.entity.sectionId == id) changedIds.insert(row.entity.id);
    if (!m_repository.apply(changes, &snapshot, &failure)) return fail(failure);
    // 落库成功之后、快照替换与任何对外信号发出之前的一次性钩子：
    // commitTextDraft 用它确定编辑会话的写入归因，避免信号回调抢先改动会话或实体。
    if (beforeSignals) beforeSignals();
    m_snapshot = snapshot; m_lastError.clear();
    const bool groupChanges = !changes->groupSaves.isEmpty() || !changes->groupRemovals.isEmpty()
        || !changes->folderSaves.isEmpty() || !changes->folderRemovals.isEmpty()
        || !changes->sectionSaves.isEmpty() || !changes->sectionRemovals.isEmpty();
    if (groupChanges) emit groupsChanged();
    for (const QString &id : changedIds) emit taskChanged(id);
    // apply 可追加删除清单等仓储旁路产生的事件，必须按最终 eventSaves 通知。
    QSet<QString> eventEntityIds;
    for (const EntityEvent &event : changes->eventSaves) eventEntityIds.insert(event.entityId);
    for (const QString &id : eventEntityIds) emit eventsChanged(id);
    for (const QString &id : desktopChangedIds) emit desktopNoteChanged(id);
    for (const Attachment &row : changes->attachmentSaves) emit attachmentsChanged(row.entityId);
    for (const Attachment &row : changes->attachmentRemovals) emit attachmentsChanged(row.entityId);
    for (const QString &id : removedTasks) emit attachmentsChanged(id);
    if (reset || !changes->removals.isEmpty() || !changes->generationSourceId.isEmpty() || groupChanges) emit tasksReset();
    if (m_attachmentStore) {
        QStringList orphans;
        for (const QString &path : cleanupPaths) { QString ignored; if (!m_attachmentStore->removeFile(path, &ignored)) orphans.append(path); }
        if (!orphans.isEmpty()) emit errorOccurred(QStringLiteral("附件清理失败，已保留孤儿文件: ") + orphans.join(QStringLiteral(", ")));
    }
    return true;
}

bool TaskStore::commitSnapshotUpdates(const EntitySnapshot &snapshot, const QVector<Entity> &updates)
{
    if (updates.isEmpty()) { m_snapshot = snapshot; m_lastError.clear(); return true; }
    // prepareSave 复用所有既有业务规则，但基于本次最新仓储快照。失败必须恢复
    // store 原内存视图；repository 的事务同时保证实体、关联、事件和 outbox 全回滚。
    const EntitySnapshot before = m_snapshot;
    m_snapshot = snapshot;
    EntityChanges changes;
    for (Entity value : updates) {
        applyDesktopFacade(&value);
        if (!prepareSave(value, &changes, SavePurpose::GroupMove)) { m_snapshot = before; return false; }
    }
    if (!commit(&changes)) { m_snapshot = before; return false; }
    return true;
}

bool TaskStore::prepareSave(Entity value, EntityChanges *changes, SavePurpose purpose,
                            const ScheduleValue* schedule)
{
    const StoredEntity *previous = value.id.isEmpty() ? nullptr : find(value.id);
    if (!value.id.isEmpty() && !previous) return fail(QStringLiteral("任务不存在"));
    const bool inserting = !previous;
    const QDateTime now = utcNow(); QString failure;
    if (inserting) {
        value.id = newUuid();
        if (value.status == TaskStatus::Active && value.completed) value.status = TaskStatus::Completed;
        value.completed = value.status == TaskStatus::Completed;
    } else {
        if (value.kind != previous->entity.kind) return fail(QStringLiteral("实体类型转换必须使用 convertKind"));
        const bool completedChanged = value.completed != previous->entity.completed;
        const bool statusChanged = value.status != previous->entity.status;
        if (completedChanged && !statusChanged) value.status = value.completed ? TaskStatus::Completed : TaskStatus::Active;
        value.completed = value.status == TaskStatus::Completed;
    }
    if (value.listId.isEmpty()) value.listId = inboxListId();
    if (!hasFolder(value.folderId) || !hasGroup(value.listId) || !hasSection(value.sectionId)) return fail(QStringLiteral("文件夹/清单/section 不存在"));
    if (!value.sectionId.isEmpty()) {
        for (const Section &section : m_snapshot.sections) if (section.id == value.sectionId && section.listId != value.listId)
            return fail(QStringLiteral("section 必须属于任务所在清单"));
    }
    const StoredEntity *parent = value.parentId.isEmpty() ? nullptr : find(value.parentId);
    if (!value.parentId.isEmpty() && (!parent || value.parentId == value.id))
        return fail(QStringLiteral("父任务不存在或形成循环"));
    if (parent && !previous) changes->touches.append(*parent);
    if (previous && value.parentId != previous->entity.parentId) {
        if (!previous->entity.parentId.isEmpty()) {
            const StoredEntity *oldParent = find(previous->entity.parentId);
            if (!oldParent) return fail(QStringLiteral("原父任务不存在"));
            changes->touches.append(*oldParent);
        }
        if (parent) changes->touches.append(*parent);
    }
    if (previous && previous->entity.deleted && value.status != previous->entity.status) return fail(QStringLiteral("请先恢复回收站任务再改变状态"));
    if (inserting) value.createdAt = now; else value.createdAt = previous->entity.createdAt;
    value.updatedAt = now;
    value.completedAt = value.completed ? (previous && previous->entity.completed ? previous->entity.completedAt : now) : QDateTime();
    value.abandonedAt = value.status == TaskStatus::Abandoned
        ? (previous && previous->entity.status == TaskStatus::Abandoned ? previous->entity.abandonedAt : now) : QDateTime();
    if (value.deleted != value.deletedAt.isValid()) {
        if (previous && value.deleted == previous->entity.deleted) value.deleted = value.deletedAt.isValid();
        else value.deletedAt = value.deleted ? now : QDateTime();
    }
    if (value.deleted && !value.deletedAt.isValid()) value.deletedAt = now;
    if (!value.deleted) value.deletedAt = QDateTime();
    if (schedule) {
        value.startAt = schedule->startAt.toUTC();
        value.dueAt = schedule->dueAt.toUTC();
        value.allDayDate = schedule->allDayDate;
        value.allDayEndDate = schedule->allDayEndDate;
        value.hasScheduleRange = schedule->hasScheduleRange;
        value.timeZone = schedule->timeZone;
        value.reminders = schedule->reminders;
    }

    RecurrenceSpec recurrence;
    bool repeating = false;
    if (schedule) {
        repeating = schedule->repeating;
        recurrence = schedule->recurrence;
        if (repeating && !isValidRecurrence(recurrence, &failure)) return fail(failure);
    } else {
        const RecurrenceSeries* existing = previous
            ? seriesFor(m_snapshot, previous->entity.recurrenceSeriesId) : nullptr;
        const bool unchanged = existing && value.rrule == previous->entity.rrule
            && value.recurrence == previous->entity.recurrence;
        if (unchanged) {
            repeating = recurrenceFromRuleText(existing->ruleJson, &recurrence, &failure);
            if (!repeating) return fail(failure);
        } else if (!simpleRecurrence(value, &recurrence, &repeating)) {
            return fail(QStringLiteral("重复规则非法；完整规则请使用调度保存接口"));
        }
    }
    if (repeating && !plannedLocalDate(value).isValid())
        return fail(QStringLiteral("重复任务必须有计划时间或全天日期"));
    const QDate localPlan = plannedLocalDate(value);
    const int anchor = localPlan.isValid() ? localPlan.day() : 0;
    const RecurrenceSeries* previousSeries = previous
        ? seriesFor(m_snapshot, previous->entity.recurrenceSeriesId) : nullptr;
    const QString newRuleJson = repeating ? recurrenceRuleText(recurrence) : QString();
    const bool recurrenceRuleChanged = repeating
        && (!previousSeries || previousSeries->ruleJson != newRuleJson);
    const bool scheduleAnchorChanged = schedule && previous
        && (previous->entity.startAt != value.startAt || previous->entity.dueAt != value.dueAt
            || previous->entity.allDayDate != value.allDayDate
            || previous->entity.allDayEndDate != value.allDayEndDate
            || previous->entity.hasScheduleRange != value.hasScheduleRange
            || previous->entity.timeZone != value.timeZone);
    const bool keepSeriesAnchor = previousSeries && !recurrenceRuleChanged && !scheduleAnchorChanged;
    if (repeating) {
        value.recurrenceSeriesId = previousSeries ? previousSeries->id : newUuid();
        if (previous && previous->entity.recurrenceSeriesId == value.recurrenceSeriesId
            && plannedAt(previous->entity) == plannedAt(value)
            && previous->entity.allDayDate == value.allDayDate
            && previous->entity.hasScheduleRange == value.hasScheduleRange)
            value.occurrenceKey = previous->entity.occurrenceKey;
        else value.occurrenceKey = occurrenceKey(value);
        value.rrule = recurrenceFrequencyName(recurrence);
        value.recurrence = recurrenceSummary(recurrence);
        RecurrenceSeries series = previousSeries ? *previousSeries : RecurrenceSeries();
        series.id = value.recurrenceSeriesId;
        series.rrule = value.rrule;
        series.ruleJson = newRuleJson;
        if (recurrenceRuleChanged) {
            series.occurrenceOrdinal = 1;
            series.completionCount = 0;
        }
        series.anchorDay = keepSeriesAnchor ? previousSeries->anchorDay : anchor;
        series.timeZone = value.timeZone;
        if (keepSeriesAnchor) {
            series.dtstartAt = previousSeries->dtstartAt;
            series.dtstartDate = previousSeries->dtstartDate;
        } else {
            series.dtstartAt = value.hasScheduleRange ? value.startAt : value.dueAt;
            series.dtstartDate = value.allDayDate;
        }
        series.updatedAt = now;
        if (series.ruleJson.isEmpty() || !series.createdAt.isValid()) series.createdAt = now;
        const bool changed = !previousSeries || previousSeries->rrule != series.rrule
            || previousSeries->ruleJson != series.ruleJson || previousSeries->anchorDay != series.anchorDay
            || previousSeries->timeZone != series.timeZone || previousSeries->dtstartAt != series.dtstartAt
            || previousSeries->dtstartDate != series.dtstartDate;
        if (changed) queueSeriesSave(changes, series);
    } else {
        value.recurrenceSeriesId.clear(); value.occurrenceKey.clear(); value.rrule.clear();
        value.recurrence = QStringLiteral("none");
    }

    // 单提醒 UI 只编辑最早触发的一条；其他提醒保持不变。
    QString previousPrimaryId;
    if (previous) {
        const int primary = primaryReminderIndex(previous->entity);
        if (primary >= 0) previousPrimaryId = previous->entity.reminders.at(primary).id;
    }
    if (previous && !schedule && value.reminderAt != previous->entity.reminderAt) {
        int editIndex = -1;
        for (int i = 0; i < value.reminders.size(); ++i) if (value.reminders.at(i).id == previousPrimaryId) editIndex = i;
        if (editIndex < 0) editIndex = primaryReminderIndex(value);
        if (!value.reminderAt.isValid()) {
            if (editIndex >= 0) value.reminders.remove(editIndex);
        } else if (editIndex < 0) {
            TaskReminder reminder; reminder.triggerAt = value.reminderAt; value.reminders.append(reminder);
        } else value.reminders[editIndex].triggerAt = value.reminderAt;
    } else if (inserting && value.reminderAt.isValid() && value.reminders.isEmpty()) {
        TaskReminder reminder; reminder.triggerAt = value.reminderAt; value.reminders.append(reminder);
    }
    QHash<QString, TaskReminder> oldReminders;
    if (previous) for (const TaskReminder &row : previous->entity.reminders) oldReminders.insert(row.id, row);
    QSet<QString> reminderIds;
    for (int i = 0; i < value.reminders.size(); ++i) {
        TaskReminder &row = value.reminders[i];
        if (row.id.isEmpty()) row.id = newUuid();
        if (!isUuid(row.id) || reminderIds.contains(row.id) || (!row.entityId.isEmpty() && row.entityId != value.id)
            || (findReminder(row.id) && !oldReminders.contains(row.id))) return fail(QStringLiteral("提醒 id 非法或不属于当前任务"));
        reminderIds.insert(row.id); row.entityId = value.id;
        // 拖放/移清单不编辑提醒，连旧库合法的非连续排序值也必须原样保留。
        row.sortOrder = purpose == SavePurpose::GroupMove && oldReminders.contains(row.id)
            ? oldReminders.value(row.id).sortOrder : i;
        if (!row.triggerAt.isValid()) return fail(QStringLiteral("提醒时间不能为空"));
        if (!row.ruleJson.isEmpty()) {
            ReminderSpec spec;
            if (!reminderFromRuleText(row.ruleJson, &spec, &failure)
                || !resolveReminder(spec, value, &row.triggerAt, &failure)) return fail(failure);
        }
        if (oldReminders.contains(row.id)) {
            row.createdAt = oldReminders.value(row.id).createdAt;
            row.deliveredAt = row.triggerAt == oldReminders.value(row.id).triggerAt
                ? oldReminders.value(row.id).deliveredAt : QDateTime();
            if (!sameReminder(row, oldReminders.value(row.id))) { row.updatedAt = now; changes->reminderSaves.append(row); }
        } else {
            row.deliveredAt = QDateTime(); row.createdAt = now; row.updatedAt = now; changes->reminderSaves.append(row);
        }
    }
    if (previous) for (const TaskReminder &row : previous->entity.reminders)
        if (!reminderIds.contains(row.id)) changes->reminderRemovals.append(row.id);
    const int primary = primaryReminderIndex(value);
    value.reminderAt = primary >= 0 ? value.reminders.at(primary).triggerAt : QDateTime();
    value.notifiedAt = primary >= 0 ? value.reminders.at(primary).deliveredAt : QDateTime();

    QSet<QString> wantedTags, existingTags;
    for (const QString &name : value.tags) if (!name.trimmed().isEmpty()) wantedTags.insert(name.trimmed().toLower());
    if (previous) for (const QString &name : previous->entity.tags) existingTags.insert(name.trimmed().toLower());
    if (wantedTags != existingTags) {
        if (previous) for (const TaskTag &link : m_snapshot.taskTags) if (link.entityId == value.id) changes->taskTagRemovals.append(link);
        QHash<QString, Tag> known;
        for (const Tag &tag : m_snapshot.tags) known.insert(tag.normalizedName, tag);
        for (const QString &name : value.tags) {
            const QString trimmed = name.trimmed(), normalized = trimmed.toLower();
            if (normalized.isEmpty()) continue;
            Tag tag = known.value(normalized);
            if (tag.id.isEmpty()) { tag.id = newUuid(); tag.name = trimmed; tag.normalizedName = normalized; changes->tagSaves.append(tag); known.insert(normalized, tag); }
            bool duplicate = false;
            for (const TaskTag &link : changes->taskTagSaves) if (link.entityId == value.id && link.tagId == tag.id) duplicate = true;
            if (!duplicate) changes->taskTagSaves.append(makeTaskTag(value.id, tag.id));
        }
    }

    StoredEntity stored = previous ? *previous : StoredEntity(); stored.entity = value; stored.revision = previous ? previous->revision : 0;
    stored.recurrenceAnchorDay = anchor; changes->saves.append(stored);

    // 桌面外观可随实体保存；open 只由桌面动作修改。
    DesktopNote current = desktopNote(value.id);
    const bool desktopChanged = current.id.isEmpty()
        ? (value.color != NoteColor::Yellow || value.geometry != QRect() || value.zOrder != 0 || value.layer != NoteLayer::Normal)
        : (value.color != current.color || value.geometry != current.geometry
           || value.zOrder != current.zOrder || value.layer != current.layer);
    if (desktopChanged) {
        const bool creatingDesktop = current.id.isEmpty();
        if (creatingDesktop) {
            current.id = newUuid(); current.entityId = value.id; current.deviceId = deviceId(); current.createdAt = now; current.open = false;
        }
        current.color = value.color; current.geometry = value.geometry; current.zOrder = value.zOrder; current.layer = value.layer;
        current.updatedAt = now; changes->desktopNoteSaves.append(current);
    }

    if (!entitycodec::validate(value, &failure)) return fail(failure);
    const bool wasCompleted = previous && previous->entity.completed;
    if (wasCompleted && !value.completed) {
        const RecurrenceLink *link = linkFor(value.id);
        if (link) {
            if (link->nextId.isEmpty()) {
                // 永久删除中间实例会把前驱 next_id 置空，并删除中间实例的出链。
                // 空链接因此不总是“最后一次完成”；已有更晚实例时不能回退全局进度。
                const QDate currentPlan = plannedLocalDate(value);
                for (const StoredEntity& candidate : m_snapshot.entities) {
                    if (candidate.entity.id == value.id
                        || candidate.entity.recurrenceSeriesId != value.recurrenceSeriesId) continue;
                    const QDate candidatePlan = plannedLocalDate(candidate.entity);
                    bool later = candidate.entity.createdAt > value.createdAt
                        || (candidatePlan.isValid() && currentPlan.isValid() && candidatePlan > currentPlan);
                    if (!later && candidate.entity.createdAt == value.createdAt
                        && candidatePlan == currentPlan) {
                        const QDateTime candidateAt = plannedAt(candidate.entity), currentAt = plannedAt(value);
                        // 同毫秒创建时用计划时刻区分先后；无法比较时保守拒绝，不能损坏全局进度。
                        later = candidateAt.isValid() && currentAt.isValid()
                            ? candidateAt > currentAt : true;
                    }
                    if (later) return fail(QStringLiteral("后续重复实例仍存在，不能安全撤销"));
                }
            }
            if (previousSeries) {
                RecurrenceSeries series = *previousSeries;
                for (const RecurrenceSeries& pending : changes->seriesSaves)
                    if (pending.id == series.id) series = pending;
                series.occurrenceOrdinal = qMax(1, series.occurrenceOrdinal - 1);
                series.completionCount = qMax(0, series.completionCount - 1);
                series.updatedAt = now;
                queueSeriesSave(changes, series);
            }
            if (link->nextId.isEmpty()) {
                changes->removeLinks.append(value.id);
            } else {
            const StoredEntity *next = find(link->nextId);
            if (!next || next->revision != link->nextRevision || next->entity.status != TaskStatus::Active
                || next->entity.deleted || linkFor(next->entity.id))
                return fail(QStringLiteral("下一重复实例已修改、完成、放弃或删除，不能安全撤销"));
            bool hasAttachments = false, hasChildren = false;
            for (const Attachment &attachment : m_snapshot.attachments)
                if (attachment.entityId == next->entity.id) { hasAttachments = true; break; }
            for (const StoredEntity &candidate : m_snapshot.entities)
                if (candidate.entity.parentId == next->entity.id) { hasChildren = true; break; }
            // 附件导入和子任务创建不一定改变 next.entity.revision，必须单独保护关联数据。
            if (hasAttachments || hasChildren)
                return fail(QStringLiteral("下一重复实例已有附件或子任务，不能安全撤销"));
            changes->removeLinks.append(value.id); changes->removals.append(*next);
            }
        }
    } else if (!wasCompleted && value.completed && repeating) {
        if (!previousSeries) return fail(QStringLiteral("请先保存重复规则，再完成任务"));
        if (inserting) return fail(QStringLiteral("请先创建重复任务，再标记完成"));
        if (!linkFor(value.id)) {
            const auto progressSeries = [changes, previousSeries]() {
                RecurrenceSeries result = *previousSeries;
                for (const RecurrenceSeries& candidate : changes->seriesSaves)
                    if (candidate.id == result.id) result = candidate;
                return result;
            };
            const QDateTime completion = value.completedAt.toTimeZone(QTimeZone(value.timeZone.toUtf8()));
            const QDate currentEnd = value.hasScheduleRange
                ? (value.allDayDate.isValid() ? value.allDayEndDate
                   : value.dueAt.toTimeZone(QTimeZone(value.timeZone.toUtf8())).date())
                : localPlan;
            const QDate seriesAnchor = !keepSeriesAnchor ? localPlan
                : previousSeries->dtstartDate.isValid() ? previousSeries->dtstartDate
                : previousSeries->dtstartAt.toTimeZone(QTimeZone(previousSeries->timeZone.toUtf8())).date();
            const RecurrenceAdvance advance = nextRecurrence(
                recurrence, currentEnd, completion.date(),
                recurrenceRuleChanged ? 1 : previousSeries->occurrenceOrdinal,
                recurrenceRuleChanged ? 0 : previousSeries->completionCount, seriesAnchor);
            if (advance.status == RecurrenceStatus::Invalid) return fail(advance.error);
            if (advance.status == RecurrenceStatus::Exhausted) {
                RecurrenceSeries series = progressSeries();
                series.occurrenceOrdinal += 1;
                series.completionCount += 1;
                series.updatedAt = now;
                queueSeriesSave(changes, series);
                RecurrenceLink link;
                link.id = newUuid(); link.sourceId = value.id; link.nextRevision = 1;
                changes->recurrenceLinkSaves.append(link);
            } else if (advance.status == RecurrenceStatus::Next) {
                Entity next = value;
                next.id = newUuid(); next.status = TaskStatus::Active; next.completed = false;
                next.completedAt = QDateTime(); next.abandonedAt = QDateTime(); next.deleted = false; next.deletedAt = QDateTime();
                next.createdAt = now; next.updatedAt = now; next.notifiedAt = QDateTime(); next.parentId.clear();
                const QTimeZone zone(value.timeZone.toUtf8());
                if (value.allDayDate.isValid()) {
                    if (value.hasScheduleRange) {
                        const qint64 span = value.allDayDate.daysTo(value.allDayEndDate);
                        next.allDayEndDate = advance.date;
                        next.allDayDate = advance.date.addDays(-span);
                    } else {
                        next.allDayDate = advance.date;
                        next.allDayEndDate = QDate();
                    }
                    next.startAt = QDateTime(); next.dueAt = QDateTime();
                } else if (value.hasScheduleRange) {
                    const QDateTime oldStart = value.startAt.toTimeZone(zone);
                    const QDateTime oldEnd = value.dueAt.toTimeZone(zone);
                    const QDate startDate = advance.date.addDays(-oldStart.date().daysTo(oldEnd.date()));
                    next.startAt = QDateTime(startDate, oldStart.time(), zone).toUTC();
                    next.dueAt = QDateTime(advance.date, oldEnd.time(), zone).toUTC();
                } else {
                    const QDateTime old = value.dueAt.toTimeZone(zone);
                    next.dueAt = QDateTime(advance.date, old.time(), zone).toUTC();
                    next.startAt = QDateTime();
                }
                if ((!next.allDayDate.isValid() && !next.dueAt.isValid())
                    || !entitycodec::validDate(next.dueAt) || !entitycodec::validDate(next.startAt))
                    return fail(QStringLiteral("下一重复日期超出范围或本地时刻不存在"));
                next.occurrenceKey = occurrenceKey(next);
                for (TaskReminder &reminder : next.reminders) {
                    if (!reminder.ruleJson.isEmpty()) {
                        ReminderSpec spec;
                        if (!reminderFromRuleText(reminder.ruleJson, &spec, &failure)
                            || !resolveReminder(spec, next, &reminder.triggerAt, &failure)) return fail(failure);
                    } else {
                        const qint64 offset = plannedAt(value).msecsTo(reminder.triggerAt);
                        reminder.triggerAt = plannedAt(next).addMSecs(offset);
                    }
                    if (!reminder.triggerAt.isValid() || !entitycodec::validDate(reminder.triggerAt))
                        return fail(QStringLiteral("下一提醒时间超出范围"));
                    reminder.id = newUuid(); reminder.entityId = next.id; reminder.deliveredAt = QDateTime();
                    reminder.createdAt = now; reminder.updatedAt = now; changes->reminderSaves.append(reminder);
                }
                const int nextPrimary = primaryReminderIndex(next);
                next.reminderAt = nextPrimary >= 0 ? next.reminders.at(nextPrimary).triggerAt : QDateTime();
                StoredEntity generated;
                generated.entity = next; generated.revision = 0; generated.recurrenceAnchorDay = anchor;
                changes->generatedTaskIndex = changes->saves.size();
                changes->generationSourceId = value.id; changes->saves.append(generated);
                RecurrenceLink link;
                link.id = newUuid(); link.sourceId = value.id; link.nextId = next.id; link.nextRevision = 1;
                changes->recurrenceLinkSaves.append(link);
                RecurrenceSeries series = progressSeries();
                series.occurrenceOrdinal = advance.occurrenceOrdinal;
                series.completionCount = advance.completionCount;
                series.updatedAt = now;
                queueSeriesSave(changes, series);
                const QVector<TaskTag> sourceTagLinks = changes->taskTagSaves;
                for (const TaskTag &link : sourceTagLinks)
                    if (link.entityId == value.id) changes->taskTagSaves.append(makeTaskTag(next.id, link.tagId));
                if (wantedTags == existingTags) for (const TaskTag &link : m_snapshot.taskTags)
                    if (link.entityId == value.id) changes->taskTagSaves.append(makeTaskTag(next.id, link.tagId));
                // 新 occurrence 的桌面投影不自动复制；附件也保留在原实例。
            }
        }
    }
    recordSaveEvents(previous, value, changes, now);
    if (changes->generatedTaskIndex >= 0 && changes->generatedTaskIndex < changes->saves.size()) {
        const Entity &generated = changes->saves.at(changes->generatedTaskIndex).entity;
        addEvent(changes, generated, QStringLiteral("created"), QString(), QVariant(), createdSnapshot(generated), now);
    }
    return true;
}

void TaskStore::recordSaveEvents(const StoredEntity *previous, const Entity &value, EntityChanges *changes, const QDateTime &now)
{
    if (!previous) {
        addEvent(changes, value, QStringLiteral("created"), QString(), QVariant(), createdSnapshot(value), now);
        // 新子任务也明确记录关联主任务；提交时把父标题写入结构化快照，重命名/删除后仍可读。
        if (!value.parentId.isEmpty())
            addEvent(changes, value, QStringLiteral("changed"), QStringLiteral("parent"), QVariant(), value.parentId, now);
        return;
    }
    const Entity &old = previous->entity;
    const auto scalar = [&](const QString &field, const QVariant &before, const QVariant &after) {
        if (before != after) addEvent(changes, value, QStringLiteral("changed"), field, before, after, now);
    };
    if (old.deleted != value.deleted) {
        addEvent(changes, value, value.deleted ? QStringLiteral("trashed") : QStringLiteral("restored"),
                 QStringLiteral("deleted"), old.deleted ? QStringLiteral("1") : QStringLiteral("0"),
                 value.deleted ? QStringLiteral("1") : QStringLiteral("0"), now);
    }
    scalar(QStringLiteral("status"), taskStatusName(old.status), taskStatusName(value.status));
    scalar(QStringLiteral("due"), msValue(old.dueAt), msValue(value.dueAt));
    scalar(QStringLiteral("start"), msValue(old.startAt), msValue(value.startAt));
    scalar(QStringLiteral("allDayDate"), old.allDayDate.isValid() ? QVariant(old.allDayDate.toString(Qt::ISODate)) : QVariant(),
           value.allDayDate.isValid() ? QVariant(value.allDayDate.toString(Qt::ISODate)) : QVariant());
    scalar(QStringLiteral("allDayEndDate"), old.allDayEndDate.isValid() ? QVariant(old.allDayEndDate.toString(Qt::ISODate)) : QVariant(),
           value.allDayEndDate.isValid() ? QVariant(value.allDayEndDate.toString(Qt::ISODate)) : QVariant());
    scalar(QStringLiteral("hasScheduleRange"), old.hasScheduleRange ? QStringLiteral("1") : QStringLiteral("0"),
           value.hasScheduleRange ? QStringLiteral("1") : QStringLiteral("0"));
    scalar(QStringLiteral("priority"), QString::number(old.priority), QString::number(value.priority));
    scalar(QStringLiteral("list"), idValue(old.listId), idValue(value.listId));
    scalar(QStringLiteral("recurrence"), old.recurrence, value.recurrence);
    const RecurrenceSeries* oldSeries = seriesFor(m_snapshot, old.recurrenceSeriesId);
    QString newRule = value.recurrenceSeriesId.isEmpty() ? QString()
        : oldSeries ? oldSeries->ruleJson : QString();
    for (const RecurrenceSeries& series : changes->seriesSaves)
        if (series.id == value.recurrenceSeriesId) newRule = series.ruleJson;
    scalar(QStringLiteral("recurrenceRule"), oldSeries ? oldSeries->ruleJson : QString(), newRule);
    scalar(QStringLiteral("parent"), idValue(old.parentId), idValue(value.parentId));
    scalar(QStringLiteral("timeZone"), old.timeZone, value.timeZone);
    const auto normalizedTags = [](const QStringList &tags) {
        QStringList result; for (const QString &tag : tags) if (!tag.trimmed().isEmpty()) result.append(tag.trimmed().toLower());
        result.sort(); return result;
    };
    if (normalizedTags(old.tags) != normalizedTags(value.tags)) scalar(QStringLiteral("tags"), tagSnapshot(old.tags), tagSnapshot(value.tags));

    QHash<QString, TaskReminder> oldReminders;
    for (const TaskReminder &row : old.reminders) oldReminders.insert(row.id, row);
    QSet<QString> currentReminderIds;
    for (const TaskReminder &row : value.reminders) {
        currentReminderIds.insert(row.id);
        const auto before = oldReminders.constFind(row.id);
        if (before == oldReminders.constEnd())
            addEvent(changes, value, QStringLiteral("reminder_added"), QStringLiteral("reminder"), QVariant(), row.id, now);
        else if (before->triggerAt != row.triggerAt)
            addEvent(changes, value, QStringLiteral("changed"), QStringLiteral("reminder.%1").arg(row.id),
                     msValue(before->triggerAt), msValue(row.triggerAt), now);
    }
    for (const TaskReminder &row : old.reminders) if (!currentReminderIds.contains(row.id))
        addEvent(changes, value, QStringLiteral("reminder_removed"), QStringLiteral("reminder"), row.id, QVariant(), now);

    // 只有携带有效会话归因的 commitTextDraft 保存才抑制直存 title/body 事件（其差异由该会话
    // 结束/取消时按“只记自己真正写入的字段”补账）。其他任何写入——包括存在其他活动会话时的
    // saveTask 直存、无会话/跨实体会话的文本提交——都必须立即记账，否则事件会凭空丢失。
    if (!m_textCommitSave)
        changes->eventSaves.append(m_journal.eventsForDirectSave(old, value, now));
}

QString TaskStore::createTask(const QString &title, const QString &listId, const QString &sectionId, const QString &parentId)
{
    if (!ensureLoaded()) return QString();
    Entity value; value.kind = EntityKind::Task; value.title = title;
    value.listId = listId; value.sectionId = sectionId; value.parentId = parentId;
    EntityChanges changes;
    if (!prepareSave(value, &changes)) return QString();
    if (!commit(&changes, true)) return QString();
    return changes.saves.first().entity.id;
}
QString TaskStore::createSubtask(const QString &title, const QString &parentId)
{
    if (!ensureLoaded()) return QString();
    // 父项必须从当前快照重读：存在、未删除、active 的任务；笔记/完成/放弃/回收站一律拒绝。
    const StoredEntity *parentStored = parentId.isEmpty() ? nullptr : find(parentId);
    if (!parentStored) { fail(QStringLiteral("父任务不存在")); return QString(); }
    const Entity &parent = parentStored->entity;
    if (parent.kind != EntityKind::Task || parent.status != TaskStatus::Active || parent.deleted) {
        fail(QStringLiteral("父任务必须是未删除的进行中任务"));
        return QString();
    }
    Entity value;
    value.kind = EntityKind::Task;
    value.title = title;
    value.listId = parent.listId; value.sectionId = parent.sectionId; value.parentId = parent.id;
    // 只继承日期语义：时间视图（今天/明天/最近7天/已过期）按 effectiveDate 聚合，
    // 复制 allDayDate/startAt/dueAt/timeZone 让子任务与父项同桶立即可见。
    value.allDayDate = parent.allDayDate; value.allDayEndDate = parent.allDayEndDate;
    value.hasScheduleRange = parent.hasScheduleRange;
    value.startAt = parent.startAt; value.dueAt = parent.dueAt;
    value.timeZone = parent.timeZone;
    // 不继承置顶、提醒、重复/完成状态：pinnedAt/reminders/rrule 保持新任务默认空值。
    qint64 order = 0;
    for (const StoredEntity &row : m_snapshot.entities)
        if (row.entity.parentId == parent.id) order = qMax(order, row.entity.sortOrder + 1);
    value.sortOrder = order;                                    // 同级末尾 max+1
    EntityChanges changes;
    if (!prepareSave(value, &changes)) return QString();        // 父子校验由 prepareSave 复用
    if (!commit(&changes, true)) return QString();
    return changes.saves.first().entity.id;
}
QString TaskStore::createNote(const QString &title, const QString &listId, const QString &sectionId)
{
    if (!ensureLoaded()) return QString();
    Entity value; value.kind = EntityKind::Note; value.title = title; value.listId = listId; value.sectionId = sectionId;
    EntityChanges changes;
    if (!prepareSave(value, &changes)) return QString();
    if (!commit(&changes, true)) return QString();
    return changes.saves.first().entity.id;
}
bool TaskStore::saveTask(Entity value)
{
    if (!ensureLoaded()) return false;
    EntityChanges changes; const bool inserting = value.id.isEmpty();
    return prepareSave(value, &changes) && commit(&changes, inserting);
}
bool TaskStore::saveSchedule(Entity value, const ScheduleValue& schedule, QString* savedId)
{
    if (savedId) savedId->clear();
    if (!ensureLoaded()) return false;
    EntityChanges changes;
    const bool inserting = value.id.isEmpty();
    if (!prepareSave(value, &changes, SavePurpose::Edit, &schedule) || !commit(&changes, inserting))
        return false;
    if (savedId && !changes.saves.isEmpty()) *savedId = changes.saves.first().entity.id;
    return true;
}
bool TaskStore::recurrenceSpec(const QString& entityId, RecurrenceSpec* spec) const
{
    const StoredEntity* stored = find(entityId);
    const RecurrenceSeries* series = stored ? seriesFor(m_snapshot, stored->entity.recurrenceSeriesId) : nullptr;
    return series && spec && recurrenceFromRuleText(series->ruleJson, spec);
}
ScheduleValue TaskStore::schedule(const QString& entityId) const
{
    const StoredEntity* stored = find(entityId);
    if (!stored) return ScheduleValue();
    ScheduleValue result = scheduleFromEntity(stored->entity);
    RecurrenceSpec spec;
    result.repeating = recurrenceSpec(entityId, &spec);
    if (result.repeating) result.recurrence = spec;
    return result;
}
bool TaskStore::convertKind(const QString &id, EntityKind kind)
{
    if (!ensureLoaded()) return false;
    if (!find(id)) return fail(QStringLiteral("实体不存在"));
    const bool changed = find(id)->entity.kind != kind;
    EntitySnapshot after; QString failure;
    if (!m_repository.convertKind(id, kind, &after, &failure)) return fail(failure);
    m_snapshot = after; m_lastError.clear();
    emit taskChanged(id); emit tasksReset();
    if (changed) emit eventsChanged(id);
    return true;
}
QString TaskStore::cloneTask(const QString &id)
{
    if (!ensureLoaded()) return QString();
    const StoredEntity *source = find(id);
    if (!source) { fail(QStringLiteral("任务不存在")); return QString(); }
    Entity value = source->entity;
    value.id.clear(); value.parentId.clear();
    value.status = TaskStatus::Active; value.completed = false; value.deleted = false;
    value.createdAt = QDateTime(); value.updatedAt = QDateTime(); value.completedAt = QDateTime();
    value.abandonedAt = QDateTime(); value.deletedAt = QDateTime();
    value.recurrenceSeriesId.clear(); value.occurrenceKey.clear(); value.rrule.clear(); value.recurrence = QStringLiteral("none");
    value.reminders.clear(); value.reminderAt = QDateTime(); value.notifiedAt = QDateTime();
    value.color = NoteColor::Yellow; value.geometry = QRect(); value.zOrder = 0; value.layer = NoteLayer::Normal;
    EntityChanges changes;
    if (!prepareSave(value, &changes) || !commit(&changes, true)) return QString();
    return changes.saves.first().entity.id;
}
bool TaskStore::setCompleted(const QString &id, bool completed)
{
    if (!ensureLoaded()) return false; Entity value = entity(id);
    if (value.id.isEmpty()) return fail(QStringLiteral("任务不存在"));
    if (value.completed == completed) { m_lastError.clear(); return true; }
    value.completed = completed; return saveTask(value);
}
bool TaskStore::setStatus(const QString &id, TaskStatus status)
{
    if (!ensureLoaded()) return false; Entity value = entity(id);
    if (value.id.isEmpty()) return fail(QStringLiteral("任务不存在"));
    if (value.status == status) return true; value.status = status; return saveTask(value);
}
bool TaskStore::abandonTask(const QString &id) { return setStatus(id, TaskStatus::Abandoned); }
bool TaskStore::setPinnedAt(const QString &id, const QDateTime &pinnedAt)
{
    if (!ensureLoaded()) return false;
    const StoredEntity *stored = find(id);
    if (!stored) return fail(QStringLiteral("实体不存在"));
    const QDateTime normalized = pinnedAt.isValid() ? pinnedAt.toUTC() : QDateTime();
    if (!entitycodec::validDate(normalized)) return fail(QStringLiteral("置顶时间超出有效范围"));
    if (stored->entity.pinnedAt == normalized) { m_lastError.clear(); return true; }
    StoredEntity updated = *stored; updated.entity.pinnedAt = normalized; updated.entity.updatedAt = utcNow();
    EntityChanges changes; changes.saves.append(updated);
    return commit(&changes);
}
bool TaskStore::trashTask(const QString &id)
{
    if (!ensureLoaded()) return false; Entity value = entity(id);
    if (value.id.isEmpty()) return fail(QStringLiteral("任务不存在"));
    if (value.deleted) return true; value.deleted = true; return saveTask(value);
}
bool TaskStore::restoreTask(const QString &id)
{
    if (!ensureLoaded()) return false; Entity value = entity(id);
    if (value.id.isEmpty()) return fail(QStringLiteral("任务不存在"));
    if (!value.deleted) return true; value.deleted = false; return saveTask(value);
}
bool TaskStore::deletePermanently(const QString &id)
{
    if (!ensureLoaded()) return false; const StoredEntity *stored = find(id);
    if (!stored) return fail(QStringLiteral("任务不存在"));
    if (!stored->entity.deleted) return fail(QStringLiteral("请先把任务移到回收站"));
    EntityChanges changes; changes.removals.append(*stored);
    QSet<QString> deletedIds; deletedIds.insert(id);
    bool grew = true;
    while (grew) {
        grew = false;
        for (const StoredEntity &candidate : m_snapshot.entities)
            if (!deletedIds.contains(candidate.entity.id) && deletedIds.contains(candidate.entity.parentId)) {
                deletedIds.insert(candidate.entity.id); grew = true;
            }
    }
    const QDateTime now = utcNow();
    for (const StoredEntity &candidate : m_snapshot.entities) if (deletedIds.contains(candidate.entity.id))
        addEvent(&changes, candidate.entity, QStringLiteral("deleted"), QString(), candidate.entity.id, QVariant(), now);
    if (!stored->entity.parentId.isEmpty()) {
        const StoredEntity *parent = find(stored->entity.parentId);
        if (parent) changes.touches.append(*parent);
    }
    return commit(&changes, true);
}

int TaskStore::nextListSortOrder(const QString &folderId) const
{
    int order = 0;
    for (const TaskList &row : m_snapshot.lists) {
        if (row.folderId != folderId) continue;
        order = qMax(order, row.sortOrder + 1);
    }
    // 顶层清单与文件夹共用排序序列：新建落到两者最大值之后，UI 合并展示不会插进文件夹中间。
    if (folderId.isEmpty())
        for (const Folder &row : m_snapshot.folders) order = qMax(order, row.sortOrder + 1);
    return order;
}
int TaskStore::nextFolderSortOrder() const
{
    int order = 0;
    for (const Folder &row : m_snapshot.folders) order = qMax(order, row.sortOrder + 1);
    for (const TaskList &row : m_snapshot.lists) if (row.folderId.isEmpty()) order = qMax(order, row.sortOrder + 1);
    return order;
}

QString TaskStore::createFolder(const QString &name, const QString &icon)
{
    if (!ensureLoaded()) return QString();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kListNameMaxLength)
        { fail(QStringLiteral("文件夹名称必须为 1..%1 个字符").arg(kListNameMaxLength)); return QString(); }
    if (!isValidListIcon(icon)) { fail(QStringLiteral("文件夹图标超出长度限制")); return QString(); }
    const QDateTime now = utcNow(); Folder row; row.id = newUuid(); row.name = trimmed; row.icon = icon;
    row.sortOrder = nextFolderSortOrder(); row.createdAt = now; row.updatedAt = now;
    EntityChanges changes; changes.folderSaves.append(row); return commit(&changes) ? row.id : QString();
}
bool TaskStore::renameFolder(const QString &id, const QString &name)
{
    if (!ensureLoaded()) return false;
    if (id.isEmpty() || !hasFolder(id)) return fail(QStringLiteral("文件夹不存在"));
    for (const Folder &row : m_snapshot.folders) if (row.id == id) {
        Folder updated = row;
        updated.name = name; updated.updatedAt = utcNow();
        return saveFolder(updated);
    } return fail(QStringLiteral("文件夹不存在"));
}
bool TaskStore::saveFolder(const Folder &folder)
{
    if (!ensureLoaded()) return false;
    const Folder *existing = nullptr;
    for (const Folder &row : m_snapshot.folders) if (row.id == folder.id) existing = &row;
    if (folder.id.isEmpty() || !existing) return fail(QStringLiteral("文件夹不存在"));
    // 旧草稿必须基于本 store 的当前快照；repository 还会核对实际数据库 revision。
    if (folder.revision != existing->revision)
        return fail(QStringLiteral("文件夹已被其他操作修改，请重新加载"));
    Folder row = *existing;
    row.name = folder.name.trimmed();
    row.icon = folder.icon;
    row.sortOrder = folder.sortOrder;
    if (row.name.isEmpty()) return fail(QStringLiteral("文件夹名称不能为空"));
    if (row.name.size() > kListNameMaxLength)
        return fail(QStringLiteral("文件夹名称必须为 1..%1 个字符").arg(kListNameMaxLength));
    if (!isValidListIcon(row.icon)) return fail(QStringLiteral("文件夹图标超出长度限制"));
    if (row.sortOrder < 0) return fail(QStringLiteral("文件夹排序非法"));
    if (row.name == existing->name && row.icon == existing->icon && row.sortOrder == existing->sortOrder) {
        m_lastError.clear(); return true;
    }
    row.updatedAt = utcNow();
    EntityChanges changes; changes.folderSaves.append(row); return commit(&changes);
}
bool TaskStore::moveList(const QString &sourceId, const QString &targetFolderId,
                         const QString &targetListId, bool after)
{
    if (!ensureLoaded()) return false;
    // source/target 都从当前快照重读；陈旧引用直接拒绝，不落任何写入。
    const TaskList *source = findList(sourceId);
    if (sourceId.isEmpty() || !source) return fail(QStringLiteral("清单不存在"));
    if (source->inbox) return fail(QStringLiteral("系统 inbox 不可移动"));
    if (source->archivedAtUtcMs > 0) return fail(QStringLiteral("已归档清单不可移动"));
    if (!targetFolderId.isEmpty() && !hasFolder(targetFolderId)) return fail(QStringLiteral("目标文件夹不存在"));
    if (!targetListId.isEmpty()) {
        const TaskList *anchor = findList(targetListId);
        if (!anchor) return fail(QStringLiteral("目标清单不存在"));
        if (anchor->inbox) return fail(QStringLiteral("不能以系统 inbox 作为移动目标"));
        if (anchor->archivedAtUtcMs > 0) return fail(QStringLiteral("不能以已归档清单作为移动目标"));
        if (anchor->folderId != targetFolderId) return fail(QStringLiteral("目标清单不在目标文件夹内"));
        if (anchor->id == source->id) return fail(QStringLiteral("不能相对自身移动清单"));
    }
    const QString sourceFolderId = source->folderId;
    const QDateTime now = utcNow();
    EntityChanges changes;
    bool changed = false;
    // 混合序列按目标位置整体写入：每行（清单或文件夹）只出现一次，避免同批 CAS 重复保存。
    const auto saveTopLevel = [&now, &changes, &changed](const QVector<TopLevelRow> &rows) {
        for (int i = 0; i < rows.size(); ++i) {
            const TopLevelRow &row = rows.at(i);
            if (row.list) {
                if (row.list->folderId.isEmpty() && row.list->sortOrder == i) continue;
                TaskList value = *row.list;
                value.folderId.clear(); value.sortOrder = i; value.updatedAt = now;
                changes.groupSaves.append(value); changed = true;
            } else {
                if (row.folder->sortOrder == i) continue;
                Folder value = *row.folder;
                value.sortOrder = i; value.updatedAt = now;
                changes.folderSaves.append(value); changed = true;
            }
        }
    };
    if (targetFolderId.isEmpty()) {
        // 顶层是“平铺清单 + 文件夹”混合序列：去掉 source 后按锚点位置（或末尾）重插，整体规范为 0..n-1。
        QVector<TopLevelRow> merged;
        for (const TaskList &row : m_snapshot.lists)
            if (row.folderId.isEmpty() && row.id != sourceId) merged.append(TopLevelRow{&row, nullptr});
        for (const Folder &row : m_snapshot.folders) merged.append(TopLevelRow{nullptr, &row});
        std::sort(merged.begin(), merged.end(), topLevelLess);
        int insertAt = merged.size();                       // 目标空 = 混合序列末尾
        if (!targetListId.isEmpty())
            for (int i = 0; i < merged.size(); ++i)
                if (merged.at(i).list && merged.at(i).list->id == targetListId) insertAt = after ? i + 1 : i;
        merged.insert(insertAt, TopLevelRow{source, nullptr});
        saveTopLevel(merged);
    } else {
        // 文件夹容器是纯清单序列：排序后重插 source 并规范为 0..n-1。
        QVector<TaskList> destination;
        for (const TaskList &row : m_snapshot.lists) if (row.folderId == targetFolderId && row.id != sourceId) destination.append(row);
        std::sort(destination.begin(), destination.end(), [](const TaskList &a, const TaskList &b) {
            if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
            return a.id < b.id;
        });
        int insertAt = destination.size();                   // 目标空 = 容器末尾
        if (!targetListId.isEmpty())
            for (int i = 0; i < destination.size(); ++i) if (destination.at(i).id == targetListId) insertAt = after ? i + 1 : i;
        destination.insert(insertAt, *source);
        for (int i = 0; i < destination.size(); ++i) {
            const TaskList &row = destination.at(i);
            if (row.folderId == targetFolderId && row.sortOrder == i) continue;   // 位置未变就不写
            TaskList value = row;
            value.folderId = targetFolderId; value.sortOrder = i; value.updatedAt = now;
            changes.groupSaves.append(value); changed = true;
        }
    }
    // 跨容器移动后源容器同样规范化：顶层规范化混合序列（不含 source），文件夹规范化剩余清单。
    if (sourceFolderId != targetFolderId) {
        if (sourceFolderId.isEmpty()) {
            QVector<TopLevelRow> merged;
            for (const TaskList &row : m_snapshot.lists)
                if (row.folderId.isEmpty() && row.id != sourceId) merged.append(TopLevelRow{&row, nullptr});
            for (const Folder &row : m_snapshot.folders) merged.append(TopLevelRow{nullptr, &row});
            std::sort(merged.begin(), merged.end(), topLevelLess);
            saveTopLevel(merged);
        } else {
            QVector<TaskList> remaining;
            for (const TaskList &row : m_snapshot.lists) if (row.folderId == sourceFolderId && row.id != sourceId) remaining.append(row);
            std::sort(remaining.begin(), remaining.end(), [](const TaskList &a, const TaskList &b) {
                if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
                return a.id < b.id;
            });
            for (int i = 0; i < remaining.size(); ++i) {
                if (remaining.at(i).sortOrder == i) continue;
                TaskList row = remaining.at(i);
                row.sortOrder = i; row.updatedAt = now;
                changes.groupSaves.append(row); changed = true;
            }
        }
    }
    if (!changed) { m_lastError.clear(); return true; }
    return commit(&changes);
}
QString TaskStore::createFolderFromLists(const QString &name, const QString &icon,
                                         const QString &sourceId, const QString &targetId)
{
    if (!ensureLoaded()) return QString();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kListNameMaxLength)
        { fail(QStringLiteral("文件夹名称必须为 1..%1 个字符").arg(kListNameMaxLength)); return QString(); }
    if (!isValidListIcon(icon)) { fail(QStringLiteral("文件夹图标超出长度限制")); return QString(); }
    const TaskList *source = findList(sourceId);
    const TaskList *target = findList(targetId);
    if (sourceId.isEmpty() || !source) { fail(QStringLiteral("源清单不存在")); return QString(); }
    if (source->inbox) { fail(QStringLiteral("系统 inbox 不可移动")); return QString(); }
    if (source->archivedAtUtcMs > 0) { fail(QStringLiteral("已归档清单不可移动")); return QString(); }
    if (targetId.isEmpty() || !target) { fail(QStringLiteral("目标清单不存在")); return QString(); }
    if (target->inbox) { fail(QStringLiteral("目标必须是普通清单")); return QString(); }
    if (target->archivedAtUtcMs > 0) { fail(QStringLiteral("目标必须是未归档清单")); return QString(); }
    if (!target->folderId.isEmpty()) { fail(QStringLiteral("目标必须是平铺清单")); return QString(); }
    if (source->id == target->id) { fail(QStringLiteral("源清单与目标清单不能相同")); return QString(); }
    const QString sourceFolderId = source->folderId;
    const QDateTime now = utcNow();
    Folder folder;
    folder.id = newUuid(); folder.name = trimmed; folder.icon = icon;
    folder.createdAt = now; folder.updatedAt = now;         // sortOrder 由混合序列分配
    // 顶层混合序列（平铺清单 + 文件夹）在同一事务内整体规范化：
    // 新文件夹精确占据 target 原来的 UI 位置；source 在顶层且位于 target 之前时先移除，位置前移一格。
    QVector<TopLevelRow> full;
    for (const TaskList &row : m_snapshot.lists)
        if (row.folderId.isEmpty()) full.append(TopLevelRow{&row, nullptr});
    for (const Folder &row : m_snapshot.folders) full.append(TopLevelRow{nullptr, &row});
    std::sort(full.begin(), full.end(), topLevelLess);
    int targetIndex = -1, sourceIndex = -1;
    for (int i = 0; i < full.size(); ++i) {
        if (full.at(i).list && full.at(i).list->id == targetId) targetIndex = i;
        if (sourceFolderId.isEmpty() && full.at(i).list && full.at(i).list->id == sourceId) sourceIndex = i;
    }
    const int folderIndex = targetIndex - (sourceIndex >= 0 && sourceIndex < targetIndex ? 1 : 0);
    QVector<TopLevelRow> merged;
    for (int i = 0; i < full.size(); ++i) {
        if (i == targetIndex || i == sourceIndex) continue;
        merged.append(full.at(i));
    }
    merged.insert(folderIndex, TopLevelRow{nullptr, &folder});
    EntityChanges changes;
    for (int i = 0; i < merged.size(); ++i) {
        const TopLevelRow &row = merged.at(i);
        if (row.list) {
            if (row.list->sortOrder == i) continue;          // 顶层清单 folderId 已空，位置未变就不写
            TaskList value = *row.list;
            value.sortOrder = i; value.updatedAt = now;
            changes.groupSaves.append(value);
        } else if (row.folder->id == folder.id) {
            folder.sortOrder = i;                            // 新文件夹占位，循环外一次入队
        } else if (row.folder->sortOrder != i) {
            Folder value = *row.folder;
            value.sortOrder = i; value.updatedAt = now;
            changes.folderSaves.append(value);
        }
    }
    changes.folderSaves.append(folder);                      // 新文件夹一次 INSERT，不重复保存
    // 两清单进入新文件夹，保持原相对顺序（混合序列比较器口径）。
    TaskList first = topLevelLess(TopLevelRow{source, nullptr}, TopLevelRow{target, nullptr}) ? *source : *target;
    TaskList second = first.id == source->id ? *target : *source;
    first.folderId = folder.id; first.sortOrder = 0; first.updatedAt = now;
    second.folderId = folder.id; second.sortOrder = 1; second.updatedAt = now;
    changes.groupSaves.append(first); changes.groupSaves.append(second);
    // source 原来在文件夹里时，剩余清单规范化。
    if (!sourceFolderId.isEmpty()) {
        QVector<TaskList> remaining;
        for (const TaskList &row : m_snapshot.lists) if (row.folderId == sourceFolderId && row.id != sourceId) remaining.append(row);
        std::sort(remaining.begin(), remaining.end(), [](const TaskList &a, const TaskList &b) {
            if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
            return a.id < b.id;
        });
        for (int i = 0; i < remaining.size(); ++i) {
            if (remaining.at(i).sortOrder == i) continue;
            TaskList row = remaining.at(i);
            row.sortOrder = i; row.updatedAt = now;
            changes.groupSaves.append(row);
        }
    }
    return commit(&changes) ? folder.id : QString();
}
bool TaskStore::deleteFolder(const QString &id)
{
    if (!ensureLoaded()) return false; if (!hasFolder(id) || id.isEmpty()) return fail(QStringLiteral("文件夹不存在"));
    EntityChanges changes; changes.folderRemovals.append(id); return commit(&changes, true);
}
QString TaskStore::createList(const QString &name, const QString &folderId, EntityKind kind,
                              const QString &icon, const QString &color)
{
    if (!ensureLoaded()) return QString(); if (!hasFolder(folderId)) { fail(QStringLiteral("文件夹不存在")); return QString(); }
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kListNameMaxLength)
        { fail(QStringLiteral("清单名称必须为 1..%1 个字符").arg(kListNameMaxLength)); return QString(); }
    if (!isValidListIcon(icon)) { fail(QStringLiteral("清单图标超出长度限制")); return QString(); }
    if (!isValidListColor(color)) { fail(QStringLiteral("清单颜色必须为空或 #RRGGBB")); return QString(); }
    const QDateTime now = utcNow(); TaskList row; row.id = newUuid(); row.folderId = folderId; row.kind = kind;
    row.name = trimmed; row.icon = icon; row.color = color; row.sortOrder = nextListSortOrder(folderId);
    row.createdAt = now; row.updatedAt = now;
    // 名称+folder+kind+icon+color+容器尾部 sortOrder 在同一事务内创建，UI 无需 create 后二次 saveList。
    EntityChanges changes; changes.groupSaves.append(row); return commit(&changes) ? row.id : QString();
}
bool TaskStore::renameList(const QString &id, const QString &name)
{
    if (!ensureLoaded()) return false;
    if (id.isEmpty() || !hasGroup(id)) return fail(QStringLiteral("清单不存在"));
    for (TaskList row : m_snapshot.lists) if (row.id == id) {
        row.name = name; row.updatedAt = utcNow(); EntityChanges changes; changes.groupSaves.append(row); return commit(&changes);
    } return fail(QStringLiteral("清单不存在"));
}
bool TaskStore::setListFolder(const QString &id, const QString &folderId)
{
    // 转调 moveList：进入目标容器即分配容器末尾顺序，不保留可能与既有行碰撞的旧 sortOrder。
    return moveList(id, folderId);
}
bool TaskStore::deleteList(const QString &id)
{
    if (!ensureLoaded()) return false; if (id.isEmpty() || !hasGroup(id)) return fail(QStringLiteral("清单不存在"));
    for (const TaskList &list : m_snapshot.lists) if (list.id == id && list.inbox) return fail(QStringLiteral("系统 inbox 不可删除"));
    EntityChanges changes; changes.groupRemovals.append(id); return commit(&changes, true);
}
bool TaskStore::updateList(const QString &id, const QString &name, const QString &icon, const QString &color,
                           const QString &folderId)
{
    if (!ensureLoaded()) return false;
    const TaskList *existing = findList(id);
    if (id.isEmpty() || !existing) return fail(QStringLiteral("清单不存在"));
    if (existing->inbox) return fail(QStringLiteral("系统 inbox 不支持外观编辑"));
    if (!hasFolder(folderId)) return fail(QStringLiteral("文件夹不存在"));
    TaskList row = *existing;
    row.name = name; row.icon = icon; row.color = color;
    row.folderId = folderId;
    return saveList(row);
}
bool TaskStore::saveList(const TaskList &list)
{
    if (!ensureLoaded()) return false;
    const TaskList *existing = findList(list.id);
    if (!existing) return fail(QStringLiteral("清单不存在"));
    if (existing->inbox) return fail(QStringLiteral("系统 inbox 不支持外观编辑"));
    if (!list.folderId.isEmpty() && !hasFolder(list.folderId)) return fail(QStringLiteral("文件夹不存在"));
    // 旧草稿必须基于本 store 的当前快照；repository 还会核对实际数据库 revision。
    if (list.revision != existing->revision)
        return fail(QStringLiteral("清单已被其他操作修改，请重新加载"));
    TaskList row = *existing;                    // kind/inbox/归档状态以当前快照为准，调用方不可改
    row.name = list.name.trimmed();
    row.icon = list.icon; row.color = list.color;
    row.folderId = list.folderId; row.sortOrder = list.sortOrder;
    if (row.name.isEmpty()) return fail(QStringLiteral("清单名称不能为空"));
    if (row.name.size() > kListNameMaxLength)
        return fail(QStringLiteral("清单名称必须为 1..%1 个字符").arg(kListNameMaxLength));
    if (!isValidListIcon(row.icon)) return fail(QStringLiteral("清单图标超出长度限制"));
    if (!isValidListColor(row.color)) return fail(QStringLiteral("清单颜色必须为空或 #RRGGBB"));
    if (row.name == existing->name && row.icon == existing->icon && row.color == existing->color
        && row.folderId == existing->folderId && row.sortOrder == existing->sortOrder) {
        m_lastError.clear(); return true;
    }
    row.updatedAt = utcNow();
    EntityChanges changes; changes.groupSaves.append(row); return commit(&changes);
}
bool TaskStore::setListArchived(const QString &id, bool archived)
{
    if (!ensureLoaded()) return false;
    const TaskList *existing = findList(id);
    if (id.isEmpty() || !existing) return fail(QStringLiteral("清单不存在"));
    if (existing->inbox) return fail(QStringLiteral("收集箱不可归档"));
    const bool currentlyArchived = existing->archivedAtUtcMs > 0;
    if (currentlyArchived == archived) { m_lastError.clear(); return true; }
    const QDateTime now = utcNow();
    TaskList row = *existing;
    row.archivedAtUtcMs = archived ? utcMs(now) : 0;
    row.updatedAt = now;
    // 实体状态和日期完全不动；清单归档与各实体活动事件必须在同一事务提交。
    EntityChanges changes; changes.groupSaves.append(row);
    QJsonObject context; context.insert(QStringLiteral("listName"), row.name);
    for (const StoredEntity &stored : m_snapshot.entities) if (stored.entity.listId == id)
        addEvent(&changes, stored.entity, QStringLiteral("changed"), QStringLiteral("listArchived"),
                 currentlyArchived ? QStringLiteral("1") : QStringLiteral("0"),
                 archived ? QStringLiteral("1") : QStringLiteral("0"), now, context);
    return commit(&changes);
}
bool TaskStore::isListArchived(const QString &id) const
{
    const TaskList *list = findList(id);
    return list && list->archivedAtUtcMs > 0;
}
QSet<QString> TaskStore::archivedListIds() const
{
    QSet<QString> ids;
    for (const TaskList &row : m_snapshot.lists) if (row.archivedAtUtcMs > 0) ids.insert(row.id);
    return ids;
}
bool TaskStore::postponeOverdueTasks(const QStringList &ids, const QDate &today)
{
    if (!ensureLoaded()) return false;
    if (!today.isValid()) return fail(QStringLiteral("顺延日期非法"));
    QSet<QString> archived = archivedListIds();
    EntityChanges changes;
    int postponed = 0;
    for (const QString &id : ids) {
        const StoredEntity *stored = find(id);
        if (!stored) return fail(QStringLiteral("任务不存在"));
        const Entity &task = stored->entity;
        if (task.kind != EntityKind::Task || task.status != TaskStatus::Active || task.deleted) continue;
        if (archived.contains(task.listId)) continue;
        const QDate date = effectiveDate(task);
        if (!date.isValid() || date >= today) continue;
        Entity value = task;
        const qint64 days = date.daysTo(today);
        const auto shiftDate = [days, &value](const QDateTime &when) {
            if (!when.isValid()) return QDateTime();
            return when.toTimeZone(QTimeZone(value.timeZone.toUtf8())).addDays(days).toUTC();
        };
        if (value.allDayDate.isValid()) value.allDayDate = value.allDayDate.addDays(days);
        if (value.allDayEndDate.isValid()) value.allDayEndDate = value.allDayEndDate.addDays(days);
        if (value.dueAt.isValid()) value.dueAt = shiftDate(value.dueAt);
        if (value.startAt.isValid()) value.startAt = shiftDate(value.startAt);
        if (!prepareSave(value, &changes)) return false;               // 事务内统一提交，失败无部分成功
        ++postponed;
    }
    if (postponed == 0) { m_lastError.clear(); return true; }
    return commit(&changes);
}
bool TaskStore::applyDropIntent(const TaskDrop::Intent &intent)
{
    if (!ensureLoaded()) return false;
    if (intent.mode == GroupMode::Created) return fail(QStringLiteral("创建时间分组不允许拖放"));
    EntitySnapshot latest; QString failure;
    if (!m_repository.load(&latest, &failure)) return fail(failure);
    DropPlan plan;
    if (!buildDropPlan(intent, latest, toAppTime(appNow()).date(), &plan, &failure)) return fail(failure);
    return commitSnapshotUpdates(latest, plan.updates);
}
bool TaskStore::moveEntityToList(const QString &entityId, const QString &listId)
{
    if (!ensureLoaded()) return false;
    EntitySnapshot latest; QString failure;
    if (!m_repository.load(&latest, &failure)) return fail(failure);
    DropSnapshot planned(latest);
    const auto source = planned.entities.constFind(entityId);
    if (source == planned.entities.constEnd() || source->deleted || !availableList(planned, source->listId))
        return fail(QStringLiteral("源实体不存在、已删除或所属清单不可用"));
    // 显式 MoveTo 可移动终态任务，但绝不改变状态、重复实例、日期或置顶。
    if (!validateAncestors(planned, entityId, QString(), &failure)) return fail(failure);
    QSet<QString> subtree;
    if (!collectSubtree(planned, entityId, &subtree, &failure)
        || !moveSubtreeToList(&planned, entityId, listId, subtree, &failure)) return fail(failure);
    DropPlan plan; finishDropPlan(latest, planned, &plan);
    return commitSnapshotUpdates(latest, plan.updates);
}
QString TaskStore::createSection(const QString &listId, const QString &name)
{
    if (!ensureLoaded()) return QString(); if (listId.isEmpty() || !hasGroup(listId)) { fail(QStringLiteral("清单不存在")); return QString(); }
    for (const TaskList &list : m_snapshot.lists) if (list.id == listId && list.inbox) {
        fail(QStringLiteral("系统 inbox 不支持 section")); return QString();
    }
    const QDateTime now = utcNow(); Section row; row.id = newUuid(); row.listId = listId; row.name = name; row.createdAt = now; row.updatedAt = now;
    EntityChanges changes; changes.sectionSaves.append(row); return commit(&changes) ? row.id : QString();
}
bool TaskStore::renameSection(const QString &id, const QString &name)
{
    if (!ensureLoaded()) return false;
    if (id.isEmpty() || !hasSection(id)) return fail(QStringLiteral("section 不存在"));
    for (Section row : m_snapshot.sections) if (row.id == id) {
        row.name = name; row.updatedAt = utcNow(); EntityChanges changes; changes.sectionSaves.append(row); return commit(&changes);
    } return fail(QStringLiteral("section 不存在"));
}
bool TaskStore::deleteSection(const QString &id)
{
    if (!ensureLoaded()) return false; if (id.isEmpty() || !hasSection(id)) return fail(QStringLiteral("section 不存在"));
    EntityChanges changes; changes.sectionRemovals.append(id); return commit(&changes, true);
}

TaskReminder TaskStore::addReminder(const QString &entityId, const QDateTime &triggerAt)
{
    Entity value = entity(entityId); if (value.id.isEmpty()) { fail(QStringLiteral("任务不存在")); return TaskReminder(); }
    TaskReminder row; row.triggerAt = triggerAt.toUTC(); value.reminders.append(row);
    if (!saveTask(value)) return TaskReminder();
    const QVector<TaskReminder> saved = reminders(entityId);
    return saved.isEmpty() ? TaskReminder() : saved.last();
}
bool TaskStore::removeReminder(const QString &reminderId)
{
    const TaskReminder *reminder = findReminder(reminderId); if (!reminder) return fail(QStringLiteral("提醒不存在"));
    Entity value = entity(reminder->entityId);
    for (int i = 0; i < value.reminders.size(); ++i) if (value.reminders.at(i).id == reminderId) { value.reminders.remove(i); break; }
    // 直接编辑多提醒数组，保持 reminderAt 与旧门面一致，由 prepareSave 重算。
    return saveTask(value);
}
bool TaskStore::isReminderDelivered(const QString &reminderId) const
{
    for (const ReminderDelivery &row : m_snapshot.reminderDeliveries)
        if (row.reminderId == reminderId && row.installationId == installationId()) return true;
    return false;
}
bool TaskStore::markReminderDelivered(const QString &reminderId, const QDateTime &expectedTrigger, const QDateTime &now)
{
    if (!ensureLoaded()) return false; const TaskReminder *reminder = findReminder(reminderId);
    if (!reminder || !now.isValid() || reminder->triggerAt != expectedTrigger || expectedTrigger > now
        || isReminderDelivered(reminderId)) return false;
    const Entity current = entity(reminder->entityId);
    // 归档清单的任务不提醒：即使 UI 门面漏检，Repository SQL 也是权威闸门。
    if ((current.kind == EntityKind::Task && current.status != TaskStatus::Active) || current.deleted
        || isListArchived(current.listId)) return false;
    ReminderDelivery delivery; delivery.reminderId = reminderId; delivery.installationId = installationId();
    delivery.scheduledAt = expectedTrigger; delivery.deliveredAt = now;
    EntityChanges changes; changes.deliverySaves.append(delivery); return commit(&changes);
}
bool TaskStore::markReminderNotified(const QString &entityId, const QDateTime &expectedReminder, const QDateTime &now)
{
    if (!ensureLoaded()) return false;
    const Entity current = entity(entityId);
    if ((current.kind == EntityKind::Task && current.status != TaskStatus::Active) || current.deleted
        || isListArchived(current.listId)) return false;
    const int primary = primaryReminderIndex(current);
    if (primary < 0) return false;
    return markReminderDelivered(current.reminders.at(primary).id, expectedReminder, now);
}

DesktopNote TaskStore::desktopNote(const QString &entityId, const QString &device) const
{
    const QString wanted = device.isEmpty() ? deviceId() : device;
    for (const DesktopNote &row : m_snapshot.desktopNotes) if (row.entityId == entityId && row.deviceId == wanted) return row;
    return DesktopNote();
}
void TaskStore::applyDesktopFacade(Entity *task) const
{
    if (!task) return; const DesktopNote note = desktopNote(task->id);
    if (!note.id.isEmpty()) { task->color = note.color; task->geometry = note.geometry; task->zOrder = note.zOrder; task->layer = note.layer; }
}
bool TaskStore::saveDesktopNote(DesktopNote note)
{
    if (!ensureLoaded()) return false; if (!find(note.entityId)) return fail(QStringLiteral("桌面便签所属任务不存在"));
    if (note.deviceId.isEmpty()) note.deviceId = deviceId(); if (!isUuid(note.deviceId)) return fail(QStringLiteral("设备 id 非法"));
    DesktopNote existing = desktopNote(note.entityId, note.deviceId);
    if (!existing.id.isEmpty()) {
        if (!note.id.isEmpty() && note.id != existing.id) return fail(QStringLiteral("桌面便签 id 与任务/设备冲突"));
        note.id = existing.id; note.createdAt = existing.createdAt;
    } else { if (note.id.isEmpty()) note.id = newUuid(); if (!note.createdAt.isValid()) note.createdAt = utcNow(); }
    note.updatedAt = utcNow(); EntityChanges changes; changes.desktopNoteSaves.append(note); return commit(&changes);
}
bool TaskStore::saveDesktopState(const QString &entityId, NoteColor color, const QRect &geometry, int zOrder, NoteLayer layer, bool open)
{
    DesktopNote note = desktopNote(entityId); note.entityId = entityId; note.deviceId = deviceId(); note.color = color;
    note.geometry = geometry; note.zOrder = zOrder; note.layer = layer; note.open = open; return saveDesktopNote(note);
}
bool TaskStore::removeDesktopNote(const QString &entityId, const QString &device)
{
    const DesktopNote note = desktopNote(entityId, device); if (note.id.isEmpty()) return fail(QStringLiteral("桌面便签不存在"));
    EntityChanges changes; changes.desktopNoteRemovals.append(note.id); return commit(&changes);
}

Attachment TaskStore::importAttachment(const QString &entityId, const QString &sourcePath)
{
    if (!ensureLoaded()) return Attachment(); if (!find(entityId)) { fail(QStringLiteral("任务不存在")); return Attachment(); }
    if (!m_attachmentStore) { fail(QStringLiteral("附件存储未初始化")); return Attachment(); }
    QString failure; Attachment row = m_attachmentStore->importFile(entityId, sourcePath, nextAttachmentSortOrder(entityId), &failure);
    if (row.id.isEmpty()) { fail(failure); return Attachment(); }
    EntityChanges changes; changes.attachmentSaves.append(row); changes.touches.append(*find(entityId));
    addEvent(&changes, entity(entityId), QStringLiteral("attachment_added"), QStringLiteral("attachment"), QVariant(), row.id, utcNow());
    if (!commit(&changes)) { QString ignored; m_attachmentStore->removeFile(row.relativePath, &ignored); return Attachment(); }
    return row;
}
bool TaskStore::removeAttachment(const QString &entityId, const QString &attachmentId)
{
    if (!ensureLoaded()) return false; const Attachment *row = findAttachment(attachmentId);
    if (!row || row->entityId != entityId) return fail(QStringLiteral("附件不存在"));
    EntityChanges changes; changes.attachmentRemovals.append(*row); changes.touches.append(*find(entityId));
    addEvent(&changes, entity(entityId), QStringLiteral("attachment_removed"), QStringLiteral("attachment"), row->id, QVariant(), utcNow());
    return commit(&changes);
}
bool TaskStore::refreshAttachmentState(const QString &attachmentId)
{
    const Attachment *found = findAttachment(attachmentId); if (!found || !m_attachmentStore) return fail(QStringLiteral("附件不存在"));
    Attachment row = *found; QByteArray content; QString failure;
    const bool readable = m_attachmentStore->readContent(row, &content, &failure);
    row.localState = readable ? AttachmentFileState::Available
        : (!QFile::exists(m_attachmentStore->filePath(row)) ? AttachmentFileState::Missing : AttachmentFileState::Corrupt);
    row.verifiedAt = utcNow(); EntityChanges changes; changes.attachmentSaves.append(row); return commit(&changes);
}
bool TaskStore::openAttachment(const QString &attachmentId)
{
    const Attachment *row = findAttachment(attachmentId); if (!row || !m_attachmentStore) return fail(QStringLiteral("附件不存在"));
    QString failure; if (!m_attachmentStore->openWithDefaultProgram(*row, &failure)) { refreshAttachmentState(attachmentId); return fail(failure); }
    m_lastError.clear(); return true;
}
bool TaskStore::copyAttachmentTo(const QString &attachmentId, const QString &destinationPath)
{
    const Attachment *row = findAttachment(attachmentId); if (!row || !m_attachmentStore) return fail(QStringLiteral("附件不存在"));
    QString failure; if (!m_attachmentStore->copyTo(*row, destinationPath, &failure)) { refreshAttachmentState(attachmentId); return fail(failure); }
    m_lastError.clear(); return true;
}

QVector<EntityEvent> TaskStore::events(const QString &entityId)
{
    if (!ensureLoaded()) return QVector<EntityEvent>();
    QString failure;
    const QVector<EntityEvent> result = m_repository.eventsFor(entityId, &failure);
    if (!failure.isEmpty()) { fail(failure); return QVector<EntityEvent>(); }
    m_lastError.clear(); return result;
}

// 统计口径（全部基于事件日志，永久删除后事件仍在，计数不回退）：
// - tasksCreated / notesCreated：出现过 created 事件的实体按 kind 去重计数；
//   kind 取创建时事件里的类型，之后 convertKind 不回溯调整。
// - tasksCompleted：发生过 active/abandoned -> completed 状态事件的不同 Task 数；
//   完成后再打开、再次完成只计一次。
// - firstTaskAt：最早一个 Task created 事件的 occurredAt。
// 早期版本数据库/导入数据可能缺少 created 事件：用 m_snapshot 补齐，
// 但以实体 id 去重，与事件绝不重复计数。
UsageStatistics TaskStore::usageStatistics(QString *error)
{
    if (!ensureLoaded()) {
        if (error) *error = m_lastError;
        return UsageStatistics();
    }
    QString failure;
    const QVector<EntityEvent> allEvents = m_repository.allEvents(&failure);
    if (!failure.isEmpty()) { fail(failure, error); return UsageStatistics(); }

    // 去重只发生在本函数内部；返回的 UsageStatistics 不携带任何 id。
    QSet<QString> createdTaskIds, createdNoteIds, completedTaskIds;
    QDateTime firstTaskAt;
    for (const EntityEvent &event : allEvents) {
        if (event.entityId.isEmpty()) continue;
        if (event.type == QStringLiteral("created")) {
            if (event.entityKind == EntityKind::Note) {
                createdNoteIds.insert(event.entityId);
            } else {
                createdTaskIds.insert(event.entityId);
                if (event.occurredAt.isValid()
                    && (!firstTaskAt.isValid() || event.occurredAt < firstTaskAt))
                    firstTaskAt = event.occurredAt;
            }
        } else if (event.entityKind == EntityKind::Task
                   && event.type == QStringLiteral("changed")
                   && event.field == QStringLiteral("status")
                   && event.newValue.type() == QVariant::String
                   && event.newValue.toString() == QStringLiteral("completed")
                   && !(event.oldValue.type() == QVariant::String
                        && event.oldValue.toString() == QStringLiteral("completed"))) {
            completedTaskIds.insert(event.entityId);
        }
    }
    for (const StoredEntity &stored : m_snapshot.entities) {
        const Entity &entity = stored.entity;
        if (entity.kind == EntityKind::Note) {
            if (!createdNoteIds.contains(entity.id)) createdNoteIds.insert(entity.id);
        } else if (!createdTaskIds.contains(entity.id)) {
            createdTaskIds.insert(entity.id);
            if (entity.createdAt.isValid()
                && (!firstTaskAt.isValid() || entity.createdAt < firstTaskAt))
                firstTaskAt = entity.createdAt;
        }
        // 事件缺失时代完成的早期任务：当前仍是完成态则补计一次（有事件则已在集合中）。
        if (entity.kind == EntityKind::Task && entity.completed)
            completedTaskIds.insert(entity.id);
    }
    UsageStatistics stats;
    stats.tasksCreated = createdTaskIds.size();
    stats.notesCreated = createdNoteIds.size();
    stats.tasksCompleted = completedTaskIds.size();
    stats.firstTaskAt = firstTaskAt;
    m_lastError.clear();
    if (error) error->clear();
    return stats;
}
QString TaskStore::beginEditSession(const QString &entityId)
{
    if (!ensureLoaded()) return QString();
    const StoredEntity *stored = find(entityId);
    if (!stored) { fail(QStringLiteral("实体不存在")); return QString(); }
    const QString sessionId = m_journal.beginEditSession(stored->entity);
    if (sessionId.isEmpty()) fail(QStringLiteral("无法创建编辑会话"));
    else m_lastError.clear();
    return sessionId;
}
bool TaskStore::endEditSession(const QString &sessionId)
{
    if (!ensureLoaded()) return false;
    const QString entityId = m_journal.entityId(sessionId);
    if (entityId.isEmpty()) { m_lastError.clear(); return true; }
    EntityChanges changes;
    changes.eventSaves = m_journal.eventsForEnd(sessionId, entity(entityId));
    if (changes.eventSaves.isEmpty()) {
        m_journal.finishEditSession(sessionId); m_lastError.clear(); return true;
    }
    // 提交失败时保留基线；成功后先终结会话，再通知观察者，避免重入重复记账。
    if (!commit(&changes, false, [this, sessionId]() { m_journal.finishEditSession(sessionId); })) return false;
    m_lastError.clear(); return true;
}
bool TaskStore::endAllSessions()
{
    if (!ensureLoaded()) return false;
    QStringList sessions = m_journal.activeEditSessions(); sessions.sort();
    for (const QString &sessionId : sessions) if (!endEditSession(sessionId)) return false;
    m_lastError.clear(); return true;
}

bool TaskStore::isEditSessionActive(const QString &sessionId) const
{
    return !sessionId.isEmpty() && !m_journal.entityId(sessionId).isEmpty();
}

EntityTextSession *TaskStore::textSession(const QString &entityId)
{
    if (!ensureLoaded()) return nullptr;
    const StoredEntity *stored = find(entityId);
    if (!stored || stored->entity.deleted) {
        fail(QStringLiteral("实体不存在或已删除，不能创建文本编辑会话"));
        return nullptr;
    }
    const auto found = m_textSessions.constFind(entityId);
    if (found != m_textSessions.constEnd()) return found.value();
    EntityTextSession *session = new EntityTextSession(this, entityId, this);
    m_textSessions.insert(entityId, session);
    // 会话被显式销毁时同步注销，注册表永不悬挂。
    connect(session, &QObject::destroyed, this, [this, entityId]() { m_textSessions.remove(entityId); });
    m_lastError.clear();
    return session;
}

TextCommitResult TaskStore::commitTextDraft(const TextDraft &draft)
{
    TextCommitResult result;
    if (draft.entityId.isEmpty()) {
        m_lastError = QStringLiteral("草稿缺少实体 id");
        result.error = m_lastError;
        return result;
    }
    if (!ensureLoaded()) { result.error = m_lastError; return result; }
    const StoredEntity *stored = find(draft.entityId);
    if (!stored || stored->entity.deleted) {
        // 不存在或已删除（含回收站）的目标不可复活，也绝不写入。
        m_lastError.clear();
        result.status = TextCommitStatus::Unavailable;
        result.error = stored ? QStringLiteral("实体已在回收站或被删除，不能提交文本修改")
                              : QStringLiteral("实体不存在或已永久删除，不能提交文本修改");
        if (stored) result.entity = entity(draft.entityId);
        return result;
    }
    const Entity &current = stored->entity;
    QString title = current.title, body = current.body;
    bool writeTitle = false, writeBody = false;
    const bool titleConflict = !mergeTextField(draft.baselineTitle, draft.proposedTitle, current.title, &title, &writeTitle);
    const bool bodyConflict = !mergeTextField(draft.baselineBody, draft.proposedBody, current.body, &body, &writeBody);
    if (titleConflict || bodyConflict) {
        // 同字段双改且不同：整体拒绝，零写入，不推进 revision/updated_at，也不产生事件。
        m_lastError.clear();
        result.status = TextCommitStatus::Conflict;
        result.entity = entity(draft.entityId);
        QStringList fields;
        if (titleConflict) fields.append(QStringLiteral("标题"));
        if (bodyConflict) fields.append(QStringLiteral("正文"));
        result.error = fields.join(QStringLiteral("和")) + QStringLiteral("已在其他窗口修改，本次未保存任何更改。请重载最新内容后重试。");
        return result;
    }
    if (!writeTitle && !writeBody) {
        // 本地未改（含恢复基线）或远端已等于本地：视为成功，零写入。
        m_lastError.clear();
        result.status = TextCommitStatus::Unchanged;
        result.entity = entity(draft.entityId);
        return result;
    }
    // 有效改变：从当前快照重建整实体（桌面门面先还原，避免外观字段误判为变更），
    // 只覆盖 title/body，其余字段保持远端当前值，走既有 prepareSave/CAS/Journal 链。
    Entity merged = current;
    applyDesktopFacade(&merged);
    merged.title = title; merged.body = body;
    EntityChanges changes;
    // 归因标记：sessionId 有效且确属本实体时，本保存的 title/body 直存事件按会话口径推迟到
    // 会话结束/取消统一记账；否则（无会话、未知/跨实体会话）立即直存记账，绝不丢失。
    const bool previousTextCommitSave = m_textCommitSave;
    m_textCommitSave = isEditSessionActive(draft.sessionId)
                       && m_journal.entityId(draft.sessionId) == draft.entityId;
    const bool prepared = prepareSave(merged, &changes);
    m_textCommitSave = previousTextCommitSave;              // 保存/恢复，防信号回调重入串扰
    if (!prepared) { result.error = m_lastError; return result; }
    // 会话归因必须在本次事务落库之后、任何对外信号（taskChanged 等）发出之前确定：
    // 信号回调可能取消会话或继续改动实体，绝不能把那时的最新全实体算成本会话的写入。
    // 只登记本次真实写入的字段与终值（即本次写入后的 title/body）。
    Entity written;
    written.id = draft.entityId; written.kind = current.kind;
    written.title = title; written.body = body;
    const QString sessionId = draft.sessionId;
    const bool wroteTitle = writeTitle, wroteBody = writeBody;
    const auto recordCommit = [this, sessionId, wroteTitle, wroteBody, written]() {
        if (!sessionId.isEmpty()) m_journal.recordSessionCommit(sessionId, wroteTitle, wroteBody, written);
    };
    if (!commit(&changes, false, recordCommit)) { result.error = m_lastError; return result; }
    result.status = TextCommitStatus::Saved;
    result.entity = entity(draft.entityId);
    return result;
}

bool TaskStore::cancelEditSession(const QString &sessionId)
{
    // 只终结该会话；未知/已结束的 id 幂等成功，绝不影响其他编辑器的会话。
    const QString entityId = m_journal.entityId(sessionId);
    if (entityId.isEmpty()) { m_lastError.clear(); return true; }
    EntityChanges changes;
    changes.eventSaves = m_journal.eventsForCancel(sessionId);
    if (changes.eventSaves.isEmpty()) {
        // 纯丢弃（含目标已消失）：没有本会话自有已保存内容，直接终结。
        m_journal.finishEditSession(sessionId); m_lastError.clear(); return true;
    }
    // 有本会话已保存内容需要补记日志：提交失败必须返回 false 并保留会话供重试
    //（与 endEditSession 口径一致），不用假成功解锁窗口。
    if (!commit(&changes, false, [this, sessionId]() { m_journal.finishEditSession(sessionId); })) return false;
    m_lastError.clear(); return true;
}

bool TaskStore::setLocalMeta(const QString &key, const QString &value)
{
    if (!ensureLoaded()) return false;
    if ((key == QStringLiteral("installation_id") || key == QStringLiteral("device_id")
         || key == QStringLiteral("inbox_list_id")) && value != localMeta(key))
        return fail(QStringLiteral("安装/设备 id 创建后不可通过通用接口修改"));
    if (key == QStringLiteral("signed_in") && value != QStringLiteral("0") && value != QStringLiteral("1"))
        return fail(QStringLiteral("signed_in 只能为 0 或 1"));
    if (key == QStringLiteral("default_timezone") && !entitycodec::validTimeZone(value)) return fail(QStringLiteral("默认 IANA 时区非法"));
    QString failure; if (!m_repository.setLocalMeta(key, value, &failure)) return fail(failure);
    return load();
}

bool TaskStore::exportBackup(const QString &path)
{
    if (!ensureLoaded()) return false;
    QString failure;
    const QVector<EntityEvent> events = m_repository.allEvents(&failure);
    if (!failure.isEmpty()) return fail(QStringLiteral("读取活动日志失败: ") + failure);
    const QByteArray metadata = QJsonDocument(backupcodec::toJson(m_snapshot, events)).toJson(QJsonDocument::Compact);
    qint64 attachmentBytes = 0;
    for (const Attachment &row : m_snapshot.attachments) attachmentBytes += row.size;
    if (!attachmentcodec::validateBackupSizes(metadata.size(), attachmentBytes, m_snapshot.attachments.size(), nullptr, &failure))
        return fail(failure);
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return fail(QStringLiteral("创建备份失败: ") + output.errorString());
    const auto cancel = [this, &output](const QString &message) { output.cancelWriting(); return fail(message); };
    if (!m_attachmentStore) return cancel(QStringLiteral("附件目录未配置"));
    if (!attachmentcodec::writeBackupHeader(&output, metadata, static_cast<quint32>(m_snapshot.attachments.size()), &failure))
        return cancel(failure);
    for (const Attachment &row : m_snapshot.attachments) {
        QFile source(m_attachmentStore->filePath(row));
        if (!source.open(QIODevice::ReadOnly)) return cancel(QStringLiteral("导出附件失败: ") + source.errorString());
        if (source.size() != row.size || row.size <= 0 || row.size > attachmentcodec::kMaximumAttachmentBytes)
            return cancel(QStringLiteral("导出附件与元数据大小不一致: ") + row.relativePath);
        if (!attachmentcodec::writeBackupFrameHeader(&output, row.id, row.size, &failure)) return cancel(failure);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!attachmentcodec::copyBytes(&source, &output, row.size, &hash, &failure)) return cancel(failure);
        if (QString::fromLatin1(hash.result().toHex()) != row.sha256)
            return cancel(QStringLiteral("导出附件 SHA256 校验失败: ") + row.relativePath);
    }
    if (!output.commit()) return fail(QStringLiteral("提交备份失败: ") + output.errorString());
    m_lastError.clear(); return true;
}

bool TaskStore::importBackup(const QString &path)
{
    if (!ensureLoaded() || !endAllSessions()) return false;
    const QDateTime importedAt = utcNow(); // 整次导入共用一个事务时刻，解析与文件交换不改变到期边界。
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) return fail(QStringLiteral("打开备份失败: ") + input.errorString());
    QByteArray metadataBytes; QVector<attachmentcodec::BackupFrameInfo> frames; QString failure;
    // 旧 JSON/KNBK、截断、重复帧、尾随数据均在动库前拒绝；附件内容不进入内存。
    if (!attachmentcodec::readBackupContainer(&input, &metadataBytes, &frames, &failure)) return fail(failure);
    QJsonParseError parseError; const QJsonDocument document = QJsonDocument::fromJson(metadataBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(QStringLiteral("备份元数据 JSON 无效"));
    EntitySnapshot incoming; QVector<EntityEvent> incomingEvents; QString importedDefaultTimezone;
    if (!backupcodec::fromJson(document.object(), &incoming, &incomingEvents, &importedDefaultTimezone, &failure)) return fail(failure);
    if (frames.size() != incoming.attachments.size()) return fail(QStringLiteral("附件内容帧与元数据数量不一致"));
    QHash<QString, attachmentcodec::BackupFrameInfo> frameById;
    for (const attachmentcodec::BackupFrameInfo &frame : frames) frameById.insert(frame.id, frame);
    if (!m_attachmentStore) return fail(QStringLiteral("附件目录未配置"));
    RestoreFiles restore(dataDirPath(), m_attachmentStore->rootPath());
    if (!restore.create(&failure)) return fail(failure);
    const auto abortStaging = [this, &restore](const QString &message) {
        QString ignored; restore.rollback(&ignored); return fail(message);
    };
    for (const Attachment &row : incoming.attachments) {
        const auto frame = frameById.constFind(row.id);
        if (frame == frameById.constEnd()) return abortStaging(QStringLiteral("附件内容帧缺失或重复"));
        if (frame->size != row.size) return abortStaging(QStringLiteral("附件帧与元数据大小不一致"));
        QString stagedFailure;
        {
            QSaveFile staged(restore.stagingPath(row)); staged.setDirectWriteFallback(false);
            if (!staged.open(QIODevice::WriteOnly)) stagedFailure = QStringLiteral("创建附件暂存文件失败: ") + staged.errorString();
            else if (!input.seek(frame->offset)) { staged.cancelWriting(); stagedFailure = QStringLiteral("无法定位附件内容帧"); }
            else {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                if (!attachmentcodec::copyBytes(&input, &staged, frame->size, &hash, &failure)) {
                    staged.cancelWriting(); stagedFailure = failure;
                } else if (QString::fromLatin1(hash.result().toHex()) != row.sha256) {
                    staged.cancelWriting(); stagedFailure = QStringLiteral("附件 SHA256 校验失败: ") + row.id;
                } else if (!staged.commit()) stagedFailure = QStringLiteral("提交附件暂存文件失败: ") + staged.errorString();
            }
        } // 先释放 QSaveFile 句柄，再在 Windows 上删除暂存目录。
        if (!stagedFailure.isEmpty()) return abortStaging(stagedFailure);
    }
    input.close();
    EntitySnapshot replaced;
    const bool ok = m_repository.replaceAll(incoming, incomingEvents, importedDefaultTimezone, importedAt, &replaced, &failure,
        [&restore](QString *error) { return restore.swap(error); });
    if (!ok) {
        QString rollbackError; restore.rollback(&rollbackError);
        if (!rollbackError.isEmpty()) failure += QStringLiteral("；") + rollbackError;
        return fail(failure);
    }
    m_snapshot = replaced;
    // 恢复成功点（已落库、快照已替换、尚未发布任何对外信号）：共享文本会话全部作废——
    // 即使恢复后 title/body 与旧草稿完全相同，旧撤销/重做历史指向恢复前的编辑上下文，
    // 绝不允许在新数据上重放；旧会话 QPointer 失效，UI 在随后的 tasksReset 后按需重建。
    // 仅此成功点清除；导入失败与普通 tasksReset/分组刷新绝不清除。
    destroyTextSessions();
    if (!restore.cleanup(&failure)) emit errorOccurred(failure);
    m_lastError.clear(); emit groupsChanged(); emit tasksReset(); return true;
}

} // namespace kl
