#include "TaskStore.h"

#include "../domain/TaskCodec.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

namespace kn {
namespace {
const char *const kBackupFormat = "KylinNote.Tasks";
const int kBackupVersion = 1;
const qint64 kMaximumBackupBytes = 64 * 1024 * 1024;

QDateTime plannedAt(const Task &task)
{
    return task.dueAt.isValid() ? task.dueAt : task.startAt;
}

bool parseBackup(const QByteArray &bytes, TaskSnapshot *out, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return taskcodec::fail(error, QStringLiteral("备份 JSON 无效: ") + parseError.errorString());
    const QJsonObject root = document.object();
    qint64 version = 0;
    if (!root.value(QStringLiteral("format")).isString()
        || root.value(QStringLiteral("format")).toString() != QLatin1String(kBackupFormat)
        || !taskcodec::readInteger(root.value(QStringLiteral("version")), kBackupVersion, kBackupVersion, &version))
        return taskcodec::fail(error, QStringLiteral("不支持的备份格式或版本"));
    if (!root.value(QStringLiteral("groups")).isArray() || !root.value(QStringLiteral("tasks")).isArray()
        || !root.value(QStringLiteral("recurrenceLinks")).isArray())
        return taskcodec::fail(error, QStringLiteral("备份缺少 groups/tasks/recurrenceLinks 数组"));

    TaskSnapshot snapshot;
    QSet<qint64> groups;
    QHash<qint64, StoredTask> tasks;
    for (const QJsonValue &value : root.value(QStringLiteral("groups")).toArray()) {
        if (!value.isObject()) return taskcodec::fail(error, QStringLiteral("分组必须是对象"));
        const QJsonObject object = value.toObject();
        TaskGroup group;
        if (!taskcodec::readId(object.value(QStringLiteral("id")), false, &group.id)
            || !object.value(QStringLiteral("name")).isString())
            return taskcodec::fail(error, QStringLiteral("备份分组字段类型非法"));
        group.name = object.value(QStringLiteral("name")).toString();
        if (groups.contains(group.id) || group.name.trimmed().isEmpty())
            return taskcodec::fail(error, QStringLiteral("备份分组 id 重复或名称为空"));
        groups.insert(group.id);
        snapshot.groups.append(group);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("tasks")).toArray()) {
        if (!value.isObject()) return taskcodec::fail(error, QStringLiteral("任务必须是对象"));
        const QJsonObject object = value.toObject();
        StoredTask stored;
        if (!taskcodec::fromJson(object, &stored.task, error)) return false;
        qint64 anchor = 0;
        if (!taskcodec::readId(object.value(QStringLiteral("revision")), false, &stored.revision)
            || !taskcodec::readInteger(object.value(QStringLiteral("recurrenceAnchorDay")), 0, 31, &anchor))
            return taskcodec::fail(error, QStringLiteral("备份任务版本/重复锚点非法"));
        stored.recurrenceAnchorDay = static_cast<int>(anchor);
        if (tasks.contains(stored.task.id) || (stored.task.groupId && !groups.contains(stored.task.groupId)))
            return taskcodec::fail(error, QStringLiteral("备份任务 id 重复或分组不存在"));
        if (stored.task.recurrence == QStringLiteral("monthly") && stored.recurrenceAnchorDay == 0)
            return taskcodec::fail(error, QStringLiteral("月度重复缺少日期锚点"));
        tasks.insert(stored.task.id, stored);
        snapshot.tasks.append(stored);
    }
    QHash<qint64, qint64> edges;
    QSet<qint64> targets;
    for (const QJsonValue &value : root.value(QStringLiteral("recurrenceLinks")).toArray()) {
        if (!value.isObject()) return taskcodec::fail(error, QStringLiteral("重复生成凭证必须是对象"));
        const QJsonObject object = value.toObject();
        RecurrenceLink link;
        if (!taskcodec::readId(object.value(QStringLiteral("sourceId")), false, &link.sourceId)
            || !taskcodec::readId(object.value(QStringLiteral("nextId")), true, &link.nextId)
            || !taskcodec::readId(object.value(QStringLiteral("nextRevision")), false, &link.nextRevision)
            || !tasks.contains(link.sourceId) || edges.contains(link.sourceId)
            || !tasks.value(link.sourceId).task.completed || link.sourceId == link.nextId
            || (link.nextId && (!tasks.contains(link.nextId) || targets.contains(link.nextId)
                                || link.nextRevision > tasks.value(link.nextId).revision)))
            return taskcodec::fail(error, QStringLiteral("备份重复任务引用/版本非法"));
        edges.insert(link.sourceId, link.nextId);
        if (link.nextId) targets.insert(link.nextId);
        snapshot.recurrenceLinks.append(link);
    }
    // 每个节点最多一个后继，以线性遍历拒绝任意长度的循环。
    QSet<qint64> verified;
    for (auto it = edges.constBegin(); it != edges.constEnd(); ++it) {
        QSet<qint64> visiting;
        qint64 id = it.key();
        while (id && edges.contains(id) && !verified.contains(id)) {
            if (visiting.contains(id)) return taskcodec::fail(error, QStringLiteral("重复任务来源形成循环"));
            visiting.insert(id);
            id = edges.value(id);
        }
        verified.unite(visiting);
    }
    *out = snapshot;
    return true;
}
}

