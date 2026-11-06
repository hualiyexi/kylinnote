#include "TaskRepository.h"

#include "../domain/TaskCodec.h"

#include <QHash>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace kn {
namespace {
using taskcodec::fail;

QString safeText(const QString &text)
{
    return text.isNull() ? QStringLiteral("") : text;
}

QVariant dateValue(const QDateTime &date)
{
    return date.isValid() ? QVariant(taskcodec::dateText(date)) : QVariant(QVariant::String);
}

bool databaseDate(const QVariant &value, QDateTime *out, QString *error)
{
    // v1 的空时间必须原样保留；任务领域中视为未设置。
    if (value.isNull() || value.toString().isEmpty()) { *out = QDateTime(); return true; }
    const QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid() || !taskcodec::validDate(parsed))
        return fail(error, QStringLiteral("数据库包含无效日期: ") + value.toString());
    *out = parsed;
    return true;
}

bool sqlFailure(const QSqlQuery &query, QString *error)
{
    return fail(error, QStringLiteral("数据库操作失败: ") + query.lastError().text());
}
}

TaskRepository::TaskRepository(const QSqlDatabase &database) : m_database(database) {}

bool TaskRepository::ready(QString *error) const
{
    return m_database.isValid() && m_database.isOpen()
        ? true : fail(error, QStringLiteral("任务数据库未打开"));
}

bool TaskRepository::readSnapshot(TaskSnapshot *out, QString *error) const
{
    TaskSnapshot snapshot;
    QSqlQuery groups(m_database);
    if (!groups.exec(QStringLiteral("SELECT id,name FROM task_groups ORDER BY id"))) return sqlFailure(groups, error);
    QSet<qint64> groupIds;
    while (groups.next()) {
        TaskGroup group;
        group.id = groups.value(0).toLongLong();
        group.name = groups.value(1).toString();
        if (group.id <= 0 || group.name.trimmed().isEmpty()) return fail(error, QStringLiteral("数据库包含无效分组"));
        groupIds.insert(group.id);
        snapshot.groups.append(group);
    }
    if (groups.lastError().isValid()) return sqlFailure(groups, error);

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral(
            "SELECT id,title,description,content_format,group_id,priority,completed,favorite,pinned,deleted,"
            "start_at,due_at,reminder_at,notified_at,created_at,updated_at,completed_at,recurrence,"
            "tags,checklist,color,geometry,z_order,layer,revision,recurrence_anchor_day FROM tasks ORDER BY id")))
        return sqlFailure(query, error);
    QSet<qint64> taskIds;
    QSet<qint64> completedIds;
    QHash<qint64, qint64> revisions;
    while (query.next()) {
        StoredTask stored;
        Task &task = stored.task;
        task.id = query.value(0).toLongLong();
        task.title = query.value(1).toString();
        task.description = query.value(2).toString();
        task.contentFormat = static_cast<ContentFormat>(query.value(3).toInt());
        task.groupId = query.value(4).isNull() ? 0 : query.value(4).toLongLong();
        task.priority = query.value(5).toInt();
        task.completed = query.value(6).toInt() != 0;
        task.favorite = query.value(7).toInt() != 0;
        task.pinned = query.value(8).toInt() != 0;
        task.deleted = query.value(9).toInt() != 0;
        QDateTime *const dates[] = {&task.startAt, &task.dueAt, &task.reminderAt, &task.notifiedAt,
                                   &task.createdAt, &task.updatedAt, &task.completedAt};
        for (int i = 0; i < 7; ++i) if (!databaseDate(query.value(10 + i), dates[i], error)) return false;
        task.recurrence = query.value(17).toString();
        QJsonParseError tagsError, checklistError;
        const QJsonDocument tags = QJsonDocument::fromJson(query.value(18).toByteArray(), &tagsError);
        const QJsonDocument checklist = QJsonDocument::fromJson(query.value(19).toByteArray(), &checklistError);
        if (tagsError.error != QJsonParseError::NoError || checklistError.error != QJsonParseError::NoError
            || !tags.isArray() || !checklist.isArray()) return fail(error, QStringLiteral("数据库标签/清单 JSON 损坏"));
        if (!taskcodec::readLists(tags.array(), checklist.array(), &task.tags, &task.checklist, error)) return false;
        task.color = static_cast<NoteColor>(query.value(20).toInt());
        task.geometry = decodeGeometry(query.value(21).toString());
        task.zOrder = query.value(22).toInt();
        task.layer = static_cast<NoteLayer>(query.value(23).toInt());
        stored.revision = query.value(24).toLongLong();
        stored.recurrenceAnchorDay = query.value(25).toInt();
        if (!taskcodec::validate(task, error)) return false;
        if (task.id <= 0 || stored.revision <= 0 || stored.recurrenceAnchorDay < 0 || stored.recurrenceAnchorDay > 31
            || (task.groupId != 0 && !groupIds.contains(task.groupId)) || task.completed != task.completedAt.isValid())
            return fail(error, QStringLiteral("数据库任务引用/版本/完成状态损坏"));
        taskIds.insert(task.id);
        if (task.completed) completedIds.insert(task.id);
        revisions.insert(task.id, stored.revision);
        snapshot.tasks.append(stored);
    }
    if (query.lastError().isValid()) return sqlFailure(query, error);
    QSqlQuery links(m_database);
    QSet<qint64> targets;
    if (!links.exec(QStringLiteral("SELECT source_id,next_id,next_revision FROM task_recurrence_links ORDER BY source_id")))
        return sqlFailure(links, error);
    while (links.next()) {
        RecurrenceLink link;
        link.sourceId = links.value(0).toLongLong();
        link.nextId = links.value(1).isNull() ? 0 : links.value(1).toLongLong();
        link.nextRevision = links.value(2).toLongLong();
        if (!taskIds.contains(link.sourceId) || !completedIds.contains(link.sourceId)
            || (link.nextId && (!taskIds.contains(link.nextId) || targets.contains(link.nextId)))
            || link.sourceId == link.nextId || link.nextRevision <= 0
            || (link.nextId && link.nextRevision > revisions.value(link.nextId)))
            return fail(error, QStringLiteral("重复任务生成凭证损坏"));
        if (link.nextId) targets.insert(link.nextId);
        snapshot.recurrenceLinks.append(link);
    }
    if (links.lastError().isValid()) return sqlFailure(links, error);
    *out = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::load(TaskSnapshot *out, QString *error) const
{
    if (!out || !ready(error)) return false;
    // 多张表必须从同一个 SQLite 快照读取；失败不会改写 out。
    QSqlDatabase database = m_database;
    if (!database.transaction()) return fail(error, database.lastError().text());
    TaskSnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) { database.rollback(); return false; }
    if (!database.commit()) { database.rollback(); return fail(error, database.lastError().text()); }
    *out = snapshot;
    return true;
}

