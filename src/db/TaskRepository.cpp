#include "TaskRepository.h"

#include "../Common.h"
#include "../domain/AttachmentCodec.h"
#include "../domain/EntityCodec.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace kl {
namespace {

QVariant nullMs() { return QVariant(QVariant::LongLong); }
QVariant msValue(const QDateTime &date) { return date.isValid() ? QVariant(utcMs(date)) : nullMs(); }
QVariant nullableText(const QString &text)
{
    return text.isEmpty() ? QVariant(QVariant::String) : QVariant(text);
}
QString safeText(const QString &text) { return text.isNull() ? QStringLiteral("") : text; }

bool dbDate(const QVariant &value, QDateTime *out, QString *error)
{
    if (value.isNull()) { *out = QDateTime(); return true; }
    if (value.type() != QVariant::Int && value.type() != QVariant::LongLong
        && value.type() != QVariant::UInt && value.type() != QVariant::ULongLong)
        return entitycodec::fail(error, QStringLiteral("数据库时间列不是 UTC 毫秒整数"));
    bool ok = false;
    const qint64 ms = value.toLongLong(&ok);
    const QDateTime date = fromUtcMs(ms);
    if (!ok || !date.isValid() || !entitycodec::validDate(date))
        return entitycodec::fail(error, QStringLiteral("数据库时间超出有效范围"));
    *out = date;
    return true;
}
bool dbAllDay(const QVariant &value, QDate *out, QString *error)
{
    if (value.isNull()) { *out = QDate(); return true; }
    const QDate date = QDate::fromString(value.toString(), Qt::ISODate);
    if (!date.isValid() || date.toString(Qt::ISODate) != value.toString())
        return entitycodec::fail(error, QStringLiteral("数据库全天日期非法"));
    *out = date;
    return true;
}
bool sqlFailure(const QSqlQuery &query, QString *error)
{
    return entitycodec::fail(error, QStringLiteral("数据库操作失败: ") + query.lastError().text());
}
QString localValue(const EntitySnapshot &snapshot, const QString &key)
{
    for (const LocalMeta &meta : snapshot.localMeta) if (meta.key == key) return meta.value;
    return QString();
}
// 当前没有远端工作区选择；signed_in 仅保留给未来同步实现。
int syncEnabled(const QSqlDatabase &database, QString *error)
{
    if (error) error->clear();
    QSqlQuery meta(database);
    if (!meta.exec(QStringLiteral("SELECT value FROM local_meta WHERE key='signed_in'"))) {
        sqlFailure(meta, error); return -1;
    }
    return meta.next() && meta.value(0).toString() == QStringLiteral("1") ? 1 : 0;
}
bool validNamedEntity(const QString &id, const QString &name, const QDateTime &created,
                      const QDateTime &updated, int sortOrder, QString *error)
{
    if (!isUuid(id) || name.trimmed().isEmpty() || !created.isValid() || !updated.isValid() || sortOrder < 0)
        return entitycodec::fail(error, QStringLiteral("数据库命名实体损坏"));
    return true;
}

bool validEventType(const QString &type)
{
    const char *const types[] = {"created","changed","kind_changed","trashed","restored","deleted",
        "attachment_added","attachment_removed","reminder_added","reminder_removed"};
    for (const char *candidate : types) if (type == QLatin1String(candidate)) return true;
    return false;
}
bool validEvent(const EntityEvent &event, QString *error)
{
    if (!isUuid(event.id) || !isUuid(event.entityId) || !isUuid(event.batchId)
        || entityKindName(event.entityKind).isEmpty() || event.entityTitle.trimmed().isEmpty()
        || !validEventType(event.type) || !event.occurredAt.isValid() || !entitycodec::validDate(event.occurredAt)
        || (event.oldValue.isValid() && event.oldValue.type() != QVariant::String)
        || (event.newValue.isValid() && event.newValue.type() != QVariant::String))
        return entitycodec::fail(error, QStringLiteral("实体事件字段非法"));
    return true;
}
QVariant eventValueForDb(const QVariant &value)
{
    return value.isValid() && !value.isNull() ? QVariant(safeText(value.toString())) : QVariant(QVariant::String);
}
bool readEventValue(const QVariant &value, QVariant *out, QString *error)
{
    if (value.isNull()) { *out = QVariant(); return true; }
    if (value.type() != QVariant::String) return entitycodec::fail(error, QStringLiteral("数据库事件值不是 TEXT"));
    *out = value; return true;
}
}

TaskRepository::TaskRepository(const QSqlDatabase &database) : m_database(database) {}
bool TaskRepository::ready(QString *error) const
{
    return m_database.isValid() && m_database.isOpen()
        ? true : entitycodec::fail(error, QStringLiteral("KylinList 数据库未打开"));
}