TaskStore::TaskStore(const QSqlDatabase &database, QObject *parent)
    : QObject(parent), m_repository(database) {}

bool TaskStore::fail(const QString &message, QString *error)
{
    m_lastError = message;
    if (error) *error = message;
    emit errorOccurred(message);
    return false;
}

bool TaskStore::ensureLoaded()
{
    return m_loaded ? true : fail(QStringLiteral("请先加载任务数据"));
}

bool TaskStore::load(QString *error)
{
    TaskSnapshot snapshot;
    QString failure;
    if (!m_repository.load(&snapshot, &failure)) return fail(failure, error);
    m_snapshot = snapshot;
    m_loaded = true;
    m_lastError.clear();
    if (error) error->clear();
    emit groupsChanged();
    emit tasksReset();
    return true;
}

QVector<Task> TaskStore::tasks() const
{
    QVector<Task> result;
    result.reserve(m_snapshot.tasks.size());
    for (const StoredTask &task : m_snapshot.tasks) result.append(task.task);
    return result;
}

QVector<TaskGroup> TaskStore::groups() const { return m_snapshot.groups; }

const StoredTask *TaskStore::find(qint64 id) const
{
    for (const StoredTask &task : m_snapshot.tasks) if (task.task.id == id) return &task;
    return nullptr;
}

const RecurrenceLink *TaskStore::linkFor(qint64 sourceId) const
{
    for (const RecurrenceLink &link : m_snapshot.recurrenceLinks) if (link.sourceId == sourceId) return &link;
    return nullptr;
}

bool TaskStore::hasGroup(qint64 id) const
{
    if (id == 0) return true;
    for (const TaskGroup &group : m_snapshot.groups) if (group.id == id) return true;
    return false;
}

Task TaskStore::task(qint64 id) const
{
    const StoredTask *stored = find(id);
    return stored ? stored->task : Task();
}

bool TaskStore::commit(TaskChanges *changes, bool reset)
{
    TaskSnapshot snapshot;
    QString failure;
    QSet<qint64> changedIds;
    for (const StoredTask &removed : changes->removals) changedIds.insert(removed.task.id);
    for (qint64 groupId : changes->groupRemovals)
        for (const StoredTask &stored : m_snapshot.tasks)
            if (stored.task.groupId == groupId) changedIds.insert(stored.task.id);
    if (!m_repository.apply(changes, &snapshot, &failure)) return fail(failure);
    for (const StoredTask &saved : changes->saves) changedIds.insert(saved.task.id);
    m_snapshot = snapshot;
    m_lastError.clear();
    const bool groupChanges = !changes->groupSaves.isEmpty() || !changes->groupRemovals.isEmpty();
    if (groupChanges) emit groupsChanged();
    for (qint64 id : changedIds) emit taskChanged(id);
    if (reset || !changes->removals.isEmpty() || changes->generationSourceId || groupChanges) emit tasksReset();
    return true;
}