bool TaskRepository::saveGroup(TaskGroup *group, QString *error)
{
    if (group->id < 0 || group->name.trimmed().isEmpty()) return fail(error, QStringLiteral("分组名称不能为空"));
    group->name = group->name.trimmed();
    QSqlQuery query(m_database);
    const bool inserting = group->id == 0;
    if (!query.prepare(inserting ? QStringLiteral("INSERT INTO task_groups(name) VALUES(?)")
                                 : QStringLiteral("UPDATE task_groups SET name=? WHERE id=?"))) return sqlFailure(query, error);
    query.addBindValue(group->name);
    if (!inserting) query.addBindValue(group->id);
    if (!query.exec()) return sqlFailure(query, error);
    if (!inserting && query.numRowsAffected() != 1) return fail(error, QStringLiteral("分组不存在"));
    if (inserting) group->id = query.lastInsertId().toLongLong();
    return true;
}

bool TaskRepository::saveTask(StoredTask *stored, QString *error)
{
    Task &task = stored->task;
    if (!taskcodec::validate(task, error)) return false;
    if (stored->recurrenceAnchorDay < 0 || stored->recurrenceAnchorDay > 31)
        return fail(error, QStringLiteral("月度重复日期锚点非法"));
    if (task.completed != task.completedAt.isValid()) return fail(error, QStringLiteral("完成状态与完成时间不一致"));
    const bool inserting = task.id == 0;
    if (!inserting) {
        QSqlQuery previous(m_database);
        previous.prepare(QStringLiteral("SELECT reminder_at FROM tasks WHERE id=? AND revision=?"));
        previous.addBindValue(task.id);
        previous.addBindValue(stored->revision);
        if (!previous.exec()) return sqlFailure(previous, error);
        if (!previous.next()) return fail(error, QStringLiteral("任务已被其他操作修改，请重新加载"));
        QDateTime oldReminder;
        if (!databaseDate(previous.value(0), &oldReminder, error)) return false;
        if (task.reminderAt != oldReminder) task.notifiedAt = QDateTime();
    }
    QSqlQuery query(m_database);
    const QString columns = QStringLiteral(
        "title,description,content_format,group_id,priority,completed,favorite,pinned,deleted,"
        "start_at,due_at,reminder_at,notified_at,created_at,updated_at,completed_at,recurrence,tags,checklist,"
        "color,geometry,z_order,layer,recurrence_anchor_day");
    const QString sql = inserting
        ? QStringLiteral("INSERT INTO tasks(%1) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)").arg(columns)
        : QStringLiteral("UPDATE tasks SET title=?,description=?,content_format=?,group_id=?,priority=?,completed=?,favorite=?,pinned=?,deleted=?,"
                         "start_at=?,due_at=?,reminder_at=?,notified_at=?,created_at=?,updated_at=?,completed_at=?,recurrence=?,tags=?,checklist=?,"
                         "color=?,geometry=?,z_order=?,layer=?,recurrence_anchor_day=?,revision=revision+1 WHERE id=? AND revision=?");
    if (!query.prepare(sql)) return sqlFailure(query, error);
    query.addBindValue(safeText(task.title));
    query.addBindValue(safeText(task.description));
    query.addBindValue(static_cast<int>(task.contentFormat));
    query.addBindValue(task.groupId ? QVariant(task.groupId) : QVariant(QVariant::LongLong));
    query.addBindValue(task.priority);
    query.addBindValue(task.completed ? 1 : 0);
    query.addBindValue(task.favorite ? 1 : 0);
    query.addBindValue(task.pinned ? 1 : 0);
    query.addBindValue(task.deleted ? 1 : 0);
    query.addBindValue(dateValue(task.startAt));
    query.addBindValue(dateValue(task.dueAt));
    query.addBindValue(dateValue(task.reminderAt));
    query.addBindValue(dateValue(task.notifiedAt));
    query.addBindValue(dateValue(task.createdAt));
    query.addBindValue(dateValue(task.updatedAt));
    query.addBindValue(dateValue(task.completedAt));
    query.addBindValue(task.recurrence);
    query.addBindValue(QString::fromUtf8(QJsonDocument(taskcodec::tagsJson(task.tags)).toJson(QJsonDocument::Compact)));
    query.addBindValue(QString::fromUtf8(QJsonDocument(taskcodec::checklistJson(task.checklist)).toJson(QJsonDocument::Compact)));
    query.addBindValue(colorToInt(task.color));
    query.addBindValue(encodeGeometry(task.geometry));
    query.addBindValue(task.zOrder);
    query.addBindValue(layerToInt(task.layer));
    query.addBindValue(stored->recurrenceAnchorDay);
    if (!inserting) { query.addBindValue(task.id); query.addBindValue(stored->revision); }
    if (!query.exec()) return sqlFailure(query, error);
    if (!inserting && query.numRowsAffected() != 1) return fail(error, QStringLiteral("任务已被其他操作修改，请重新加载"));
    if (inserting) { task.id = query.lastInsertId().toLongLong(); stored->revision = 1; }
    else ++stored->revision;
    return true;
}