bool TaskRepository::readSnapshot(EntitySnapshot *out, QString *error) const
{
    EntitySnapshot snapshot;
    QSqlQuery meta(m_database);
    if (!meta.exec(QStringLiteral("SELECT key,value FROM local_meta ORDER BY key"))) return sqlFailure(meta, error);
    while (meta.next()) {
        LocalMeta row{meta.value(0).toString(), meta.value(1).toString()};
        if (row.key.isEmpty()) return entitycodec::fail(error, QStringLiteral("local_meta 键为空"));
        snapshot.localMeta.append(row);
    }
    const QString installationId = localValue(snapshot, QStringLiteral("installation_id"));
    const QString deviceId = localValue(snapshot, QStringLiteral("device_id"));
    const QString inboxId = localValue(snapshot, QStringLiteral("inbox_list_id"));
    if (!isUuid(installationId) || !isUuid(deviceId) || !isUuid(inboxId)
        || localValue(snapshot, QStringLiteral("default_timezone")).isEmpty())
        return entitycodec::fail(error, QStringLiteral("安装/设备/inbox 元数据缺失"));

    QSqlQuery filters(m_database);
    if (!filters.exec(QStringLiteral("SELECT id,name,spec_json,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM view_defs ORDER BY sort_order,id")))
        return sqlFailure(filters, error);
    while (filters.next()) {
        ViewDef row; row.id = filters.value(0).toString(); row.name = filters.value(1).toString();
        row.specJson = filters.value(2).toString(); row.sortOrder = filters.value(3).toInt();
        if (!dbDate(filters.value(4), &row.createdAt, error) || !dbDate(filters.value(5), &row.updatedAt, error)) return false;
        row.revision = filters.value(6).toLongLong(); row.serverRevision = filters.value(7).toLongLong();
        QJsonParseError parseError; const QJsonDocument spec = QJsonDocument::fromJson(row.specJson.toUtf8(), &parseError);
        if (!isUuid(row.id) || row.name.trimmed().isEmpty() || row.sortOrder < 0
            || !row.createdAt.isValid() || !row.updatedAt.isValid() || row.revision <= 0 || row.serverRevision < 0
            || parseError.error != QJsonParseError::NoError || !spec.isObject())
            return entitycodec::fail(error, QStringLiteral("view definition 损坏"));
        snapshot.viewDefs.append(row);
    }

    QSqlQuery folders(m_database);
    if (!folders.exec(QStringLiteral("SELECT id,name,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM folders ORDER BY sort_order,id")))
        return sqlFailure(folders, error);
    QSet<QString> folderIds;
    while (folders.next()) {
        Folder row;
        row.id = folders.value(0).toString(); row.name = folders.value(1).toString(); row.sortOrder = folders.value(2).toInt();
        row.revision = folders.value(5).toLongLong(); row.serverRevision = folders.value(6).toLongLong();
        if (!dbDate(folders.value(3), &row.createdAt, error) || !dbDate(folders.value(4), &row.updatedAt, error)
            || row.revision <= 0 || row.serverRevision < 0 || !validNamedEntity(row.id, row.name, row.createdAt, row.updatedAt, row.sortOrder, error)) return false;
        folderIds.insert(row.id); snapshot.folders.append(row);
    }

    QSqlQuery lists(m_database);
    if (!lists.exec(QStringLiteral("SELECT id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM lists ORDER BY sort_order,id")))
        return sqlFailure(lists, error);
    QSet<QString> listIds;
    QHash<QString, bool> listInbox;
    int inboxCount = 0;
    while (lists.next()) {
        TaskList row;
        row.id = lists.value(0).toString(); row.folderId = lists.value(1).toString();
        row.kind = entityKindFromName(lists.value(2).toString());
        row.name = lists.value(3).toString(); row.inbox = lists.value(4).toInt() != 0; row.sortOrder = lists.value(5).toInt();
        row.revision = lists.value(8).toLongLong(); row.serverRevision = lists.value(9).toLongLong();
        if (!dbDate(lists.value(6), &row.createdAt, error) || !dbDate(lists.value(7), &row.updatedAt, error)
            || row.revision <= 0 || row.serverRevision < 0 || !validNamedEntity(row.id, row.name, row.createdAt, row.updatedAt, row.sortOrder, error)
            || entityKindName(row.kind) != lists.value(2).toString()
            || (!row.folderId.isEmpty() && !folderIds.contains(row.folderId)))
            return entitycodec::fail(error, QStringLiteral("清单或文件夹引用损坏"));
        if (row.inbox) ++inboxCount;
        listIds.insert(row.id); listInbox.insert(row.id, row.inbox); snapshot.lists.append(row);
    }
    if (inboxCount != 1 || !listInbox.value(inboxId, false))
        return entitycodec::fail(error, QStringLiteral("默认 inbox 不存在或不唯一"));

    QSqlQuery sections(m_database);
    if (!sections.exec(QStringLiteral("SELECT id,list_id,name,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM sections ORDER BY sort_order,id")))
        return sqlFailure(sections, error);
    QHash<QString, QString> sectionLists;
    while (sections.next()) {
        Section row;
        row.id = sections.value(0).toString(); row.listId = sections.value(1).toString();
        row.name = sections.value(2).toString(); row.sortOrder = sections.value(3).toInt(); row.revision = sections.value(6).toLongLong();
        row.serverRevision = sections.value(7).toLongLong();
        if (!dbDate(sections.value(4), &row.createdAt, error) || !dbDate(sections.value(5), &row.updatedAt, error)
            || row.revision <= 0 || row.serverRevision < 0 || !validNamedEntity(row.id, row.name, row.createdAt, row.updatedAt, row.sortOrder, error)
            || !listIds.contains(row.listId))
            return entitycodec::fail(error, QStringLiteral("section 或清单引用损坏"));
        sectionLists.insert(row.id, row.listId); snapshot.sections.append(row);
    }

    QSqlQuery seriesQuery(m_database);
    if (!seriesQuery.exec(QStringLiteral("SELECT id,rrule,anchor_day,timezone,dtstart_utc_ms,dtstart_date,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM recurrence_series ORDER BY id")))
        return sqlFailure(seriesQuery, error);
    QHash<QString, RecurrenceSeries> seriesById;
    while (seriesQuery.next()) {
        RecurrenceSeries row;
        row.id = seriesQuery.value(0).toString(); row.rrule = seriesQuery.value(1).toString();
        row.anchorDay = seriesQuery.value(2).toInt(); row.timeZone = seriesQuery.value(3).toString(); row.revision = seriesQuery.value(8).toLongLong();
        row.serverRevision = seriesQuery.value(9).toLongLong();
        if (!dbDate(seriesQuery.value(4), &row.dtstartAt, error) || !dbAllDay(seriesQuery.value(5), &row.dtstartDate, error)
            || !dbDate(seriesQuery.value(6), &row.createdAt, error) || !dbDate(seriesQuery.value(7), &row.updatedAt, error)) return false;
        if (!isUuid(row.id) || recurrenceRRule(row.rrule) != row.rrule || row.anchorDay < 0 || row.anchorDay > 31
            || row.revision <= 0 || row.serverRevision < 0 || !entitycodec::validTimeZone(row.timeZone) || !row.createdAt.isValid() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("重复 series 损坏"));
        seriesById.insert(row.id, row); snapshot.recurrenceSeries.append(row);
    }

    QSqlQuery entities(m_database);
    if (!entities.exec(QStringLiteral(
            "SELECT id,kind,title,body_markdown,folder_id,list_id,section_id,parent_id,sort_order,status,"
            "deleted_at_utc_ms,start_at_utc_ms,due_at_utc_ms,all_day_date,timezone,recurrence_series_id,occurrence_key,"
            "priority,pinned_at_utc_ms,created_at_utc_ms,updated_at_utc_ms,completed_at_utc_ms,abandoned_at_utc_ms,revision,server_revision "
            "FROM entities ORDER BY created_at_utc_ms,id"))) return sqlFailure(entities, error);
    QHash<QString, int> indexes;
    while (entities.next()) {
        StoredEntity stored;
        Entity &task = stored.entity;
        task.id = entities.value(0).toString(); task.kind = entityKindFromName(entities.value(1).toString());
        task.title = entities.value(2).toString(); task.body = entities.value(3).toString();
        task.folderId = entities.value(4).toString(); task.listId = entities.value(5).toString();
        task.sectionId = entities.value(6).toString(); task.parentId = entities.value(7).toString();
        task.sortOrder = entities.value(8).toLongLong(); task.status = taskStatusFromName(entities.value(9).toString());
        task.priority = entities.value(17).toInt();
        if (entityKindName(task.kind) != entities.value(1).toString()) return entitycodec::fail(error, QStringLiteral("实体 kind 损坏"));
        if (taskStatusName(task.status) != entities.value(9).toString()) return entitycodec::fail(error, QStringLiteral("任务状态损坏"));
        task.completed = task.status == TaskStatus::Completed;
        QDateTime *const dates[] = {&task.deletedAt, &task.startAt, &task.dueAt, &task.pinnedAt,
                                   &task.createdAt, &task.updatedAt, &task.completedAt, &task.abandonedAt};
        const int columns[] = {10, 11, 12, 18, 19, 20, 21, 22};
        for (int i = 0; i < 8; ++i) if (!dbDate(entities.value(columns[i]), dates[i], error)) return false;
        task.deleted = task.deletedAt.isValid();
        if (!dbAllDay(entities.value(13), &task.allDayDate, error)) return false;
        task.timeZone = entities.value(14).toString(); task.recurrenceSeriesId = entities.value(15).toString();
        task.occurrenceKey = entities.value(16).toString(); stored.revision = entities.value(23).toLongLong();
        stored.serverRevision = entities.value(24).toLongLong();
        if (!task.recurrenceSeriesId.isEmpty()) {
            if (!seriesById.contains(task.recurrenceSeriesId))
                return entitycodec::fail(error, QStringLiteral("任务 series 引用损坏"));
            const RecurrenceSeries series = seriesById.value(task.recurrenceSeriesId);
            task.rrule = series.rrule; task.recurrence = recurrenceName(series.rrule); stored.recurrenceAnchorDay = series.anchorDay;
        } else {
            task.recurrence = QStringLiteral("none");
        }
        if (!task.createdAt.isValid() || !task.updatedAt.isValid() || stored.revision <= 0 || stored.serverRevision < 0
            || !listIds.contains(task.listId) || (!task.folderId.isEmpty() && !folderIds.contains(task.folderId))
            || (!task.sectionId.isEmpty() && (!sectionLists.contains(task.sectionId) || sectionLists.value(task.sectionId) != task.listId))
            || !entitycodec::validate(task, error)) return false;
        indexes.insert(task.id, snapshot.entities.size()); snapshot.entities.append(stored);
    }
    for (const StoredEntity &stored : snapshot.entities)
        if (!stored.entity.parentId.isEmpty() && !indexes.contains(stored.entity.parentId))
            return entitycodec::fail(error, QStringLiteral("子任务父引用损坏"));
    // 真正的子任务不允许出现环。
    for (const StoredEntity &stored : snapshot.entities) {
        QSet<QString> path;
        QString id = stored.entity.id;
        while (!id.isEmpty()) {
            if (path.contains(id)) return entitycodec::fail(error, QStringLiteral("子任务关系形成循环"));
            path.insert(id);
            const int index = indexes.value(id, -1);
            id = index >= 0 ? snapshot.entities.at(index).entity.parentId : QString();
        }
    }

    QSqlQuery checklist(m_database);
    if (!checklist.exec(QStringLiteral("SELECT id,entity_id,text,done,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM checklist_items ORDER BY entity_id,sort_order,id")))
        return sqlFailure(checklist, error);
    QSet<QString> checklistIds;
    while (checklist.next()) {
        ChecklistItem row;
        row.id = checklist.value(0).toString(); row.entityId = checklist.value(1).toString(); row.text = checklist.value(2).toString();
        row.done = checklist.value(3).toInt() != 0; row.sortOrder = checklist.value(4).toInt(); row.revision = checklist.value(7).toLongLong();
        row.serverRevision = checklist.value(8).toLongLong();
        if (!dbDate(checklist.value(5), &row.createdAt, error) || !dbDate(checklist.value(6), &row.updatedAt, error)) return false;
        const int index = indexes.value(row.entityId, -1);
        if (index < 0 || checklistIds.contains(row.id) || !isUuid(row.id) || row.sortOrder < 0 || row.revision <= 0
            || !row.createdAt.isValid() || !row.updatedAt.isValid()) return entitycodec::fail(error, QStringLiteral("独立清单项损坏"));
        checklistIds.insert(row.id); snapshot.checklistItems.append(row); snapshot.entities[index].entity.checklist.append(row);
    }

    QSqlQuery tags(m_database);
    if (!tags.exec(QStringLiteral("SELECT id,name,normalized_name,color,revision,server_revision FROM tags ORDER BY name,id"))) return sqlFailure(tags, error);
    QHash<QString, Tag> tagsById;
    QSet<QString> tagNames;
    while (tags.next()) {
        Tag row; row.id = tags.value(0).toString();
        row.name = tags.value(1).toString(); row.normalizedName = tags.value(2).toString(); row.color = tags.value(3).toString();
        row.revision = tags.value(4).toLongLong(); row.serverRevision = tags.value(5).toLongLong();
        if (!isUuid(row.id) || row.name.trimmed().isEmpty() || row.normalizedName != row.name.trimmed().toLower()
            || row.revision <= 0 || row.serverRevision < 0 || tagNames.contains(row.normalizedName)) return entitycodec::fail(error, QStringLiteral("标签损坏"));
        tagNames.insert(row.normalizedName); tagsById.insert(row.id, row); snapshot.tags.append(row);
    }
    QSqlQuery taskTags(m_database);
    if (!taskTags.exec(QStringLiteral("SELECT id,entity_id,tag_id,revision,server_revision FROM entity_tags ORDER BY entity_id,tag_id,id"))) return sqlFailure(taskTags, error);
    while (taskTags.next()) {
        TaskTag row; row.id = taskTags.value(0).toString();
        row.entityId = taskTags.value(1).toString(); row.tagId = taskTags.value(2).toString(); row.revision = taskTags.value(3).toLongLong();
        row.serverRevision = taskTags.value(4).toLongLong();
        const int index = indexes.value(row.entityId, -1);
        if (index < 0 || !isUuid(row.id) || row.revision <= 0 || row.serverRevision < 0 || !tagsById.contains(row.tagId))
            return entitycodec::fail(error, QStringLiteral("任务标签引用损坏"));
        snapshot.taskTags.append(row); snapshot.entities[index].entity.tags.append(tagsById.value(row.tagId).name);
    }

    QSqlQuery reminders(m_database);
    if (!reminders.exec(QStringLiteral("SELECT id,entity_id,trigger_at_utc_ms,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision FROM reminders ORDER BY entity_id,sort_order,trigger_at_utc_ms,id")))
        return sqlFailure(reminders, error);
    QHash<QString, int> reminderTasks;
    while (reminders.next()) {
        TaskReminder row;
        row.id = reminders.value(0).toString(); row.entityId = reminders.value(1).toString(); row.sortOrder = reminders.value(3).toInt();
        row.revision = reminders.value(6).toLongLong(); row.serverRevision = reminders.value(7).toLongLong();
        if (!dbDate(reminders.value(2), &row.triggerAt, error) || !dbDate(reminders.value(4), &row.createdAt, error)
            || !dbDate(reminders.value(5), &row.updatedAt, error)) return false;
        const int index = indexes.value(row.entityId, -1);
        if (index < 0 || reminderTasks.contains(row.id) || !isUuid(row.id) || !row.triggerAt.isValid()
            || row.sortOrder < 0 || row.revision <= 0 || row.serverRevision < 0 || !row.createdAt.isValid() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("提醒记录损坏"));
        reminderTasks.insert(row.id, index); snapshot.reminders.append(row); snapshot.entities[index].entity.reminders.append(row);
        if (!snapshot.entities[index].entity.reminderAt.isValid() || row.triggerAt < snapshot.entities[index].entity.reminderAt)
            snapshot.entities[index].entity.reminderAt = row.triggerAt;
    }
    QSqlQuery deliveries(m_database);
    if (!deliveries.exec(QStringLiteral("SELECT reminder_id,installation_id,scheduled_at_utc_ms,delivered_at_utc_ms FROM reminder_deliveries ORDER BY reminder_id,installation_id")))
        return sqlFailure(deliveries, error);
    while (deliveries.next()) {
        ReminderDelivery row;
        row.reminderId = deliveries.value(0).toString(); row.installationId = deliveries.value(1).toString();
        if (!dbDate(deliveries.value(2), &row.scheduledAt, error) || !dbDate(deliveries.value(3), &row.deliveredAt, error)) return false;
        const int index = reminderTasks.value(row.reminderId, -1);
        if (index < 0 || !isUuid(row.installationId) || !row.scheduledAt.isValid() || !row.deliveredAt.isValid()
            || row.deliveredAt < row.scheduledAt)
            return entitycodec::fail(error, QStringLiteral("提醒投递记录损坏"));
        bool triggerMatches = false;
        for (const TaskReminder &reminder : snapshot.entities.at(index).entity.reminders)
            if (reminder.id == row.reminderId && reminder.triggerAt == row.scheduledAt) triggerMatches = true;
        if (!triggerMatches) return entitycodec::fail(error, QStringLiteral("提醒投递与当前计划时间不一致"));
        snapshot.reminderDeliveries.append(row);
        if (row.installationId == installationId) {
            for (TaskReminder &reminder : snapshot.entities[index].entity.reminders)
                if (reminder.id == row.reminderId) reminder.deliveredAt = row.deliveredAt;
        }
    }
    for (StoredEntity &stored : snapshot.entities) {
        const int primary = primaryReminderIndex(stored.entity);
        stored.entity.reminderAt = primary >= 0 ? stored.entity.reminders.at(primary).triggerAt : QDateTime();
        stored.entity.notifiedAt = primary >= 0 ? stored.entity.reminders.at(primary).deliveredAt : QDateTime();
    }

    QSqlQuery files(m_database);
    if (!files.exec(QStringLiteral("SELECT id,entity_id,original_name,relative_path,mime,size,sha256,is_image,local_state,created_at_utc_ms,verified_at_utc_ms,sort_order,revision,server_revision FROM attachments ORDER BY entity_id,sort_order,id")))
        return sqlFailure(files, error);
    QSet<QString> attachmentIds, attachmentPaths;
    while (files.next()) {
        Attachment row;
        row.id = files.value(0).toString(); row.entityId = files.value(1).toString(); row.originalName = files.value(2).toString();
        row.relativePath = files.value(3).toString(); row.mime = files.value(4).toString(); row.size = files.value(5).toLongLong();
        row.sha256 = files.value(6).toString(); row.isImage = files.value(7).toInt() != 0;
        row.localState = attachmentFileStateFromName(files.value(8).toString()); row.sortOrder = files.value(11).toInt();
        row.revision = files.value(12).toLongLong(); row.serverRevision = files.value(13).toLongLong();
        if (!dbDate(files.value(9), &row.createdAt, error) || !dbDate(files.value(10), &row.verifiedAt, error)) return false;
        if (!indexes.contains(row.entityId) || attachmentIds.contains(row.id) || attachmentPaths.contains(row.relativePath)
            || row.revision <= 0 || row.serverRevision < 0 || attachmentFileStateName(row.localState) != files.value(8).toString()
            || !attachmentcodec::validateAttachment(row, error)) return false;
        attachmentIds.insert(row.id); attachmentPaths.insert(row.relativePath); snapshot.attachments.append(row);
    }

    QSqlQuery notes(m_database);
    if (!notes.exec(QStringLiteral("SELECT id,entity_id,device_id,color,geometry,z_order,layer,open,created_at_utc_ms,updated_at_utc_ms FROM desktop_notes ORDER BY device_id,entity_id")))
        return sqlFailure(notes, error);
    while (notes.next()) {
        DesktopNote row;
        row.id = notes.value(0).toString(); row.entityId = notes.value(1).toString(); row.deviceId = notes.value(2).toString();
        row.color = colorFromInt(notes.value(3).toInt()); row.geometry = decodeGeometry(notes.value(4).toString());
        row.zOrder = notes.value(5).toInt(); row.layer = layerFromInt(notes.value(6).toInt()); row.open = notes.value(7).toInt() != 0;
        if (!dbDate(notes.value(8), &row.createdAt, error) || !dbDate(notes.value(9), &row.updatedAt, error)) return false;
        const int index = indexes.value(row.entityId, -1);
        if (index < 0 || !isUuid(row.id) || !isUuid(row.deviceId) || !row.createdAt.isValid() || !row.updatedAt.isValid()
            || colorToInt(row.color) != notes.value(3).toInt() || layerToInt(row.layer) != notes.value(6).toInt())
            return entitycodec::fail(error, QStringLiteral("桌面便签状态损坏"));
        snapshot.desktopNotes.append(row);
        if (row.deviceId == deviceId) {
            Entity &task = snapshot.entities[index].entity;
            task.color = row.color; task.geometry = row.geometry; task.zOrder = row.zOrder; task.layer = row.layer;
        }
    }

    QSqlQuery links(m_database);
    if (!links.exec(QStringLiteral("SELECT id,source_id,next_id,next_revision,revision,server_revision FROM recurrence_links ORDER BY source_id"))) {
        // 表在 schema 中创建；这里仍保留明确错误。
        return sqlFailure(links, error);
    }
    QSet<QString> linkTargets;
    while (links.next()) {
        RecurrenceLink row; row.id = links.value(0).toString();
        row.sourceId = links.value(1).toString(); row.nextId = links.value(2).toString(); row.nextRevision = links.value(3).toLongLong();
        row.revision = links.value(4).toLongLong(); row.serverRevision = links.value(5).toLongLong();
        const int source = indexes.value(row.sourceId, -1), next = indexes.value(row.nextId, -1);
        if (!isUuid(row.id) || source < 0 || snapshot.entities.at(source).entity.status != TaskStatus::Completed || row.nextRevision <= 0
            || row.revision <= 0
            || (!row.nextId.isEmpty() && (next < 0 || linkTargets.contains(row.nextId)
                                         || row.nextRevision > snapshot.entities.at(next).revision)))
            return entitycodec::fail(error, QStringLiteral("重复生成凭证损坏"));
        if (!row.nextId.isEmpty()) linkTargets.insert(row.nextId);
        snapshot.recurrenceLinks.append(row);
    }

    QSqlQuery bindings(m_database);
    if (!bindings.exec(QStringLiteral("SELECT id,entity_type,entity_id,remote_id,updated_at_utc_ms FROM sync_bindings ORDER BY id")))
        return sqlFailure(bindings, error);
    while (bindings.next()) {
        SyncBinding row; row.id = bindings.value(0).toString();
        row.entityType = bindings.value(1).toString(); row.entityId = bindings.value(2).toString(); row.remoteId = bindings.value(3).toString();
        if (!dbDate(bindings.value(4), &row.updatedAt, error) || !isUuid(row.id)
            || row.entityType.isEmpty() || !isUuid(row.entityId) || row.remoteId.isEmpty() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("sync binding 损坏"));
        snapshot.syncBindings.append(row);
    }
    QSqlQuery tombstones(m_database);
    if (!tombstones.exec(QStringLiteral("SELECT id,entity_type,entity_id,deleted_at_utc_ms FROM tombstones ORDER BY deleted_at_utc_ms,id")))
        return sqlFailure(tombstones, error);
    while (tombstones.next()) {
        Tombstone row; row.id = tombstones.value(0).toString();
        row.entityType = tombstones.value(1).toString(); row.entityId = tombstones.value(2).toString();
        if (!dbDate(tombstones.value(3), &row.deletedAt, error) || !isUuid(row.id) || !isUuid(row.entityId)
            || row.entityType.isEmpty() || !row.deletedAt.isValid()) return entitycodec::fail(error, QStringLiteral("tombstone 损坏"));
        snapshot.tombstones.append(row);
    }
    QSqlQuery outbox(m_database);
    if (!outbox.exec(QStringLiteral("SELECT id,batch_id,base_server_revision,entity_type,entity_id,operation,payload_json,created_at_utc_ms FROM outbox ORDER BY created_at_utc_ms,id")))
        return sqlFailure(outbox, error);
    while (outbox.next()) {
        OutboxItem row; row.id = outbox.value(0).toString();
        row.batchId = outbox.value(1).toString(); row.baseServerRevision = outbox.value(2).toLongLong();
        row.entityType = outbox.value(3).toString(); row.entityId = outbox.value(4).toString(); row.operation = outbox.value(5).toString();
        row.payloadJson = outbox.value(6).toString();
        if (!dbDate(outbox.value(7), &row.createdAt, error) || !isUuid(row.id) || !isUuid(row.entityId)
            || !isUuid(row.batchId) || row.baseServerRevision < 0
            || row.entityType.isEmpty() || (row.operation != QStringLiteral("upsert") && row.operation != QStringLiteral("delete"))
            || !row.createdAt.isValid()) return entitycodec::fail(error, QStringLiteral("outbox 损坏"));
        snapshot.outbox.append(row);
    }
    *out = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::load(EntitySnapshot *out, QString *error) const
{
    if (!out || !ready(error)) return false;
    QSqlDatabase database = m_database;
    if (!database.transaction()) return entitycodec::fail(error, database.lastError().text());
    EntitySnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) { database.rollback(); return false; }
    if (!database.commit()) { database.rollback(); return entitycodec::fail(error, database.lastError().text()); }
    *out = snapshot;
    return true;
}