bool TaskStore::prepareSave(Task value, TaskChanges *changes)
{
    const StoredTask *previous = value.id ? find(value.id) : nullptr;
    if (value.id && !previous) return fail(QStringLiteral("任务不存在"));
    if (!hasGroup(value.groupId)) return fail(QStringLiteral("任务分组不存在"));
    QString failure;
    // 提醒编辑优先清除旧 claim；移除 reminderAt 时调用者通常仍带着旧 notifiedAt。
    if (previous) value.notifiedAt = value.reminderAt == previous->task.reminderAt ? previous->task.notifiedAt : QDateTime();
    else value.notifiedAt = QDateTime();
    // 先检查输入中非法的日期；后续归一化只填未设置的审计时间。
    if (!taskcodec::validate(value, &failure)) return fail(failure);
    StoredTask stored = previous ? *previous : StoredTask();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool wasCompleted = previous && previous->task.completed;
    if (previous) {
        value.createdAt = previous->task.createdAt;
        value.notifiedAt = value.reminderAt == previous->task.reminderAt ? previous->task.notifiedAt : QDateTime();
    } else {
        value.createdAt = now;
        value.notifiedAt = QDateTime();
    }
    value.updatedAt = now;
    value.completedAt = value.completed ? (wasCompleted ? previous->task.completedAt : now) : QDateTime();
    if (previous && previous->task.deleted && value.completed != wasCompleted)
        return fail(QStringLiteral("请先恢复回收站任务再改变完成状态"));
    const QDateTime plan = plannedAt(value);
    if (value.recurrence == QStringLiteral("monthly")) {
        if (!previous || stored.recurrenceAnchorDay == 0 || previous->task.recurrence != value.recurrence
            || plannedAt(previous->task).date() != plan.date()) stored.recurrenceAnchorDay = plan.date().day();
    } else {
        stored.recurrenceAnchorDay = 0;
    }
    stored.task = value;
    changes->saves.append(stored);

    if (wasCompleted && !value.completed) {
        const RecurrenceLink *link = linkFor(value.id);
        if (link) {
            const StoredTask *next = find(link->nextId);
            if (!next || next->revision != link->nextRevision
                || next->task.completed || next->task.deleted || linkFor(link->nextId))
                return fail(QStringLiteral("下一次重复任务已被修改、完成或删除，无法安全撤销；请先处理该实例"));
            changes->removeLinks.append(value.id);
            changes->removals.append(*next);
        }
    } else if (!wasCompleted && value.completed && value.recurrence != QStringLiteral("none")) {
        // 唯一凭证是防重复的最终约束；再次保存/重启不会重复生成。
        if (!linkFor(value.id)) {
            Task next = value;
            const QDateTime nextPlan = nextRecurrence(plan, value.recurrence, stored.recurrenceAnchorDay);
            if (!nextPlan.isValid() || !taskcodec::validDate(nextPlan)) return fail(QStringLiteral("下一重复日期超出可用范围"));
            next.id = 0;
            next.completed = false;
            next.completedAt = QDateTime();
            next.notifiedAt = QDateTime();
            next.deleted = false;
            next.createdAt = now;
            next.updatedAt = now;
            if (value.dueAt.isValid()) {
                next.dueAt = nextPlan;
                if (value.startAt.isValid()) next.startAt = nextPlan.addSecs(-value.startAt.secsTo(value.dueAt));
            } else {
                next.startAt = nextPlan;
            }
            if (value.reminderAt.isValid()) next.reminderAt = nextPlan.addSecs(plan.secsTo(value.reminderAt));
            for (ChecklistItem &item : next.checklist) item.done = false;
            if (!taskcodec::validate(next, &failure)) return fail(failure);
            StoredTask generated;
            generated.task = next;
            generated.recurrenceAnchorDay = stored.recurrenceAnchorDay;
            changes->generationSourceId = value.id; // 新建已完成重复任务没有来源 id，见 saveTask 的限制
            changes->generatedTaskIndex = changes->saves.size();
            changes->saves.append(generated);
        }
    }
    return true;
}

qint64 TaskStore::createTask(const QString &title, qint64 groupId)
{
    if (!ensureLoaded()) return 0;
    Task task;
    task.title = title;
    task.groupId = groupId;
    TaskChanges changes;
    if (!prepareSave(task, &changes) || !commit(&changes, true)) return 0;
    return changes.saves.first().task.id;
}

bool TaskStore::saveTask(Task value)
{
    if (!ensureLoaded()) return false;
    if (value.id == 0 && value.completed && value.recurrence != QStringLiteral("none"))
        return fail(QStringLiteral("请先创建重复任务，再将其标记完成"));
    TaskChanges changes;
    const bool inserting = value.id == 0;
    return prepareSave(value, &changes) && commit(&changes, inserting);
}

bool TaskStore::setCompleted(qint64 id, bool completed)
{
    if (!ensureLoaded()) return false;
    Task value = task(id);
    if (!value.id) return fail(QStringLiteral("任务不存在"));
    if (value.completed == completed) { m_lastError.clear(); return true; }
    value.completed = completed;
    return saveTask(value);
}

bool TaskStore::setPinned(qint64 id, bool pinned)
{
    if (!ensureLoaded()) return false;
    Task value = task(id);
    if (!value.id) return fail(QStringLiteral("任务不存在"));
    if (value.pinned == pinned) { m_lastError.clear(); return true; }
    value.pinned = pinned;
    return saveTask(value);
}

bool TaskStore::trashTask(qint64 id)
{
    if (!ensureLoaded()) return false;
    Task value = task(id);
    if (!value.id) return fail(QStringLiteral("任务不存在"));
    if (value.deleted) return true;
    value.deleted = true;
    return saveTask(value);
}

bool TaskStore::restoreTask(qint64 id)
{
    if (!ensureLoaded()) return false;
    Task value = task(id);
    if (!value.id) return fail(QStringLiteral("任务不存在"));
    if (!value.deleted) return true;
    value.deleted = false;
    return saveTask(value);
}