bool TaskRepository::removeTask(const StoredTask &task, QString *error)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM tasks WHERE id=? AND revision=?"));
    query.addBindValue(task.task.id);
    query.addBindValue(task.revision);
    if (!query.exec()) return sqlFailure(query, error);
    return query.numRowsAffected() == 1 ? true : fail(error, QStringLiteral("任务不存在或已被其他操作修改"));
}

bool TaskRepository::removeGroup(qint64 id, QString *error)
{
    if (id <= 0) return fail(error, QStringLiteral("不能删除未分组"));
    QSqlQuery move(m_database);
    move.prepare(QStringLiteral("UPDATE tasks SET group_id=NULL,updated_at=?,revision=revision+1 WHERE group_id=?"));
    move.addBindValue(taskcodec::dateText(QDateTime::currentDateTimeUtc()));
    move.addBindValue(id);
    if (!move.exec()) return sqlFailure(move, error);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM task_groups WHERE id=?"));
    remove.addBindValue(id);
    if (!remove.exec()) return sqlFailure(remove, error);
    return remove.numRowsAffected() == 1 ? true : fail(error, QStringLiteral("分组不存在"));
}

bool TaskRepository::insertLink(const RecurrenceLink &link, QString *error)
{
    if (link.sourceId <= 0 || link.nextId < 0 || link.sourceId == link.nextId || link.nextRevision <= 0)
        return fail(error, QStringLiteral("重复任务生成凭证非法"));
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO task_recurrence_links(source_id,next_id,next_revision) VALUES(?,?,?)"));
    query.addBindValue(link.sourceId);
    query.addBindValue(link.nextId ? QVariant(link.nextId) : QVariant(QVariant::LongLong));
    query.addBindValue(link.nextRevision);
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::apply(TaskChanges *changes, TaskSnapshot *after, QString *error)
{
    if (!changes || !after || !ready(error)) return false;
    TaskChanges pending = *changes;
    if (!m_database.transaction()) return fail(error, m_database.lastError().text());
    const auto abort = [this]() { m_database.rollback(); return false; };
    for (qint64 sourceId : pending.removeLinks) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM task_recurrence_links WHERE source_id=?"));
        query.addBindValue(sourceId);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1) { fail(error, QStringLiteral("重复任务生成凭证已变化")); return abort(); }
    }
    for (const StoredTask &task : pending.removals) if (!removeTask(task, error)) return abort();
    for (TaskGroup &group : pending.groupSaves) if (!saveGroup(&group, error)) return abort();
    for (qint64 groupId : pending.groupRemovals) if (!removeGroup(groupId, error)) return abort();
    for (StoredTask &task : pending.saves) if (!saveTask(&task, error)) return abort();
    if (pending.generationSourceId) {
        if (pending.generatedTaskIndex < 0 || pending.generatedTaskIndex >= pending.saves.size()
            || changes->saves.at(pending.generatedTaskIndex).task.id != 0) {
            fail(error, QStringLiteral("重复任务生成参数非法"));
            return abort();
        }
        RecurrenceLink link;
        link.sourceId = pending.generationSourceId;
        link.nextId = pending.saves.at(pending.generatedTaskIndex).task.id;
        link.nextRevision = pending.saves.at(pending.generatedTaskIndex).revision;
        if (!insertLink(link, error)) return abort();
    }
    TaskSnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) return abort();
    if (!m_database.commit()) { fail(error, m_database.lastError().text()); return abort(); }
    *changes = pending;
    *after = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::merge(const TaskSnapshot &incoming, TaskSnapshot *after, QString *error)
{
    if (!after || !ready(error)) return false;
    if (!m_database.transaction()) return fail(error, m_database.lastError().text());
    const auto abort = [this]() { m_database.rollback(); return false; };
    QHash<qint64, qint64> groupIds;
    QHash<qint64, qint64> taskIds;
    QHash<qint64, qint64> originalRevisions;
    for (TaskGroup group : incoming.groups) {
        const qint64 oldId = group.id;
        if (oldId <= 0 || groupIds.contains(oldId)) { fail(error, QStringLiteral("备份分组 id 重复或非法")); return abort(); }
        group.id = 0;
        if (!saveGroup(&group, error)) return abort();
        groupIds.insert(oldId, group.id);
    }
    for (StoredTask task : incoming.tasks) {
        const qint64 oldId = task.task.id;
        if (oldId <= 0 || taskIds.contains(oldId) || (task.task.groupId && !groupIds.contains(task.task.groupId))) {
            fail(error, QStringLiteral("备份任务 id 重复或分组引用非法")); return abort();
        }
        originalRevisions.insert(oldId, task.revision);
        task.task.id = 0;
        task.task.groupId = task.task.groupId ? groupIds.value(task.task.groupId) : 0;
        task.revision = 0;
        if (!saveTask(&task, error)) return abort();
        taskIds.insert(oldId, task.task.id);
    }
    QSet<qint64> sources, nextIds;
    for (const RecurrenceLink &original : incoming.recurrenceLinks) {
        if (!taskIds.contains(original.sourceId) || sources.contains(original.sourceId)
            || original.sourceId == original.nextId
            || (original.nextId && (!taskIds.contains(original.nextId) || nextIds.contains(original.nextId)))) {
            fail(error, QStringLiteral("备份重复任务引用非法")); return abort();
        }
        sources.insert(original.sourceId);
        if (original.nextId) nextIds.insert(original.nextId);
        RecurrenceLink link;
        link.sourceId = taskIds.value(original.sourceId);
        link.nextId = original.nextId ? taskIds.value(original.nextId) : 0;
        // 在原备份的同一 revision 空间比较是否编辑过，再把这层关系映射到
        // 新行的 revision 空间（1=未动，2=已动）。保持撤销保护，不拷贝旧 id。
        if (original.nextId && originalRevisions.value(original.nextId) != original.nextRevision) {
            QSqlQuery mark(m_database);
            mark.prepare(QStringLiteral("UPDATE tasks SET revision=2 WHERE id=?"));
            mark.addBindValue(link.nextId);
            if (!mark.exec()) { sqlFailure(mark, error); return abort(); }
        }
        link.nextRevision = 1;
        if (!insertLink(link, error)) return abort();
    }
    TaskSnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) return abort();
    if (!m_database.commit()) { fail(error, m_database.lastError().text()); return abort(); }
    *after = snapshot;
    if (error) error->clear();
    return true;
}

} // namespace kn
