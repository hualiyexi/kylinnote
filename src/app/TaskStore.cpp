#include "TaskStore.h"

#include "../domain/AttachmentCodec.h"
#include "../domain/BackupCodec.h"
#include "../domain/EntityCodec.h"

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

QDateTime plannedAt(const Entity &task) { return task.dueAt.isValid() ? task.dueAt : task.startAt; }
QString occurrenceKey(const Entity &task)
{
    if (task.allDayDate.isValid()) return task.allDayDate.toString(Qt::ISODate);
    const QDateTime plan = plannedAt(task);
    return plan.isValid() ? QString::number(utcMs(plan)) : QString();
}
QDateTime nextTimedRecurrence(const Entity &task, int anchorDay)
{
    const QDateTime plan = plannedAt(task);
    if (!plan.isValid()) return QDateTime();
    const QTimeZone zone(task.timeZone.toUtf8());
    const QDateTime local = plan.toTimeZone(zone);
    return nextRecurrence(local, task.rrule, anchorDay).toUTC();
}
bool sameChecklist(const ChecklistItem &a, const ChecklistItem &b)
{
    return a.text == b.text && a.done == b.done && a.sortOrder == b.sortOrder;
}
bool sameReminder(const TaskReminder &a, const TaskReminder &b)
{
    return a.triggerAt == b.triggerAt && a.sortOrder == b.sortOrder;
}
TaskTag makeTaskTag(const QString &entityId, const QString &tagId)
{
    TaskTag link; link.id = newUuid(); link.entityId = entityId; link.tagId = tagId; return link;
}
void addEvent(EntityChanges *changes, const Entity &entity, const QString &type, const QString &field,
              const QVariant &oldValue, const QVariant &newValue, const QDateTime &occurredAt)
{
    EntityEvent event; event.entityId = entity.id; event.entityKind = entity.kind; event.entityTitle = entity.title;
    event.type = type; event.field = field; event.oldValue = oldValue; event.newValue = newValue;
    event.occurredAt = occurredAt; changes->eventSaves.append(event);
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
    QJsonArray checklist;
    for (const ChecklistItem &item : entity.checklist) {
        QJsonObject row; row.insert(QStringLiteral("id"), item.id); row.insert(QStringLiteral("text"), item.text);
        row.insert(QStringLiteral("done"), item.done); checklist.append(row);
    }
    root.insert(QStringLiteral("checklist"), checklist);
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
const ChecklistItem *TaskStore::findChecklistItem(const QString &id) const
{
    for (const ChecklistItem &row : m_snapshot.checklistItems) if (row.id == id && find(row.entityId)) return &row;
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

bool TaskStore::commit(EntityChanges *changes, bool reset)
{
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
    for (const ChecklistItem &row : changes->checklistSaves) changedIds.insert(row.entityId);
    for (const TaskReminder &row : changes->reminderSaves) changedIds.insert(row.entityId);
    for (const ReminderDelivery &row : changes->deliverySaves) {
        const TaskReminder *reminder = findReminder(row.reminderId); if (reminder) changedIds.insert(reminder->entityId);
    }
    for (const QString &id : changes->checklistRemovals) {
        const ChecklistItem *item = findChecklistItem(id); if (item) changedIds.insert(item->entityId);
    }
    for (const QString &id : changes->reminderRemovals) {
        const TaskReminder *reminder = findReminder(id); if (reminder) changedIds.insert(reminder->entityId);
    }
    QSet<QString> removedTasks;
    for (const StoredEntity &row : changes->removals) removedTasks.insert(row.entity.id);
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
    m_snapshot = snapshot; m_lastError.clear();
    const bool groupChanges = !changes->groupSaves.isEmpty() || !changes->groupRemovals.isEmpty()
        || !changes->folderSaves.isEmpty() || !changes->folderRemovals.isEmpty()
        || !changes->sectionSaves.isEmpty() || !changes->sectionRemovals.isEmpty();
    if (groupChanges) emit groupsChanged();
    for (const QString &id : changedIds) emit taskChanged(id);
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

bool TaskStore::prepareSave(Entity value, EntityChanges *changes)
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

    QString rule;
    if (previous) {
        if (value.rrule != previous->entity.rrule) rule = value.rrule;
        else if (value.recurrence != previous->entity.recurrence) rule = value.recurrence;
        else rule = previous->entity.rrule;
    } else rule = value.rrule.isEmpty() ? value.recurrence : value.rrule;
    if (rule.isEmpty() || rule == QStringLiteral("none")) rule.clear();
    const QString canonical = rule.isEmpty() ? QString() : recurrenceRRule(rule);
    if (!rule.isEmpty() && canonical.isEmpty()) return fail(QStringLiteral("重复规则非法或包含未支持的扩展"));
    int anchor = 0;
    if (!canonical.isEmpty()) {
        const QDateTime plan = plannedAt(value);
        if (canonical.contains(QStringLiteral("FREQ=MONTHLY")))
            anchor = value.allDayDate.isValid() ? value.allDayDate.day() : plan.date().day();
        if (!value.allDayDate.isValid() && !plan.isValid()) return fail(QStringLiteral("重复任务必须有计划时间或全天日期"));
        bool reuse = previous && !previous->entity.recurrenceSeriesId.isEmpty() && previous->entity.rrule == canonical;
        if (reuse) {
            value.recurrenceSeriesId = previous->entity.recurrenceSeriesId;
            for (const RecurrenceSeries &series : m_snapshot.recurrenceSeries) if (series.id == value.recurrenceSeriesId && series.anchorDay > 0)
                anchor = series.anchorDay;
        } else value.recurrenceSeriesId = newUuid();
        if (previous && previous->entity.recurrenceSeriesId == value.recurrenceSeriesId
            && plannedAt(previous->entity) == plannedAt(value) && previous->entity.allDayDate == value.allDayDate)
            value.occurrenceKey = previous->entity.occurrenceKey;
        else value.occurrenceKey = occurrenceKey(value);
        value.rrule = canonical; value.recurrence = recurrenceName(canonical);
        if (!reuse || (previous && (previous->entity.recurrence != value.recurrence
                                   || plannedAt(previous->entity).date() != plannedAt(value).date()
                                   || previous->entity.allDayDate != value.allDayDate))) {
            RecurrenceSeries series; series.id = value.recurrenceSeriesId; series.rrule = canonical; series.anchorDay = anchor;
            series.timeZone = value.timeZone; series.dtstartAt = plannedAt(value); series.dtstartDate = value.allDayDate;
            series.createdAt = now; series.updatedAt = now;
            if (reuse) for (const RecurrenceSeries &old : m_snapshot.recurrenceSeries) if (old.id == series.id) series.createdAt = old.createdAt;
            changes->seriesSaves.append(series);
        }
    } else {
        value.recurrenceSeriesId.clear(); value.occurrenceKey.clear(); value.rrule.clear(); value.recurrence = QStringLiteral("none");
    }

    // 单提醒 UI 只编辑最早触发的一条；其他提醒保持不变。
    QString previousPrimaryId;
    if (previous) {
        const int primary = primaryReminderIndex(previous->entity);
        if (primary >= 0) previousPrimaryId = previous->entity.reminders.at(primary).id;
    }
    if (previous && value.reminderAt != previous->entity.reminderAt) {
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
        reminderIds.insert(row.id); row.entityId = value.id; row.sortOrder = i;
        if (!row.triggerAt.isValid()) return fail(QStringLiteral("提醒时间不能为空"));
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

    QHash<QString, ChecklistItem> oldItems;
    if (previous) for (const ChecklistItem &row : previous->entity.checklist) oldItems.insert(row.id, row);
    QSet<QString> itemIds;
    for (int i = 0; i < value.checklist.size(); ++i) {
        ChecklistItem &row = value.checklist[i];
        if (row.id.isEmpty()) row.id = newUuid();
        if (!isUuid(row.id) || itemIds.contains(row.id) || (!row.entityId.isEmpty() && row.entityId != value.id)
            || (findChecklistItem(row.id) && !oldItems.contains(row.id))) return fail(QStringLiteral("清单项 id 非法或不属于当前任务"));
        itemIds.insert(row.id); row.entityId = value.id; row.sortOrder = i;
        if (oldItems.contains(row.id)) {
            row.createdAt = oldItems.value(row.id).createdAt;
            if (!sameChecklist(row, oldItems.value(row.id))) { row.updatedAt = now; changes->checklistSaves.append(row); }
        } else { row.createdAt = now; row.updatedAt = now; changes->checklistSaves.append(row); }
    }
    if (previous) for (const ChecklistItem &row : previous->entity.checklist)
        if (!itemIds.contains(row.id)) changes->checklistRemovals.append(row.id);

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
    } else if (!wasCompleted && value.completed && !canonical.isEmpty()) {
        if (inserting) return fail(QStringLiteral("请先创建重复任务，再标记完成"));
        if (!linkFor(value.id)) {
            Entity next = value; next.id = newUuid(); next.status = TaskStatus::Active; next.completed = false;
            next.completedAt = QDateTime(); next.abandonedAt = QDateTime(); next.deleted = false; next.deletedAt = QDateTime();
            next.createdAt = now; next.updatedAt = now; next.notifiedAt = QDateTime(); next.parentId.clear();
            QDateTime nextPlan;
            if (value.allDayDate.isValid()) {
                next.allDayDate = nextRecurrenceDate(value.allDayDate, canonical, anchor);
                if (!next.allDayDate.isValid()) return fail(QStringLiteral("下一重复日期超出范围"));
                next.startAt = QDateTime(); next.dueAt = QDateTime();
            } else {
                nextPlan = nextTimedRecurrence(value, anchor);
                if (!nextPlan.isValid() || !entitycodec::validDate(nextPlan)) return fail(QStringLiteral("下一重复日期超出范围"));
                const QDateTime plan = plannedAt(value);
                if (value.dueAt.isValid()) {
                    next.dueAt = nextPlan;
                    if (value.startAt.isValid()) next.startAt = nextPlan.addMSecs(-value.startAt.msecsTo(value.dueAt));
                } else next.startAt = nextPlan;
            }
            next.occurrenceKey = occurrenceKey(next);
            for (TaskReminder &reminder : next.reminders) {
                if (value.allDayDate.isValid()) {
                    const qint64 days = value.allDayDate.daysTo(next.allDayDate);
                    reminder.triggerAt = reminder.triggerAt.toTimeZone(QTimeZone(value.timeZone.toUtf8())).addDays(days).toUTC();
                } else {
                    const qint64 offset = plannedAt(value).msecsTo(reminder.triggerAt);
                    reminder.triggerAt = nextPlan.addMSecs(offset);
                }
                if (!reminder.triggerAt.isValid() || !entitycodec::validDate(reminder.triggerAt)) return fail(QStringLiteral("下一提醒时间超出范围"));
                reminder.id = newUuid(); reminder.entityId = next.id; reminder.deliveredAt = QDateTime();
                reminder.createdAt = now; reminder.updatedAt = now; changes->reminderSaves.append(reminder);
            }
            const int nextPrimary = primaryReminderIndex(next);
            next.reminderAt = nextPrimary >= 0 ? next.reminders.at(nextPrimary).triggerAt : QDateTime();
            for (ChecklistItem &item : next.checklist) {
                item.id = newUuid(); item.entityId = next.id; item.done = false; item.createdAt = now; item.updatedAt = now;
                changes->checklistSaves.append(item);
            }
            StoredEntity generated; generated.entity = next; generated.revision = 0; generated.recurrenceAnchorDay = anchor;
            changes->generatedTaskIndex = changes->saves.size(); changes->generationSourceId = value.id; changes->saves.append(generated);
            RecurrenceLink link; link.id = newUuid();             link.sourceId = value.id; link.nextId = next.id; link.nextRevision = 1; changes->recurrenceLinkSaves.append(link);
            const QVector<TaskTag> sourceTagLinks = changes->taskTagSaves;
            for (const TaskTag &link : sourceTagLinks)
                if (link.entityId == value.id) changes->taskTagSaves.append(makeTaskTag(next.id, link.tagId));
            if (wantedTags == existingTags) for (const TaskTag &link : m_snapshot.taskTags)
                if (link.entityId == value.id) changes->taskTagSaves.append(makeTaskTag(next.id, link.tagId));
            // 新 occurrence 的桌面投影不自动复制；附件也保留在原实例。
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
    scalar(QStringLiteral("priority"), QString::number(old.priority), QString::number(value.priority));
    scalar(QStringLiteral("list"), idValue(old.listId), idValue(value.listId));
    scalar(QStringLiteral("section"), idValue(old.sectionId), idValue(value.sectionId));
    scalar(QStringLiteral("pinned"), msValue(old.pinnedAt), msValue(value.pinnedAt));
    scalar(QStringLiteral("recurrence"), old.rrule.isEmpty() ? QStringLiteral("none") : old.rrule,
           value.rrule.isEmpty() ? QStringLiteral("none") : value.rrule);
    scalar(QStringLiteral("folder"), idValue(old.folderId), idValue(value.folderId));
    scalar(QStringLiteral("parent"), idValue(old.parentId), idValue(value.parentId));
    scalar(QStringLiteral("timeZone"), old.timeZone, value.timeZone);
    scalar(QStringLiteral("sortOrder"), QString::number(old.sortOrder), QString::number(value.sortOrder));
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

    QHash<QString, ChecklistItem> oldItems;
    for (const ChecklistItem &row : old.checklist) oldItems.insert(row.id, row);
    for (const ChecklistItem &row : value.checklist) {
        const auto before = oldItems.constFind(row.id);
        if (before != oldItems.constEnd() && before->done != row.done)
            addEvent(changes, value, QStringLiteral("changed"), QStringLiteral("checklist.%1.done").arg(row.id),
                     before->done ? QStringLiteral("1") : QStringLiteral("0"), row.done ? QStringLiteral("1") : QStringLiteral("0"), now);
    }
    if (!m_journal.hasActiveSessionForEntity(value.id))
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
bool TaskStore::convertKind(const QString &id, EntityKind kind)
{
    if (!ensureLoaded()) return false;
    if (!find(id)) return fail(QStringLiteral("实体不存在"));
    EntitySnapshot after; QString failure;
    if (!m_repository.convertKind(id, kind, &after, &failure)) return fail(failure);
    m_snapshot = after; m_lastError.clear();
    emit taskChanged(id); emit tasksReset();
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
    for (ChecklistItem &item : value.checklist) {
        item.id.clear(); item.entityId.clear(); item.createdAt = QDateTime(); item.updatedAt = QDateTime();
    }
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
    addEvent(&changes, updated.entity, QStringLiteral("changed"), QStringLiteral("pinned"),
             msValue(stored->entity.pinnedAt), msValue(normalized), updated.entity.updatedAt);
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

QString TaskStore::createFolder(const QString &name)
{
    if (!ensureLoaded()) return QString();
    const QDateTime now = utcNow(); Folder row; row.id = newUuid(); row.name = name; row.createdAt = now; row.updatedAt = now;
    EntityChanges changes; changes.folderSaves.append(row); return commit(&changes) ? row.id : QString();
}
bool TaskStore::renameFolder(const QString &id, const QString &name)
{
    if (!ensureLoaded()) return false;
    if (id.isEmpty() || !hasFolder(id)) return fail(QStringLiteral("文件夹不存在"));
    for (Folder row : m_snapshot.folders) if (row.id == id) {
        row.name = name; row.updatedAt = utcNow(); EntityChanges changes; changes.folderSaves.append(row); return commit(&changes);
    } return fail(QStringLiteral("文件夹不存在"));
}
bool TaskStore::deleteFolder(const QString &id)
{
    if (!ensureLoaded()) return false; if (!hasFolder(id) || id.isEmpty()) return fail(QStringLiteral("文件夹不存在"));
    EntityChanges changes; changes.folderRemovals.append(id); return commit(&changes, true);
}
QString TaskStore::createList(const QString &name, const QString &folderId, EntityKind kind)
{
    if (!ensureLoaded()) return QString(); if (!hasFolder(folderId)) { fail(QStringLiteral("文件夹不存在")); return QString(); }
    const QDateTime now = utcNow(); TaskList row; row.id = newUuid(); row.folderId = folderId; row.kind = kind; row.name = name; row.createdAt = now; row.updatedAt = now;
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
    if (!ensureLoaded()) return false;
    if (!hasFolder(folderId)) return fail(QStringLiteral("文件夹不存在"));
    for (TaskList row : m_snapshot.lists) if (row.id == id) {
        if (row.inbox) return fail(QStringLiteral("系统 inbox 不可归入其他文件夹"));
        if (row.folderId == folderId) { m_lastError.clear(); return true; }
        row.folderId = folderId; row.updatedAt = utcNow();
        EntityChanges changes; changes.groupSaves.append(row); return commit(&changes);
    }
    return fail(QStringLiteral("清单不存在"));
}
bool TaskStore::deleteList(const QString &id)
{
    if (!ensureLoaded()) return false; if (id.isEmpty() || !hasGroup(id)) return fail(QStringLiteral("清单不存在"));
    for (const TaskList &list : m_snapshot.lists) if (list.id == id && list.inbox) return fail(QStringLiteral("系统 inbox 不可删除"));
    EntityChanges changes; changes.groupRemovals.append(id); return commit(&changes, true);
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

QString TaskStore::addChecklistItem(const QString &entityId, const QString &text)
{
    Entity value = entity(entityId); if (value.id.isEmpty()) { fail(QStringLiteral("任务不存在")); return QString(); }
    ChecklistItem row; row.text = text; value.checklist.append(row);
    if (!saveTask(value)) return QString();
    return entity(entityId).checklist.isEmpty() ? QString() : entity(entityId).checklist.last().id;
}
bool TaskStore::setChecklistDone(const QString &itemId, bool done)
{
    const ChecklistItem *item = findChecklistItem(itemId); if (!item) return fail(QStringLiteral("清单项不存在"));
    Entity value = entity(item->entityId); for (ChecklistItem &row : value.checklist) if (row.id == itemId) row.done = done;
    return saveTask(value);
}
bool TaskStore::removeChecklistItem(const QString &itemId)
{
    const ChecklistItem *item = findChecklistItem(itemId); if (!item) return fail(QStringLiteral("清单项不存在"));
    Entity value = entity(item->entityId);
    for (int i = 0; i < value.checklist.size(); ++i) if (value.checklist.at(i).id == itemId) { value.checklist.remove(i); break; }
    return saveTask(value);
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
    if (current.kind != EntityKind::Task || current.status != TaskStatus::Active || current.deleted) return false;
    ReminderDelivery delivery; delivery.reminderId = reminderId; delivery.installationId = installationId();
    delivery.scheduledAt = expectedTrigger; delivery.deliveredAt = now;
    EntityChanges changes; changes.deliverySaves.append(delivery); return commit(&changes);
}
bool TaskStore::markReminderNotified(const QString &entityId, const QDateTime &expectedReminder, const QDateTime &now)
{
    if (!ensureLoaded()) return false;
    const Entity current = entity(entityId);
    if (current.kind != EntityKind::Task || current.status != TaskStatus::Active || current.deleted) return false;
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
    // 提交失败时保留基线，调用方可以用同一 session UUID 重试。
    if (!commit(&changes)) return false;
    m_journal.finishEditSession(sessionId); m_lastError.clear(); return true;
}
bool TaskStore::endAllSessions()
{
    if (!ensureLoaded()) return false;
    QStringList sessions = m_journal.activeEditSessions(); sessions.sort();
    for (const QString &sessionId : sessions) if (!endEditSession(sessionId)) return false;
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
    if (!restore.cleanup(&failure)) emit errorOccurred(failure);
    m_lastError.clear(); emit groupsChanged(); emit tasksReset(); return true;
}

} // namespace kl