bool TaskStore::deletePermanently(qint64 id)
{
    if (!ensureLoaded()) return false;
    const StoredTask *stored = find(id);
    if (!stored) return fail(QStringLiteral("任务不存在"));
    if (!stored->task.deleted) return fail(QStringLiteral("请先把任务移到回收站"));
    TaskChanges changes;
    changes.removals.append(*stored);
    return commit(&changes, true);
}

qint64 TaskStore::createGroup(const QString &name)
{
    if (!ensureLoaded()) return 0;
    TaskGroup group;
    group.name = name;
    TaskChanges changes;
    changes.groupSaves.append(group);
    if (!commit(&changes)) return 0;
    return changes.groupSaves.first().id;
}

bool TaskStore::renameGroup(qint64 id, const QString &name)
{
    if (!ensureLoaded()) return false;
    if (id <= 0 || !hasGroup(id)) return fail(QStringLiteral("分组不存在"));
    TaskGroup group;
    group.id = id;
    group.name = name;
    TaskChanges changes;
    changes.groupSaves.append(group);
    return commit(&changes);
}

bool TaskStore::deleteGroup(qint64 id)
{
    if (!ensureLoaded()) return false;
    if (id <= 0 || !hasGroup(id)) return fail(QStringLiteral("分组不存在"));
    TaskChanges changes;
    changes.groupRemovals.append(id);
    return commit(&changes, true);
}

bool TaskStore::markReminderNotified(qint64 id, const QDateTime &expectedReminder, const QDateTime &now)
{
    if (!ensureLoaded()) return false;
    const StoredTask *current = find(id);
    if (!current || current->task.completed || current->task.deleted || !now.isValid()
        || !expectedReminder.isValid() || current->task.reminderAt != expectedReminder
        || expectedReminder > now || (current->task.notifiedAt.isValid() && current->task.notifiedAt >= expectedReminder)) return false;
    StoredTask updated = *current;
    updated.task.notifiedAt = now < expectedReminder ? expectedReminder : now;
    updated.task.updatedAt = now;
    TaskChanges changes;
    changes.saves.append(updated);
    return commit(&changes);
}

bool TaskStore::exportBackup(const QString &path)
{
    if (!ensureLoaded()) return false;
    QJsonObject root;
    root.insert(QStringLiteral("format"), QString::fromLatin1(kBackupFormat));
    root.insert(QStringLiteral("version"), kBackupVersion);
    QJsonArray groups;
    for (const TaskGroup &group : m_snapshot.groups) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), QString::number(group.id));
        object.insert(QStringLiteral("name"), group.name);
        groups.append(object);
    }
    QJsonArray tasks;
    for (const StoredTask &task : m_snapshot.tasks) {
        QJsonObject object = taskcodec::toJson(task.task);
        object.insert(QStringLiteral("revision"), QString::number(task.revision));
        object.insert(QStringLiteral("recurrenceAnchorDay"), task.recurrenceAnchorDay);
        tasks.append(object);
    }
    QJsonArray links;
    for (const RecurrenceLink &link : m_snapshot.recurrenceLinks) {
        QJsonObject object;
        object.insert(QStringLiteral("sourceId"), QString::number(link.sourceId));
        object.insert(QStringLiteral("nextId"), QString::number(link.nextId));
        object.insert(QStringLiteral("nextRevision"), QString::number(link.nextRevision));
        links.append(object);
    }
    root.insert(QStringLiteral("groups"), groups);
    root.insert(QStringLiteral("tasks"), tasks);
    root.insert(QStringLiteral("recurrenceLinks"), links);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    // 默认不回退直接覆盖，确保失败时既有备份仍完整。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(QStringLiteral("创建备份失败: ") + file.errorString());
    if (file.write(bytes) != bytes.size()) {
        const QString failure = file.errorString();
        file.cancelWriting();
        return fail(QStringLiteral("写入备份失败: ") + failure);
    }
    if (!file.commit()) return fail(QStringLiteral("提交备份失败: ") + file.errorString());
    m_lastError.clear();
    return true;
}

bool TaskStore::importBackup(const QString &path)
{
    if (!ensureLoaded()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("打开备份失败: ") + file.errorString());
    if (file.size() > kMaximumBackupBytes) return fail(QStringLiteral("备份超过 64 MiB 限制"));
    const QByteArray bytes = file.read(kMaximumBackupBytes + 1);
    if (file.error() != QFile::NoError) return fail(QStringLiteral("读取备份失败: ") + file.errorString());
    if (bytes.size() > kMaximumBackupBytes) return fail(QStringLiteral("备份超过 64 MiB 限制"));
    TaskSnapshot incoming;
    QString failure;
    if (!parseBackup(bytes, &incoming, &failure)) return fail(failure);
    TaskSnapshot merged;
    if (!m_repository.merge(incoming, &merged, &failure)) return fail(failure);
    m_snapshot = merged;
    m_lastError.clear();
    emit groupsChanged();
    emit tasksReset();
    return true;
}

} // namespace kn