bool TaskRepository::saveFolder(Folder *folder, QString *error)
{
    if (!isUuid(folder->id) || folder->name.trimmed().isEmpty() || folder->sortOrder < 0
        || !folder->createdAt.isValid() || !folder->updatedAt.isValid()) return entitycodec::fail(error, QStringLiteral("文件夹字段非法"));
    folder->name = folder->name.trimmed();
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT 1 FROM folders WHERE id=?")); exists.addBindValue(folder->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE folders SET name=?,sort_order=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO folders(id,name,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(folder->name); query.addBindValue(folder->sortOrder);
        query.addBindValue(utcMs(folder->updatedAt)); query.addBindValue(folder->id);
    } else {
        query.addBindValue(folder->id); query.addBindValue(folder->name);
        query.addBindValue(folder->sortOrder); query.addBindValue(utcMs(folder->createdAt)); query.addBindValue(utcMs(folder->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveGroup(TaskList *group, QString *error)
{
    if (!isUuid(group->id) || entityKindName(group->kind).isEmpty() || group->name.trimmed().isEmpty() || group->sortOrder < 0
        || !group->createdAt.isValid() || !group->updatedAt.isValid() || (group->inbox && group->kind != EntityKind::Task)
        || (!group->folderId.isEmpty() && !isUuid(group->folderId))) return entitycodec::fail(error, QStringLiteral("清单字段非法"));
    group->name = group->name.trimmed();
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT is_inbox FROM lists WHERE id=?")); exists.addBindValue(group->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    if (update && (exists.value(0).toInt() != 0) != group->inbox)
        return entitycodec::fail(error, QStringLiteral("清单不可改变 inbox 属性"));
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE lists SET folder_id=?,kind=?,name=?,sort_order=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(nullableText(group->folderId)); query.addBindValue(entityKindName(group->kind));
        query.addBindValue(group->name); query.addBindValue(group->sortOrder);
        query.addBindValue(utcMs(group->updatedAt)); query.addBindValue(group->id);
    } else {
        query.addBindValue(group->id); query.addBindValue(nullableText(group->folderId)); query.addBindValue(entityKindName(group->kind));
        query.addBindValue(group->name); query.addBindValue(group->inbox ? 1 : 0); query.addBindValue(group->sortOrder);
        query.addBindValue(utcMs(group->createdAt)); query.addBindValue(utcMs(group->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveSection(Section *section, QString *error)
{
    if (!isUuid(section->id) || !isUuid(section->listId) || section->name.trimmed().isEmpty() || section->sortOrder < 0
        || !section->createdAt.isValid() || !section->updatedAt.isValid()) return entitycodec::fail(error, QStringLiteral("section 字段非法"));
    section->name = section->name.trimmed();
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT 1 FROM sections WHERE id=?")); exists.addBindValue(section->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE sections SET list_id=?,name=?,sort_order=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO sections(id,list_id,name,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(section->listId); query.addBindValue(section->name); query.addBindValue(section->sortOrder);
        query.addBindValue(utcMs(section->updatedAt)); query.addBindValue(section->id);
    } else {
        query.addBindValue(section->id); query.addBindValue(section->listId); query.addBindValue(section->name);
        query.addBindValue(section->sortOrder); query.addBindValue(utcMs(section->createdAt)); query.addBindValue(utcMs(section->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveSeries(RecurrenceSeries *series, QString *error)
{
    if (!isUuid(series->id) || recurrenceRRule(series->rrule) != series->rrule || series->anchorDay < 0
        || series->anchorDay > 31 || !entitycodec::validTimeZone(series->timeZone)
        || !series->createdAt.isValid() || !series->updatedAt.isValid())
        return entitycodec::fail(error, QStringLiteral("重复 series 字段非法"));
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT 1 FROM recurrence_series WHERE id=?")); exists.addBindValue(series->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE recurrence_series SET rrule=?,anchor_day=?,timezone=?,dtstart_utc_ms=?,dtstart_date=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO recurrence_series(id,rrule,anchor_day,timezone,dtstart_utc_ms,dtstart_date,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(series->rrule); query.addBindValue(series->anchorDay); query.addBindValue(series->timeZone);
        query.addBindValue(msValue(series->dtstartAt)); query.addBindValue(nullableText(series->dtstartDate.isValid() ? series->dtstartDate.toString(Qt::ISODate) : QString()));
        query.addBindValue(utcMs(series->updatedAt)); query.addBindValue(series->id);
    } else {
        query.addBindValue(series->id); query.addBindValue(series->rrule); query.addBindValue(series->anchorDay); query.addBindValue(series->timeZone);
        query.addBindValue(msValue(series->dtstartAt)); query.addBindValue(nullableText(series->dtstartDate.isValid() ? series->dtstartDate.toString(Qt::ISODate) : QString()));
        query.addBindValue(utcMs(series->createdAt)); query.addBindValue(utcMs(series->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveTask(StoredEntity *stored, QString *error)
{
    Entity &task = stored->entity;
    if (!isUuid(task.id) || !isUuid(task.listId)
        || !task.createdAt.isValid() || !task.updatedAt.isValid() || !entitycodec::validate(task, error)) return false;
    if (!task.parentId.isEmpty()) {
        QSqlQuery cycle(m_database);
        cycle.prepare(QStringLiteral("WITH RECURSIVE ancestors(id,parent_id) AS ("
            "SELECT id,parent_id FROM entities WHERE id=? UNION ALL "
            "SELECT t.id,t.parent_id FROM entities t JOIN ancestors a ON t.id=a.parent_id) "
            "SELECT 1 FROM ancestors WHERE id=? LIMIT 1"));
        cycle.addBindValue(task.parentId); cycle.addBindValue(task.id);
        if (!cycle.exec()) return sqlFailure(cycle, error);
        if (cycle.next()) return entitycodec::fail(error, QStringLiteral("子任务关系不能形成循环"));
    }
    const bool inserting = stored->revision == 0;
    QSqlQuery query(m_database);
    if (!query.prepare(inserting
            ? QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,folder_id,list_id,section_id,parent_id,sort_order,status,"
                "deleted_at_utc_ms,start_at_utc_ms,due_at_utc_ms,all_day_date,timezone,recurrence_series_id,occurrence_key,priority,"
                "pinned_at_utc_ms,created_at_utc_ms,updated_at_utc_ms,completed_at_utc_ms,abandoned_at_utc_ms) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")
            : QStringLiteral("UPDATE entities SET kind=?,title=?,body_markdown=?,folder_id=?,list_id=?,section_id=?,parent_id=?,sort_order=?,status=?,"
                "deleted_at_utc_ms=?,start_at_utc_ms=?,due_at_utc_ms=?,all_day_date=?,timezone=?,recurrence_series_id=?,occurrence_key=?,priority=?,"
                "pinned_at_utc_ms=?,created_at_utc_ms=?,updated_at_utc_ms=?,completed_at_utc_ms=?,abandoned_at_utc_ms=?,revision=revision+1 "
                "WHERE id=? AND revision=?"))) return sqlFailure(query, error);
    if (inserting) query.addBindValue(task.id);
    query.addBindValue(entityKindName(task.kind)); query.addBindValue(safeText(task.title)); query.addBindValue(safeText(task.body));
    query.addBindValue(nullableText(task.folderId)); query.addBindValue(task.listId); query.addBindValue(nullableText(task.sectionId));
    query.addBindValue(nullableText(task.parentId)); query.addBindValue(task.sortOrder); query.addBindValue(taskStatusName(task.status));
    query.addBindValue(msValue(task.deletedAt)); query.addBindValue(msValue(task.startAt)); query.addBindValue(msValue(task.dueAt));
    query.addBindValue(nullableText(task.allDayDate.isValid() ? task.allDayDate.toString(Qt::ISODate) : QString()));
    query.addBindValue(task.timeZone); query.addBindValue(nullableText(task.recurrenceSeriesId)); query.addBindValue(nullableText(task.occurrenceKey));
    query.addBindValue(task.priority); query.addBindValue(msValue(task.pinnedAt));
    query.addBindValue(utcMs(task.createdAt)); query.addBindValue(utcMs(task.updatedAt));
    query.addBindValue(msValue(task.completedAt)); query.addBindValue(msValue(task.abandonedAt));
    if (!inserting) { query.addBindValue(task.id); query.addBindValue(stored->revision); }
    if (!query.exec()) return sqlFailure(query, error);
    if (!inserting && query.numRowsAffected() != 1) return entitycodec::fail(error, QStringLiteral("任务已被其他操作修改，请重新加载"));
    stored->revision = inserting ? 1 : stored->revision + 1;
    return true;
}

bool TaskRepository::touchTask(const StoredEntity &stored, QString *error)
{
    if (!isUuid(stored.entity.id) || stored.revision <= 0)
        return entitycodec::fail(error, QStringLiteral("关联任务修订信息非法"));
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE entities SET revision=revision+1,updated_at_utc_ms=? WHERE id=? AND revision=?"));
    query.addBindValue(utcMs(utcNow())); query.addBindValue(stored.entity.id); query.addBindValue(stored.revision);
    if (!query.exec()) return sqlFailure(query, error);
    if (query.numRowsAffected() != 1) return entitycodec::fail(error, QStringLiteral("关联任务已被其他操作修改，请重新加载"));
    return true;
}

bool TaskRepository::removeTask(const StoredEntity &stored, QString *error)
{
    QSqlQuery query(m_database); query.prepare(QStringLiteral("DELETE FROM entities WHERE id=? AND revision=?"));
    query.addBindValue(stored.entity.id); query.addBindValue(stored.revision);
    if (!query.exec()) return sqlFailure(query, error);
    if (query.numRowsAffected() != 1) return entitycodec::fail(error, QStringLiteral("任务不存在或已被修改"));
    return tombstone(QStringLiteral("task"), stored.entity.id, error);
}

bool TaskRepository::saveChecklist(ChecklistItem *item, QString *error)
{
    if (!isUuid(item->id) || !isUuid(item->entityId) || item->sortOrder < 0 || !item->createdAt.isValid() || !item->updatedAt.isValid())
        return entitycodec::fail(error, QStringLiteral("清单项字段非法"));
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT entity_id FROM checklist_items WHERE id=?")); exists.addBindValue(item->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    if (update && exists.value(0).toString() != item->entityId)
        return entitycodec::fail(error, QStringLiteral("不能移动清单项到其他实体"));
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE checklist_items SET text=?,done=?,sort_order=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO checklist_items(id,entity_id,text,done,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(safeText(item->text)); query.addBindValue(item->done ? 1 : 0); query.addBindValue(item->sortOrder);
        query.addBindValue(utcMs(item->updatedAt)); query.addBindValue(item->id);
    } else {
        query.addBindValue(item->id); query.addBindValue(item->entityId); query.addBindValue(safeText(item->text));
        query.addBindValue(item->done ? 1 : 0); query.addBindValue(item->sortOrder); query.addBindValue(utcMs(item->createdAt)); query.addBindValue(utcMs(item->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveTag(Tag *tag, QString *error)
{
    if (!isUuid(tag->id) || tag->name.trimmed().isEmpty()) return entitycodec::fail(error, QStringLiteral("标签字段非法"));
    tag->name = tag->name.trimmed(); tag->normalizedName = tag->name.toLower();
    QSqlQuery query(m_database); query.prepare(QStringLiteral("INSERT INTO tags(id,name,normalized_name,color) VALUES(?,?,?,?)"));
    query.addBindValue(tag->id); query.addBindValue(tag->name); query.addBindValue(tag->normalizedName); query.addBindValue(safeText(tag->color));
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveReminder(TaskReminder *reminder, QString *error)
{
    if (!isUuid(reminder->id) || !isUuid(reminder->entityId) || !reminder->triggerAt.isValid() || reminder->sortOrder < 0
        || !reminder->createdAt.isValid() || !reminder->updatedAt.isValid()) return entitycodec::fail(error, QStringLiteral("提醒字段非法"));
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT entity_id,trigger_at_utc_ms FROM reminders WHERE id=?")); exists.addBindValue(reminder->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    const bool update = exists.next();
    if (update && exists.value(0).toString() != reminder->entityId)
        return entitycodec::fail(error, QStringLiteral("不能移动提醒到其他实体"));
    if (update) {
        QDateTime oldTrigger; if (!dbDate(exists.value(1), &oldTrigger, error)) return false;
        if (oldTrigger != reminder->triggerAt) {
            QSqlQuery clear(m_database); clear.prepare(QStringLiteral("DELETE FROM reminder_deliveries WHERE reminder_id=?")); clear.addBindValue(reminder->id);
            if (!clear.exec()) return sqlFailure(clear, error);
        }
    }
    QSqlQuery query(m_database);
    if (!query.prepare(update ? QStringLiteral("UPDATE reminders SET trigger_at_utc_ms=?,sort_order=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=?")
                              : QStringLiteral("INSERT INTO reminders(id,entity_id,trigger_at_utc_ms,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    if (update) {
        query.addBindValue(utcMs(reminder->triggerAt)); query.addBindValue(reminder->sortOrder); query.addBindValue(utcMs(reminder->updatedAt));
        query.addBindValue(reminder->id);
    } else {
        query.addBindValue(reminder->id); query.addBindValue(reminder->entityId); query.addBindValue(utcMs(reminder->triggerAt));
        query.addBindValue(reminder->sortOrder); query.addBindValue(utcMs(reminder->createdAt)); query.addBindValue(utcMs(reminder->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveDelivery(const ReminderDelivery &delivery, QString *error)
{
    if (!isUuid(delivery.reminderId) || !isUuid(delivery.installationId) || !delivery.scheduledAt.isValid() || !delivery.deliveredAt.isValid())
        return entitycodec::fail(error, QStringLiteral("提醒投递字段非法"));
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO reminder_deliveries(reminder_id,installation_id,scheduled_at_utc_ms,delivered_at_utc_ms) "
        "SELECT ?,?,?,? WHERE EXISTS(SELECT 1 FROM reminders r JOIN entities t ON t.id=r.entity_id "
        "WHERE r.id=? AND r.trigger_at_utc_ms=? AND t.kind='task' AND t.status='active' AND t.deleted_at_utc_ms IS NULL) "
        "AND NOT EXISTS(SELECT 1 FROM reminder_deliveries WHERE reminder_id=? AND installation_id=?)"));
    query.addBindValue(delivery.reminderId); query.addBindValue(delivery.installationId); query.addBindValue(utcMs(delivery.scheduledAt)); query.addBindValue(utcMs(delivery.deliveredAt));
    query.addBindValue(delivery.reminderId); query.addBindValue(utcMs(delivery.scheduledAt)); query.addBindValue(delivery.reminderId); query.addBindValue(delivery.installationId);
    if (!query.exec()) return sqlFailure(query, error);
    if (query.numRowsAffected() != 1) return entitycodec::fail(error, QStringLiteral("提醒已投递或计划时间已变化"));
    return true;
}

bool TaskRepository::saveAttachment(const Attachment &attachment, QString *error)
{
    if (!attachmentcodec::validateAttachment(attachment, error)) return false;
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT entity_id FROM attachments WHERE id=?")); exists.addBindValue(attachment.id);
    if (!exists.exec()) return sqlFailure(exists, error);
    QSqlQuery query(m_database);
    if (exists.next()) {
        if (exists.value(0).toString() != attachment.entityId)
            return entitycodec::fail(error, QStringLiteral("附件不能移动到其他实体"));
        query.prepare(QStringLiteral("UPDATE attachments SET local_state=?,verified_at_utc_ms=? WHERE id=?"));
        query.addBindValue(attachmentFileStateName(attachment.localState)); query.addBindValue(msValue(attachment.verifiedAt));
        query.addBindValue(attachment.id);
    } else {
        query.prepare(QStringLiteral("INSERT INTO attachments(id,entity_id,original_name,relative_path,mime,size,sha256,is_image,local_state,created_at_utc_ms,verified_at_utc_ms,sort_order) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(attachment.id); query.addBindValue(attachment.entityId); query.addBindValue(attachment.originalName);
        query.addBindValue(attachment.relativePath); query.addBindValue(attachment.mime); query.addBindValue(attachment.size);
        query.addBindValue(attachment.sha256); query.addBindValue(attachment.isImage ? 1 : 0); query.addBindValue(attachmentFileStateName(attachment.localState));
        query.addBindValue(utcMs(attachment.createdAt)); query.addBindValue(msValue(attachment.verifiedAt)); query.addBindValue(attachment.sortOrder);
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::saveDesktopNote(DesktopNote *note, QString *error)
{
    if (!isUuid(note->id) || !isUuid(note->entityId) || !isUuid(note->deviceId) || !note->createdAt.isValid() || !note->updatedAt.isValid()
        || colorToInt(note->color) < 0 || colorToInt(note->color) >= ColorCount || layerToInt(note->layer) < 0 || layerToInt(note->layer) >= LayerCount)
        return entitycodec::fail(error, QStringLiteral("桌面便签字段非法"));
    QSqlQuery exists(m_database); exists.prepare(QStringLiteral("SELECT entity_id,device_id FROM desktop_notes WHERE id=?")); exists.addBindValue(note->id);
    if (!exists.exec()) return sqlFailure(exists, error);
    QSqlQuery query(m_database);
    if (exists.next()) {
        if (exists.value(0).toString() != note->entityId || exists.value(1).toString() != note->deviceId)
            return entitycodec::fail(error, QStringLiteral("桌面便签所属任务/设备不可变更"));
        query.prepare(QStringLiteral("UPDATE desktop_notes SET color=?,geometry=?,z_order=?,layer=?,open=?,updated_at_utc_ms=? WHERE id=?"));
        query.addBindValue(colorToInt(note->color)); query.addBindValue(encodeGeometry(note->geometry)); query.addBindValue(note->zOrder);
        query.addBindValue(layerToInt(note->layer)); query.addBindValue(note->open ? 1 : 0); query.addBindValue(utcMs(note->updatedAt)); query.addBindValue(note->id);
    } else {
        query.prepare(QStringLiteral("INSERT INTO desktop_notes(id,entity_id,device_id,color,geometry,z_order,layer,open,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(note->id); query.addBindValue(note->entityId); query.addBindValue(note->deviceId); query.addBindValue(colorToInt(note->color));
        query.addBindValue(encodeGeometry(note->geometry)); query.addBindValue(note->zOrder); query.addBindValue(layerToInt(note->layer));
        query.addBindValue(note->open ? 1 : 0); query.addBindValue(utcMs(note->createdAt)); query.addBindValue(utcMs(note->updatedAt));
    }
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::insertLink(const RecurrenceLink &link, QString *error)
{
    if (!isUuid(link.id) || !isUuid(link.sourceId) || (!link.nextId.isEmpty() && !isUuid(link.nextId))
        || link.sourceId == link.nextId || link.nextRevision <= 0) return entitycodec::fail(error, QStringLiteral("重复生成凭证非法"));
    QSqlQuery query(m_database); query.prepare(QStringLiteral("INSERT INTO recurrence_links(id,source_id,next_id,next_revision) VALUES(?,?,?,?)"));
    query.addBindValue(link.id); query.addBindValue(link.sourceId); query.addBindValue(nullableText(link.nextId)); query.addBindValue(link.nextRevision);
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::tombstone(const QString &type, const QString &id, QString *error)
{
    const int enabled = syncEnabled(m_database, error);
    if (enabled < 0) return false;
    if (enabled == 0) return true;
    QSqlQuery query(m_database); query.prepare(QStringLiteral("INSERT INTO tombstones(id,entity_type,entity_id,deleted_at_utc_ms) VALUES(?,?,?,?)"));
    query.addBindValue(newUuid()); query.addBindValue(type); query.addBindValue(id); query.addBindValue(utcMs(utcNow()));
    return query.exec() ? true : sqlFailure(query, error);
}

bool TaskRepository::queueOutbox(const EntityChanges &changes, QString *error)
{
    const int enabled = syncEnabled(m_database, error);
    if (enabled < 0) return false;
    if (enabled == 0) return true;
    QVector<OutboxItem> rows;
    const QString batchId = newUuid();
    QSqlQuery baseQuery(m_database);
    if (!baseQuery.exec(QStringLiteral("SELECT value FROM local_meta WHERE key='sync_base_server_revision'")) || !baseQuery.next())
        { sqlFailure(baseQuery, error); return false; }
    bool baseOk = false; const qint64 baseRevision = baseQuery.value(0).toString().toLongLong(&baseOk);
    if (!baseOk || baseRevision < 0) return entitycodec::fail(error, QStringLiteral("同步基线 revision 非法"));
    const auto add = [&rows, &batchId, baseRevision](const QString &type, const QString &id, const QString &operation) {
        if (!id.isEmpty()) { OutboxItem row; row.id = newUuid(); row.batchId = batchId;
            row.baseServerRevision = baseRevision; row.entityType = type; row.entityId = id; row.operation = operation; row.createdAt = utcNow(); rows.append(row); }
    };
    for (const Folder &row : changes.folderSaves) add(QStringLiteral("folder"), row.id, QStringLiteral("upsert"));
    for (const QString &id : changes.folderRemovals) add(QStringLiteral("folder"), id, QStringLiteral("delete"));
    for (const TaskList &row : changes.groupSaves) add(QStringLiteral("list"), row.id, QStringLiteral("upsert"));
    for (const QString &id : changes.groupRemovals) add(QStringLiteral("list"), id, QStringLiteral("delete"));
    for (const Section &row : changes.sectionSaves) add(QStringLiteral("section"), row.id, QStringLiteral("upsert"));
    for (const QString &id : changes.sectionRemovals) add(QStringLiteral("section"), id, QStringLiteral("delete"));
    for (const RecurrenceSeries &row : changes.seriesSaves) add(QStringLiteral("recurrence_series"), row.id, QStringLiteral("upsert"));
    for (const Tag &row : changes.tagSaves) add(QStringLiteral("tag"), row.id, QStringLiteral("upsert"));
    for (const TaskTag &row : changes.taskTagSaves) add(QStringLiteral("task_tag"), row.id, QStringLiteral("upsert"));
    for (const TaskTag &row : changes.taskTagRemovals) add(QStringLiteral("task_tag"), row.id, QStringLiteral("delete"));
    for (const RecurrenceLink &row : changes.recurrenceLinkSaves) add(QStringLiteral("recurrence_link"), row.id, QStringLiteral("upsert"));
    for (const StoredEntity &row : changes.saves) add(QStringLiteral("task"), row.entity.id, QStringLiteral("upsert"));
    for (const StoredEntity &row : changes.touches) add(QStringLiteral("task"), row.entity.id, QStringLiteral("upsert"));
    for (const StoredEntity &row : changes.removals) add(QStringLiteral("task"), row.entity.id, QStringLiteral("delete"));
    for (const ChecklistItem &row : changes.checklistSaves) add(QStringLiteral("checklist_item"), row.id, QStringLiteral("upsert"));
    for (const QString &id : changes.checklistRemovals) add(QStringLiteral("checklist_item"), id, QStringLiteral("delete"));
    for (const TaskReminder &row : changes.reminderSaves) add(QStringLiteral("reminder"), row.id, QStringLiteral("upsert"));
    for (const QString &id : changes.reminderRemovals) add(QStringLiteral("reminder"), id, QStringLiteral("delete"));
    for (const Attachment &row : changes.attachmentSaves) add(QStringLiteral("attachment"), row.id, QStringLiteral("upsert"));
    for (const Attachment &row : changes.attachmentRemovals) add(QStringLiteral("attachment"), row.id, QStringLiteral("delete"));
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral("INSERT INTO outbox(id,batch_id,base_server_revision,entity_type,entity_id,operation,payload_json,created_at_utc_ms) VALUES(?,?,?,?,?,?,'{}',?)")))
        return sqlFailure(query, error);
    for (const OutboxItem &row : rows) {
        query.bindValue(0, row.id); query.bindValue(1, row.batchId); query.bindValue(2, row.baseServerRevision);
        query.bindValue(3, row.entityType); query.bindValue(4, row.entityId); query.bindValue(5, row.operation); query.bindValue(6, utcMs(row.createdAt));
        if (!query.exec()) return sqlFailure(query, error);
    }
    return true;
}

bool TaskRepository::appendEventsInTransaction(QVector<EntityEvent> *events, QString *error)
{
    if (!events) return entitycodec::fail(error, QStringLiteral("事件数组不能为空"));
    if (events->isEmpty()) return true;
    QString batchId;
    for (const EntityEvent &event : *events) {
        if (event.batchId.isEmpty()) continue;
        if (batchId.isEmpty()) batchId = event.batchId;
        else if (batchId != event.batchId) return entitycodec::fail(error, QStringLiteral("同一 apply 的事件 batch_id 必须一致"));
    }
    if (batchId.isEmpty()) batchId = newUuid();
    QSqlQuery query(m_database);
    if (!query.prepare(QStringLiteral("INSERT INTO entity_events(id,entity_id,entity_kind,entity_title,batch_id,type,field,old_value,new_value,occurred_at_utc_ms) VALUES(?,?,?,?,?,?,?,?,?,?)")))
        return sqlFailure(query, error);
    for (EntityEvent &event : *events) {
        if (event.id.isEmpty()) event.id = newUuid();
        if (event.batchId.isEmpty()) event.batchId = batchId;
        if (!validEvent(event, error)) return false;
        query.bindValue(0, event.id); query.bindValue(1, event.entityId); query.bindValue(2, entityKindName(event.entityKind));
        query.bindValue(3, event.entityTitle); query.bindValue(4, event.batchId); query.bindValue(5, event.type);
        query.bindValue(6, safeText(event.field)); query.bindValue(7, eventValueForDb(event.oldValue));
        query.bindValue(8, eventValueForDb(event.newValue)); query.bindValue(9, utcMs(event.occurredAt));
        if (!query.exec()) return sqlFailure(query, error);
    }
    return true;
}

bool TaskRepository::appendEvents(const QVector<EntityEvent> &events, QString *error)
{
    if (!ready(error)) return false;
    if (events.isEmpty()) { if (error) error->clear(); return true; }
    QVector<EntityEvent> pending = events;
    if (!m_database.transaction()) return entitycodec::fail(error, m_database.lastError().text());
    if (!appendEventsInTransaction(&pending, error)) { m_database.rollback(); return false; }
    if (!m_database.commit()) {
        const QString failure = m_database.lastError().text(); m_database.rollback();
        return entitycodec::fail(error, failure);
    }
    if (error) error->clear();
    return true;
}

QVector<EntityEvent> TaskRepository::eventsFor(const QString &entityId, QString *error) const
{
    QVector<EntityEvent> events;
    if (!isUuid(entityId)) { entitycodec::fail(error, QStringLiteral("实体 id 必须为 UUID")); return events; }
    if (!ready(error)) return events;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id,entity_id,entity_kind,entity_title,batch_id,type,field,old_value,new_value,occurred_at_utc_ms "
        "FROM entity_events WHERE entity_id=? ORDER BY occurred_at_utc_ms ASC,id ASC"));
    query.addBindValue(entityId);
    if (!query.exec()) { sqlFailure(query, error); return QVector<EntityEvent>(); }
    while (query.next()) {
        EntityEvent event;
        event.id = query.value(0).toString(); event.entityId = query.value(1).toString();
        event.entityKind = entityKindFromName(query.value(2).toString()); event.entityTitle = query.value(3).toString();
        event.batchId = query.value(4).toString(); event.type = query.value(5).toString(); event.field = query.value(6).toString();
        if (entityKindName(event.entityKind) != query.value(2).toString()
            || !readEventValue(query.value(7), &event.oldValue, error)
            || !readEventValue(query.value(8), &event.newValue, error)
            || !dbDate(query.value(9), &event.occurredAt, error) || !validEvent(event, error))
            return QVector<EntityEvent>();
        events.append(event);
    }
    if (error) error->clear();
    return events;
}

QVector<EntityEvent> TaskRepository::allEvents(QString *error) const
{
    QVector<EntityEvent> events;
    if (!ready(error)) return events;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT id,entity_id,entity_kind,entity_title,batch_id,type,field,old_value,new_value,occurred_at_utc_ms "
        "FROM entity_events ORDER BY occurred_at_utc_ms ASC,id ASC"))) { sqlFailure(query, error); return events; }
    while (query.next()) {
        EntityEvent event;
        event.id = query.value(0).toString(); event.entityId = query.value(1).toString();
        event.entityKind = entityKindFromName(query.value(2).toString()); event.entityTitle = query.value(3).toString();
        event.batchId = query.value(4).toString(); event.type = query.value(5).toString(); event.field = query.value(6).toString();
        if (entityKindName(event.entityKind) != query.value(2).toString()
            || !readEventValue(query.value(7), &event.oldValue, error)
            || !readEventValue(query.value(8), &event.newValue, error)
            || !dbDate(query.value(9), &event.occurredAt, error) || !validEvent(event, error))
            return QVector<EntityEvent>();
        events.append(event);
    }
    if (error) error->clear();
    return events;
}

bool TaskRepository::apply(EntityChanges *changes, EntitySnapshot *after, QString *error)
{
    if (!changes || !after || !ready(error)) return false;
    EntityChanges pending = *changes;
    if (!m_database.transaction()) return entitycodec::fail(error, m_database.lastError().text());
    const auto abort = [this]() { m_database.rollback(); return false; };
    if (!appendEventsInTransaction(&pending.eventSaves, error)) return abort();
    for (const QString &sourceId : pending.removeLinks) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM recurrence_links WHERE source_id=?"));
        query.addBindValue(sourceId);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1) { entitycodec::fail(error, QStringLiteral("重复生成凭证已变化")); return abort(); }
    }
    for (const StoredEntity &stored : pending.touches) if (!touchTask(stored, error)) return abort();
    for (const StoredEntity &stored : pending.removals) if (!removeTask(stored, error)) return abort();
    const auto removeRows = [this](const QStringList &ids, const QString &table, const QString &type, QString *error) -> bool {
        for (const QString &id : ids) {
            QSqlQuery ownerQuery(m_database);
            ownerQuery.prepare(QStringLiteral("SELECT 1 FROM %1 WHERE id=?").arg(table)); ownerQuery.addBindValue(id);
            if (!ownerQuery.exec()) return sqlFailure(ownerQuery, error);
            if (!ownerQuery.next()) return entitycodec::fail(error, QStringLiteral("要删除的实体不存在"));
            if (type == QStringLiteral("list")) {
                QSqlQuery move(m_database);
                move.prepare(QStringLiteral("UPDATE entities SET list_id=(SELECT id FROM lists WHERE is_inbox=1),"
                    "section_id=NULL,updated_at_utc_ms=?,revision=revision+1 WHERE list_id=?"));
                move.addBindValue(utcMs(utcNow())); move.addBindValue(id);
                if (!move.exec()) return sqlFailure(move, error);
            } else if (type == QStringLiteral("section")) {
                QSqlQuery move(m_database);
                move.prepare(QStringLiteral("UPDATE entities SET section_id=NULL,updated_at_utc_ms=?,revision=revision+1 WHERE section_id=?"));
                move.addBindValue(utcMs(utcNow())); move.addBindValue(id);
                if (!move.exec()) return sqlFailure(move, error);
            } else if (type == QStringLiteral("folder")) {
                QSqlQuery lists(m_database);
                lists.prepare(QStringLiteral("UPDATE lists SET folder_id=NULL,updated_at_utc_ms=?,revision=revision+1 WHERE folder_id=?"));
                lists.addBindValue(utcMs(utcNow())); lists.addBindValue(id);
                if (!lists.exec()) return sqlFailure(lists, error);
                QSqlQuery entities(m_database);
                entities.prepare(QStringLiteral("UPDATE entities SET folder_id=NULL,updated_at_utc_ms=?,revision=revision+1 WHERE folder_id=?"));
                entities.addBindValue(utcMs(utcNow())); entities.addBindValue(id);
                if (!entities.exec()) return sqlFailure(entities, error);
            }
            QSqlQuery query(m_database); query.prepare(QStringLiteral("DELETE FROM %1 WHERE id=?").arg(table));
            query.addBindValue(id);
            if (!query.exec()) return sqlFailure(query, error);
            if (query.numRowsAffected() != 1) return entitycodec::fail(error, QStringLiteral("要删除的实体不存在"));
            if (!tombstone(type, id, error)) return false;
        }
        return true;
    };
    if (!removeRows(pending.folderRemovals, QStringLiteral("folders"), QStringLiteral("folder"), error)
        || !removeRows(pending.groupRemovals, QStringLiteral("lists"), QStringLiteral("list"), error)
        || !removeRows(pending.sectionRemovals, QStringLiteral("sections"), QStringLiteral("section"), error)) return abort();
    for (const QString &id : pending.checklistRemovals) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM checklist_items WHERE id=?"));
        query.addBindValue(id);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1 || !tombstone(QStringLiteral("checklist_item"), id, error)) return abort();
    }
    for (const QString &id : pending.reminderRemovals) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("DELETE FROM reminders WHERE id=?"));
        query.addBindValue(id);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1 || !tombstone(QStringLiteral("reminder"), id, error)) return abort();
    }
    for (Folder &row : pending.folderSaves) if (!saveFolder(&row, error)) return abort();
    for (TaskList &row : pending.groupSaves) if (!saveGroup(&row, error)) return abort();
    for (Section &row : pending.sectionSaves) if (!saveSection(&row, error)) return abort();
    for (RecurrenceSeries &row : pending.seriesSaves) if (!saveSeries(&row, error)) return abort();
    for (StoredEntity &row : pending.saves) if (!saveTask(&row, error)) return abort();
    for (ChecklistItem &row : pending.checklistSaves) if (!saveChecklist(&row, error)) return abort();
    for (Tag &row : pending.tagSaves) if (!saveTag(&row, error)) return abort();
    for (const TaskTag &row : pending.taskTagRemovals) {
        QSqlQuery query(m_database); query.prepare(QStringLiteral("DELETE FROM entity_tags WHERE id=?"));
        query.addBindValue(row.id); if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() > 0 && !tombstone(QStringLiteral("task_tag"), row.id, error)) return abort();
    }
    for (const TaskTag &row : pending.taskTagSaves) {
        if (!isUuid(row.id) || !isUuid(row.entityId) || !isUuid(row.tagId)) { entitycodec::fail(error, QStringLiteral("任务标签关系非法")); return abort(); }
        QSqlQuery query(m_database); query.prepare(QStringLiteral("INSERT INTO entity_tags(id,entity_id,tag_id) VALUES(?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); query.addBindValue(row.tagId); if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (TaskReminder &row : pending.reminderSaves) if (!saveReminder(&row, error)) return abort();
    for (const ReminderDelivery &row : pending.deliverySaves) if (!saveDelivery(row, error)) return abort();
    for (const Attachment &row : pending.attachmentRemovals) {
        QSqlQuery query(m_database); query.prepare(QStringLiteral("DELETE FROM attachments WHERE id=? AND entity_id=?"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1 || !tombstone(QStringLiteral("attachment"), row.id, error)) return abort();
    }
    for (const Attachment &row : pending.attachmentSaves) if (!saveAttachment(row, error)) return abort();
    for (const QString &id : pending.desktopNoteRemovals) {
        QSqlQuery query(m_database); query.prepare(QStringLiteral("DELETE FROM desktop_notes WHERE id=?")); query.addBindValue(id);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
        if (query.numRowsAffected() != 1) { entitycodec::fail(error, QStringLiteral("桌面便签不存在")); return abort(); }
    }
    for (DesktopNote &row : pending.desktopNoteSaves) if (!saveDesktopNote(&row, error)) return abort();
    for (const RecurrenceLink &row : pending.recurrenceLinkSaves) if (!insertLink(row, error)) return abort();
    if (!queueOutbox(pending, error)) return abort();
    EntitySnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) return abort();
    if (!m_database.commit()) { entitycodec::fail(error, m_database.lastError().text()); return abort(); }
    *changes = pending; *after = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::convertKind(const QString &id, EntityKind to, EntitySnapshot *after, QString *error)
{
    if (!after) return entitycodec::fail(error, QStringLiteral("输出快照不能为空"));
    if (!isUuid(id)) return entitycodec::fail(error, QStringLiteral("实体 id 必须为 UUID"));
    if (entityKindName(to).isEmpty()) return entitycodec::fail(error, QStringLiteral("目标 kind 非法"));
    if (!ready(error)) return false;
    if (!m_database.transaction()) return entitycodec::fail(error, m_database.lastError().text());
    const auto abort = [this]() { m_database.rollback(); return false; };
    QSqlQuery current(m_database);
    current.prepare(QStringLiteral("SELECT kind,title,parent_id,revision FROM entities WHERE id=?")); current.addBindValue(id);
    if (!current.exec()) { sqlFailure(current, error); return abort(); }
    if (!current.next()) { entitycodec::fail(error, QStringLiteral("实体不存在")); return abort(); }
    const QString oldKind = current.value(0).toString(), newKind = entityKindName(to);
    const QString title = current.value(1).toString(), parentId = current.value(2).toString();
    const qint64 revision = current.value(3).toLongLong();
    const EntityKind from = entityKindFromName(oldKind);
    current.finish();
    if (entityKindName(from) != oldKind || revision <= 0) { entitycodec::fail(error, QStringLiteral("实体 kind 或 revision 损坏")); return abort(); }
    if (from == to) { entitycodec::fail(error, QStringLiteral("实体已经是目标 kind")); return abort(); }
    if (to == EntityKind::Note && from != EntityKind::Task) { entitycodec::fail(error, QStringLiteral("只有任务可以转为笔记")); return abort(); }
    if (to == EntityKind::Task && from != EntityKind::Note) { entitycodec::fail(error, QStringLiteral("只有笔记可以转为任务")); return abort(); }
    if (to == EntityKind::Note) {
        if (!parentId.isEmpty()) { entitycodec::fail(error, QStringLiteral("自身为子任务的实体不能转为笔记")); return abort(); }
        QSqlQuery child(m_database); child.prepare(QStringLiteral("SELECT 1 FROM entities WHERE parent_id=? LIMIT 1")); child.addBindValue(id);
        if (!child.exec()) { sqlFailure(child, error); return abort(); }
        if (child.next()) { entitycodec::fail(error, QStringLiteral("含有子实体的实体不能转为笔记")); return abort(); }
    }
    const QDateTime now = utcNow();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral("UPDATE entities SET kind=?,updated_at_utc_ms=?,revision=revision+1 WHERE id=? AND revision=?"));
    update.addBindValue(newKind); update.addBindValue(utcMs(now)); update.addBindValue(id); update.addBindValue(revision);
    if (!update.exec()) { sqlFailure(update, error); return abort(); }
    if (update.numRowsAffected() != 1) { entitycodec::fail(error, QStringLiteral("实体已被其他操作修改，请重新加载")); return abort(); }
    QVector<EntityEvent> events;
    EntityEvent event; event.entityId = id; event.entityKind = to; event.entityTitle = title; event.type = QStringLiteral("kind_changed");
    event.field = QStringLiteral("kind"); event.oldValue = oldKind; event.newValue = newKind; event.occurredAt = now;
    events.append(event);
    if (!appendEventsInTransaction(&events, error)) return abort();
    EntitySnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) return abort();
    if (!m_database.commit()) { entitycodec::fail(error, m_database.lastError().text()); return abort(); }
    *after = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::replaceAll(const EntitySnapshot &incoming, const QVector<EntityEvent> &events,
                                      const QString &defaultTimezone, const QDateTime &importedAt,
                                      EntitySnapshot *after, QString *error,
                                      const BeforeCommit &beforeCommit)
{
    if (!after || !ready(error)) return false;
    if (!entitycodec::validTimeZone(defaultTimezone) || !importedAt.isValid())
        return entitycodec::fail(error, QStringLiteral("备份默认时区或导入时刻非法"));
    QString inboxId;
    for (const TaskList &list : incoming.lists) if (list.inbox) {
        if (!inboxId.isEmpty()) return entitycodec::fail(error, QStringLiteral("备份系统收集箱不唯一"));
        inboxId = list.id;
    }
    if (!isUuid(inboxId)) return entitycodec::fail(error, QStringLiteral("备份缺少系统收集箱"));
    if (!m_database.transaction()) return entitycodec::fail(error, m_database.lastError().text());
    const auto abort = [this]() { m_database.rollback(); return false; };
    const auto exec = [this, error](const QString &sql) {
        QSqlQuery query(m_database); if (!query.exec(sql)) return sqlFailure(query, error); return true;
    };
    // 覆盖恢复不是业务合并：禁止生成新的同步历史，设备身份则原样保留。
    if (!exec(QStringLiteral("UPDATE local_meta SET value='0' WHERE key='signed_in'"))
        || !exec(QStringLiteral("DROP TRIGGER prevent_inbox_delete"))
        || !exec(QStringLiteral("DROP TRIGGER prevent_inbox_unset"))) return abort();
    const char *const clearOrder[] = {
        "entity_events", "reminder_deliveries", "desktop_notes", "attachments", "checklist_items",
        "reminders", "entity_tags", "tags", "recurrence_links", "entities", "recurrence_series",
        "sections", "lists", "folders", "view_defs", "sync_bindings", "tombstones", "outbox"
    };
    for (const char *table : clearOrder)
        if (!exec(QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)))) return abort();

    for (const Folder &row : incoming.folders) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO folders(id,name,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.name); query.addBindValue(row.sortOrder);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const TaskList &row : incoming.lists) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(nullableText(row.folderId)); query.addBindValue(entityKindName(row.kind));
        query.addBindValue(row.name); query.addBindValue(row.inbox ? 1 : 0); query.addBindValue(row.sortOrder);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const Section &row : incoming.sections) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO sections(id,list_id,name,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.listId); query.addBindValue(row.name); query.addBindValue(row.sortOrder);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const RecurrenceSeries &row : incoming.recurrenceSeries) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO recurrence_series(id,rrule,anchor_day,timezone,dtstart_utc_ms,dtstart_date,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.rrule); query.addBindValue(row.anchorDay); query.addBindValue(row.timeZone);
        query.addBindValue(msValue(row.dtstartAt)); query.addBindValue(nullableText(row.dtstartDate.isValid() ? row.dtstartDate.toString(Qt::ISODate) : QString()));
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }

    // 仅排序以满足父子外键，不重写任何 UUID 或引用。
    QVector<StoredEntity> ordered;
    QSet<QString> inserted;
    while (ordered.size() < incoming.entities.size()) {
        bool progressed = false;
        for (const StoredEntity &row : incoming.entities) {
            if (inserted.contains(row.entity.id)) continue;
            if (!row.entity.parentId.isEmpty() && !inserted.contains(row.entity.parentId)) continue;
            ordered.append(row); inserted.insert(row.entity.id); progressed = true;
        }
        if (!progressed) { entitycodec::fail(error, QStringLiteral("备份父子引用形成循环或缺失")); return abort(); }
    }
    for (const StoredEntity &stored : ordered) {
        const Entity &row = stored.entity;
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,folder_id,list_id,section_id,parent_id,sort_order,status,"
            "deleted_at_utc_ms,start_at_utc_ms,due_at_utc_ms,all_day_date,timezone,recurrence_series_id,occurrence_key,priority,"
            "pinned_at_utc_ms,revision,server_revision,created_at_utc_ms,updated_at_utc_ms,completed_at_utc_ms,abandoned_at_utc_ms) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(entityKindName(row.kind)); query.addBindValue(safeText(row.title)); query.addBindValue(safeText(row.body));
        query.addBindValue(nullableText(row.folderId)); query.addBindValue(row.listId); query.addBindValue(nullableText(row.sectionId));
        query.addBindValue(nullableText(row.parentId)); query.addBindValue(row.sortOrder); query.addBindValue(taskStatusName(row.status));
        query.addBindValue(msValue(row.deletedAt)); query.addBindValue(msValue(row.startAt)); query.addBindValue(msValue(row.dueAt));
        query.addBindValue(nullableText(row.allDayDate.isValid() ? row.allDayDate.toString(Qt::ISODate) : QString()));
        query.addBindValue(row.timeZone); query.addBindValue(nullableText(row.recurrenceSeriesId)); query.addBindValue(nullableText(row.occurrenceKey));
        query.addBindValue(row.priority); query.addBindValue(msValue(row.pinnedAt)); query.addBindValue(stored.revision);
        query.addBindValue(stored.serverRevision);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt));
        query.addBindValue(msValue(row.completedAt)); query.addBindValue(msValue(row.abandonedAt));
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const ChecklistItem &row : incoming.checklistItems) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO checklist_items(id,entity_id,text,done,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); query.addBindValue(safeText(row.text)); query.addBindValue(row.done ? 1 : 0);
        query.addBindValue(row.sortOrder); query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision);
        query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const Tag &row : incoming.tags) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO tags(id,name,normalized_name,color,revision,server_revision) VALUES(?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.name); query.addBindValue(row.normalizedName); query.addBindValue(safeText(row.color));
        query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const TaskTag &row : incoming.taskTags) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO entity_tags(id,entity_id,tag_id,revision,server_revision) VALUES(?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); query.addBindValue(row.tagId); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const TaskReminder &row : incoming.reminders) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO reminders(id,entity_id,trigger_at_utc_ms,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); query.addBindValue(utcMs(row.triggerAt)); query.addBindValue(row.sortOrder);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    // 导入时刻之前的全部提醒都只写本机抑制凭证；是否调度仍由正常 task/active 门槛决定。
    {
        QSqlQuery deliveries(m_database);
        deliveries.prepare(QStringLiteral("INSERT INTO reminder_deliveries(reminder_id,installation_id,scheduled_at_utc_ms,delivered_at_utc_ms) "
            "SELECT id,(SELECT value FROM local_meta WHERE key='installation_id'),trigger_at_utc_ms,? FROM reminders WHERE trigger_at_utc_ms<=?"));
        deliveries.addBindValue(utcMs(importedAt)); deliveries.addBindValue(utcMs(importedAt));
        if (!deliveries.exec()) { sqlFailure(deliveries, error); return abort(); }
    }
    for (const RecurrenceLink &row : incoming.recurrenceLinks) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO recurrence_links(id,source_id,next_id,next_revision,revision,server_revision) VALUES(?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.sourceId); query.addBindValue(nullableText(row.nextId));
        query.addBindValue(row.nextRevision); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const Attachment &row : incoming.attachments) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO attachments(id,entity_id,original_name,relative_path,mime,size,sha256,is_image,local_state,created_at_utc_ms,verified_at_utc_ms,sort_order,revision,server_revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.entityId); query.addBindValue(row.originalName); query.addBindValue(row.relativePath);
        query.addBindValue(row.mime); query.addBindValue(row.size); query.addBindValue(row.sha256); query.addBindValue(row.isImage ? 1 : 0);
        query.addBindValue(attachmentFileStateName(row.localState)); query.addBindValue(utcMs(row.createdAt)); query.addBindValue(msValue(row.verifiedAt));
        query.addBindValue(row.sortOrder); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const ViewDef &row : incoming.viewDefs) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO view_defs(id,name,spec_json,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.id); query.addBindValue(row.name); query.addBindValue(row.specJson); query.addBindValue(row.sortOrder);
        query.addBindValue(utcMs(row.createdAt)); query.addBindValue(utcMs(row.updatedAt)); query.addBindValue(row.revision); query.addBindValue(row.serverRevision);
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    for (const EntityEvent &event : events) {
        if (!validEvent(event, error)) return abort();
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT INTO entity_events(id,entity_id,entity_kind,entity_title,batch_id,type,field,old_value,new_value,occurred_at_utc_ms) VALUES(?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(event.id); query.addBindValue(event.entityId); query.addBindValue(entityKindName(event.entityKind));
        query.addBindValue(safeText(event.entityTitle)); query.addBindValue(event.batchId); query.addBindValue(event.type);
        query.addBindValue(safeText(event.field)); query.addBindValue(eventValueForDb(event.oldValue));
        query.addBindValue(eventValueForDb(event.newValue)); query.addBindValue(utcMs(event.occurredAt));
        if (!query.exec()) { sqlFailure(query, error); return abort(); }
    }
    QSqlQuery meta(m_database);
    meta.prepare(QStringLiteral("INSERT OR REPLACE INTO local_meta(key,value) VALUES(?,?)"));
    const QString keys[] = {QStringLiteral("inbox_list_id"), QStringLiteral("default_timezone"),
                            QStringLiteral("signed_in"), QStringLiteral("sync_base_server_revision")};
    const QString values[] = {inboxId, defaultTimezone, QStringLiteral("0"), QStringLiteral("0")};
    for (int i = 0; i < 4; ++i) {
        meta.bindValue(0, keys[i]); meta.bindValue(1, values[i]);
        if (!meta.exec()) { sqlFailure(meta, error); return abort(); }
    }
    if (!exec(QStringLiteral("CREATE TRIGGER prevent_inbox_delete BEFORE DELETE ON lists WHEN OLD.is_inbox=1 BEGIN SELECT RAISE(ABORT,'inbox is permanent'); END"))
        || !exec(QStringLiteral("CREATE TRIGGER prevent_inbox_unset BEFORE UPDATE OF is_inbox ON lists WHEN OLD.is_inbox=1 AND NEW.is_inbox<>1 BEGIN SELECT RAISE(ABORT,'inbox is permanent'); END"))) return abort();
    QSqlQuery foreign(m_database);
    if (!foreign.exec(QStringLiteral("PRAGMA foreign_key_check"))) { sqlFailure(foreign, error); return abort(); }
    if (foreign.next()) { entitycodec::fail(error, QStringLiteral("覆盖恢复外键检查失败")); return abort(); }
    EntitySnapshot snapshot;
    if (!readSnapshot(&snapshot, error)) return abort();
    if (beforeCommit && !beforeCommit(error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("附件文件交换失败");
        return abort();
    }
    if (!m_database.commit()) {
        entitycodec::fail(error, m_database.lastError().text());
        return abort();
    }
    *after = snapshot;
    if (error) error->clear();
    return true;
}

bool TaskRepository::setLocalMeta(const QString &key, const QString &value, QString *error)
{
    if (!ready(error) || key.trimmed().isEmpty()) return entitycodec::fail(error, QStringLiteral("local_meta 键不能为空"));
    QSqlQuery query(m_database); query.prepare(QStringLiteral("INSERT OR REPLACE INTO local_meta(key,value) VALUES(?,?)"));
    query.addBindValue(key.trimmed()); query.addBindValue(value);
    return query.exec() ? true : sqlFailure(query, error);
}

} // namespace kl
