#ifndef KYLINLIST_BACKUPCODEC_H
#define KYLINLIST_BACKUPCODEC_H

#include "../db/TaskRepository.h"
#include "AttachmentCodec.h"
#include "EntityCodec.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace kl {
namespace backupcodec {

const int kVersion = 2;
const qint64 kMaximumRevision = 9007199254740991LL;
inline QString formatName() { return QStringLiteral("KylinList.Entities"); }
inline int entityPosition(const EntitySnapshot &snapshot, const QString &id)
{
    for (int i = 0; i < snapshot.entities.size(); ++i) if (snapshot.entities.at(i).entity.id == id) return i;
    return -1;
}
inline bool readRevision(const QJsonObject &object, qint64 *revision, QString *error)
{
    if (!entitycodec::readInteger(object.value(QStringLiteral("revision")), 1, kMaximumRevision, revision))
        return entitycodec::fail(error, QStringLiteral("备份 revision 必须为正整数"));
    return true;
}
inline bool readServerRevision(const QJsonObject &object, qint64 *serverRevision, QString *error)
{
    if (!entitycodec::readInteger(object.value(QStringLiteral("serverRevision")), 0, kMaximumRevision, serverRevision))
        return entitycodec::fail(error, QStringLiteral("备份 serverRevision 必须为非负整数"));
    return true;
}
inline QJsonObject namedJson(const QString &id, const QString &name, const QString &parent, int order,
                             const QDateTime &created, const QDateTime &updated, const QString &parentKey,
                             qint64 revision, qint64 serverRevision)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), id); object.insert(QStringLiteral("name"), name);
    if (!parentKey.isEmpty()) object.insert(parentKey, parent.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(parent));
    object.insert(QStringLiteral("sortOrder"), order);
    object.insert(QStringLiteral("createdAtUtcMs"), entitycodec::dateJson(created));
    object.insert(QStringLiteral("updatedAtUtcMs"), entitycodec::dateJson(updated));
    object.insert(QStringLiteral("revision"), static_cast<double>(revision));
    object.insert(QStringLiteral("serverRevision"), static_cast<double>(serverRevision));
    return object;
}
inline bool readNamed(const QJsonObject &object, const QString &parentKey, QString *id, QString *name,
                      QString *parent, int *order, QDateTime *created, QDateTime *updated,
                      qint64 *revision, qint64 *serverRevision, QString *error)
{
    if (!object.value(QStringLiteral("id")).isString() || !isUuid(object.value(QStringLiteral("id")).toString())
        || !object.value(QStringLiteral("name")).isString()
        || (!parentKey.isEmpty() && !entitycodec::optionalId(object, parentKey, parent)))
        return entitycodec::fail(error, QStringLiteral("备份命名实体字段非法"));
    if (parentKey.isEmpty()) parent->clear();
    qint64 number = 0;
    if (!entitycodec::readInteger(object.value(QStringLiteral("sortOrder")), 0, 1000000, &number)
        || !readRevision(object, revision, error) || !readServerRevision(object, serverRevision, error))
        return entitycodec::fail(error, QStringLiteral("备份排序或 revision 字段非法"));
    *id = object.value(QStringLiteral("id")).toString(); *name = object.value(QStringLiteral("name")).toString();
    *order = static_cast<int>(number);
    if (!entitycodec::readDate(object.value(QStringLiteral("createdAtUtcMs")), created, error)
        || !entitycodec::readDate(object.value(QStringLiteral("updatedAtUtcMs")), updated, error)
        || !created->isValid() || !updated->isValid() || name->trimmed().isEmpty())
        return entitycodec::fail(error, QStringLiteral("备份命名实体名称或时间非法"));
    return true;
}
inline QJsonObject seriesJson(const RecurrenceSeries &series)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), series.id); object.insert(QStringLiteral("rrule"), series.rrule);
    object.insert(QStringLiteral("anchorDay"), series.anchorDay); object.insert(QStringLiteral("timeZone"), series.timeZone);
    object.insert(QStringLiteral("dtstartAtUtcMs"), entitycodec::dateJson(series.dtstartAt));
    object.insert(QStringLiteral("dtstartDate"), entitycodec::allDayJson(series.dtstartDate));
    object.insert(QStringLiteral("createdAtUtcMs"), entitycodec::dateJson(series.createdAt));
    object.insert(QStringLiteral("updatedAtUtcMs"), entitycodec::dateJson(series.updatedAt));
    object.insert(QStringLiteral("revision"), static_cast<double>(series.revision));
    object.insert(QStringLiteral("serverRevision"), static_cast<double>(series.serverRevision));
    return object;
}
inline QJsonValue eventValueJson(const QVariant &value)
{
    if (!value.isValid()) return QJsonValue(QJsonValue::Null);
    return value.type() == QVariant::String ? QJsonValue(value.toString()) : QJsonValue(QJsonValue::Null);
}
inline QJsonObject eventJson(const EntityEvent &event)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), event.id); object.insert(QStringLiteral("entityId"), event.entityId);
    object.insert(QStringLiteral("entityKind"), entityKindName(event.entityKind));
    object.insert(QStringLiteral("entityTitle"), event.entityTitle); object.insert(QStringLiteral("batchId"), event.batchId);
    object.insert(QStringLiteral("type"), event.type); object.insert(QStringLiteral("field"), event.field);
    object.insert(QStringLiteral("oldValue"), eventValueJson(event.oldValue));
    object.insert(QStringLiteral("newValue"), eventValueJson(event.newValue));
    object.insert(QStringLiteral("occurredAtUtcMs"), entitycodec::dateJson(event.occurredAt));
    return object;
}
inline bool validEventType(const QString &type)
{
    const char *const types[] = {"created","changed","kind_changed","trashed","restored","deleted",
        "attachment_added","attachment_removed","reminder_added","reminder_removed"};
    for (const char *candidate : types) if (type == QLatin1String(candidate)) return true;
    return false;
}
inline bool readEventValue(const QJsonValue &value, QVariant *out, QString *error)
{
    if (value.isNull()) { *out = QVariant(); return true; }
    if (!value.isString()) return entitycodec::fail(error, QStringLiteral("事件 oldValue/newValue 必须为字符串或 null"));
    *out = QVariant(value.toString()); return true;
}
inline bool readEvent(const QJsonObject &object, EntityEvent *out, QString *error)
{
    const char *const strings[] = {"id","entityId","entityKind","entityTitle","batchId","type","field"};
    for (const char *key : strings) if (!object.value(QLatin1String(key)).isString())
        return entitycodec::fail(error, QStringLiteral("事件文本字段缺失或类型非法"));
    EntityEvent event;
    event.id = object.value(QStringLiteral("id")).toString(); event.entityId = object.value(QStringLiteral("entityId")).toString();
    event.entityKind = entityKindFromName(object.value(QStringLiteral("entityKind")).toString());
    event.entityTitle = object.value(QStringLiteral("entityTitle")).toString();
    event.batchId = object.value(QStringLiteral("batchId")).toString(); event.type = object.value(QStringLiteral("type")).toString();
    event.field = object.value(QStringLiteral("field")).toString();
    if (!isUuid(event.id) || !isUuid(event.entityId) || !isUuid(event.batchId)
        || entityKindName(event.entityKind) != object.value(QStringLiteral("entityKind")).toString()
        || event.entityTitle.trimmed().isEmpty() || !validEventType(event.type)
        || !readEventValue(object.value(QStringLiteral("oldValue")), &event.oldValue, error)
        || !readEventValue(object.value(QStringLiteral("newValue")), &event.newValue, error)
        || !entitycodec::readDate(object.value(QStringLiteral("occurredAtUtcMs")), &event.occurredAt, error)
        || !event.occurredAt.isValid()) return entitycodec::fail(error, QStringLiteral("事件内容非法"));
    *out = event; return true;
}
inline QString snapshotTimeZone(const EntitySnapshot &snapshot)
{
    for (const LocalMeta &meta : snapshot.localMeta)
        if (meta.key == QStringLiteral("default_timezone") && entitycodec::validTimeZone(meta.value)) return meta.value;
    return defaultTimeZone();
}
inline QJsonObject checklistJson(const ChecklistItem &item)
{
    QJsonObject object = entitycodec::checklistJson(item);
    object.insert(QStringLiteral("revision"), static_cast<double>(item.revision));
    object.insert(QStringLiteral("serverRevision"), static_cast<double>(item.serverRevision)); return object;
}
inline QJsonObject reminderJson(const TaskReminder &reminder)
{
    QJsonObject object = entitycodec::reminderJson(reminder);
    object.insert(QStringLiteral("revision"), static_cast<double>(reminder.revision));
    object.insert(QStringLiteral("serverRevision"), static_cast<double>(reminder.serverRevision)); return object;
}
inline QJsonObject attachmentJson(const Attachment &attachment)
{
    QJsonObject object = attachmentcodec::toJson(attachment);
    object.insert(QStringLiteral("revision"), static_cast<double>(attachment.revision));
    object.insert(QStringLiteral("serverRevision"), static_cast<double>(attachment.serverRevision)); return object;
}
inline QJsonObject toJson(const EntitySnapshot &snapshot, const QVector<EntityEvent> &events = QVector<EntityEvent>())
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), formatName()); root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("defaultTimezone"), snapshotTimeZone(snapshot));
    QJsonArray folders, lists, sections, series, entities, links, attachments, eventArray;
    QJsonArray checklist, tags, entityTags, reminders, viewDefs;
    for (const Folder &row : snapshot.folders)
        folders.append(namedJson(row.id, row.name, QString(), row.sortOrder, row.createdAt, row.updatedAt, QString(), row.revision, row.serverRevision));
    for (const TaskList &row : snapshot.lists) {
        QJsonObject object = namedJson(row.id, row.name, row.folderId, row.sortOrder, row.createdAt, row.updatedAt,
                                      QStringLiteral("folderId"), row.revision, row.serverRevision);
        object.insert(QStringLiteral("kind"), entityKindName(row.kind)); object.insert(QStringLiteral("isInbox"), row.inbox);
        lists.append(object);
    }
    for (const Section &row : snapshot.sections)
        sections.append(namedJson(row.id, row.name, row.listId, row.sortOrder, row.createdAt, row.updatedAt,
                                  QStringLiteral("listId"), row.revision, row.serverRevision));
    for (const RecurrenceSeries &row : snapshot.recurrenceSeries) series.append(seriesJson(row));
    for (const StoredEntity &row : snapshot.entities) {
        QJsonObject object = entitycodec::toJson(row.entity);
        object.insert(QStringLiteral("revision"), static_cast<double>(row.revision));
        object.insert(QStringLiteral("serverRevision"), static_cast<double>(row.serverRevision)); entities.append(object);
    }
    for (const RecurrenceLink &row : snapshot.recurrenceLinks) {
        QJsonObject object; object.insert(QStringLiteral("id"), row.id); object.insert(QStringLiteral("sourceId"), row.sourceId);
        object.insert(QStringLiteral("nextId"), row.nextId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(row.nextId));
        object.insert(QStringLiteral("nextRevision"), static_cast<double>(row.nextRevision));
        object.insert(QStringLiteral("revision"), static_cast<double>(row.revision));
        object.insert(QStringLiteral("serverRevision"), static_cast<double>(row.serverRevision)); links.append(object);
    }
    for (const Attachment &row : snapshot.attachments) attachments.append(attachmentJson(row));
    const QVector<ChecklistItem> checklistRows = !snapshot.checklistItems.isEmpty() ? snapshot.checklistItems : [&snapshot]() {
        QVector<ChecklistItem> rows; for (const StoredEntity &entity : snapshot.entities) rows.append(entity.entity.checklist); return rows;
    }();
    for (const ChecklistItem &row : checklistRows) checklist.append(checklistJson(row));
    for (const Tag &row : snapshot.tags) {
        QJsonObject object; object.insert(QStringLiteral("id"), row.id); object.insert(QStringLiteral("name"), row.name);
        object.insert(QStringLiteral("normalizedName"), row.normalizedName); object.insert(QStringLiteral("color"), row.color);
        object.insert(QStringLiteral("revision"), static_cast<double>(row.revision));
        object.insert(QStringLiteral("serverRevision"), static_cast<double>(row.serverRevision)); tags.append(object);
    }
    for (const TaskTag &row : snapshot.taskTags) {
        QJsonObject object; object.insert(QStringLiteral("id"), row.id); object.insert(QStringLiteral("entityId"), row.entityId);
        object.insert(QStringLiteral("tagId"), row.tagId); object.insert(QStringLiteral("revision"), static_cast<double>(row.revision));
        object.insert(QStringLiteral("serverRevision"), static_cast<double>(row.serverRevision)); entityTags.append(object);
    }
    const QVector<TaskReminder> reminderRows = !snapshot.reminders.isEmpty() ? snapshot.reminders : [&snapshot]() {
        QVector<TaskReminder> rows; for (const StoredEntity &entity : snapshot.entities) rows.append(entity.entity.reminders); return rows;
    }();
    for (const TaskReminder &row : reminderRows) reminders.append(reminderJson(row));
    for (const ViewDef &row : snapshot.viewDefs) {
        QJsonObject object; object.insert(QStringLiteral("id"), row.id); object.insert(QStringLiteral("name"), row.name);
        object.insert(QStringLiteral("specJson"), row.specJson); object.insert(QStringLiteral("sortOrder"), row.sortOrder);
        object.insert(QStringLiteral("createdAtUtcMs"), entitycodec::dateJson(row.createdAt));
        object.insert(QStringLiteral("updatedAtUtcMs"), entitycodec::dateJson(row.updatedAt));
        object.insert(QStringLiteral("revision"), static_cast<double>(row.revision));
        object.insert(QStringLiteral("serverRevision"), static_cast<double>(row.serverRevision)); viewDefs.append(object);
    }
    for (const EntityEvent &row : events) eventArray.append(eventJson(row));
    root.insert(QStringLiteral("folders"), folders); root.insert(QStringLiteral("lists"), lists);
    root.insert(QStringLiteral("sections"), sections); root.insert(QStringLiteral("recurrenceSeries"), series);
    root.insert(QStringLiteral("entities"), entities); root.insert(QStringLiteral("recurrenceLinks"), links);
    root.insert(QStringLiteral("attachments"), attachments); root.insert(QStringLiteral("events"), eventArray);
    root.insert(QStringLiteral("checklistItems"), checklist); root.insert(QStringLiteral("tags"), tags);
    root.insert(QStringLiteral("entityTags"), entityTags); root.insert(QStringLiteral("reminders"), reminders);
    root.insert(QStringLiteral("viewDefs"), viewDefs);
    return root;
}

inline bool fromJson(const QJsonObject &root, EntitySnapshot *out, QVector<EntityEvent> *eventsOut,
                     QString *defaultTimezoneOut, QString *error)
{
    if (!out) return entitycodec::fail(error, QStringLiteral("备份输出不能为空"));
    qint64 version = 0;
    const QString defaultTimezone = root.value(QStringLiteral("defaultTimezone")).toString();
    if (root.value(QStringLiteral("format")).toString() != formatName()
        || !entitycodec::readInteger(root.value(QStringLiteral("version")), kVersion, kVersion, &version)
        || !entitycodec::validTimeZone(defaultTimezone))
        return entitycodec::fail(error, QStringLiteral("不是受支持的 KylinList.Entities v2 备份；旧格式不迁移"));
    const char *const arrays[] = {"folders","lists","sections","recurrenceSeries","entities","recurrenceLinks",
        "attachments","events","checklistItems","tags","entityTags","reminders","viewDefs"};
    for (const char *key : arrays) if (!root.value(QLatin1String(key)).isArray())
        return entitycodec::fail(error, QStringLiteral("备份缺少必需数组"));
    EntitySnapshot snapshot;
    QSet<QString> folderIds, listIds, sectionIds, seriesIds, entityIds, attachmentIds;
    for (const QJsonValue &value : root.value(QStringLiteral("folders")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("文件夹必须为对象"));
        Folder row; QString ignoredParent;
        if (!readNamed(value.toObject(), QString(), &row.id, &row.name, &ignoredParent,
                       &row.sortOrder, &row.createdAt, &row.updatedAt, &row.revision, &row.serverRevision, error)) return false;
        if (folderIds.contains(row.id)) return entitycodec::fail(error, QStringLiteral("文件夹 id 重复"));
        folderIds.insert(row.id); snapshot.folders.append(row);
    }
    int inboxCount = 0;
    for (const QJsonValue &value : root.value(QStringLiteral("lists")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("清单必须为对象"));
        const QJsonObject object = value.toObject(); TaskList row;
        if (!readNamed(object, QStringLiteral("folderId"), &row.id, &row.name, &row.folderId,
                       &row.sortOrder, &row.createdAt, &row.updatedAt, &row.revision, &row.serverRevision, error)) return false;
        if (listIds.contains(row.id) || (!row.folderId.isEmpty() && !folderIds.contains(row.folderId))
            || !object.value(QStringLiteral("isInbox")).isBool())
            return entitycodec::fail(error, QStringLiteral("清单 id、文件夹或 inbox 字段非法"));
        const QString kind = object.value(QStringLiteral("kind")).toString();
        row.kind = entityKindFromName(kind); row.inbox = object.value(QStringLiteral("isInbox")).toBool();
        if (entityKindName(row.kind) != kind || (row.inbox && row.kind != EntityKind::Task))
            return entitycodec::fail(error, QStringLiteral("清单 kind 非法"));
        if (row.inbox) ++inboxCount;
        listIds.insert(row.id); snapshot.lists.append(row);
    }
    if (inboxCount != 1) return entitycodec::fail(error, QStringLiteral("备份必须包含唯一系统收集箱"));
    QHash<QString, QString> sectionLists;
    for (const QJsonValue &value : root.value(QStringLiteral("sections")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("section 必须为对象"));
        Section row;
        if (!readNamed(value.toObject(), QStringLiteral("listId"), &row.id, &row.name, &row.listId,
                       &row.sortOrder, &row.createdAt, &row.updatedAt, &row.revision, &row.serverRevision, error)) return false;
        if (sectionIds.contains(row.id) || !listIds.contains(row.listId)) return entitycodec::fail(error, QStringLiteral("section 引用非法"));
        sectionIds.insert(row.id); sectionLists.insert(row.id, row.listId); snapshot.sections.append(row);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("recurrenceSeries")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("series 必须为对象"));
        const QJsonObject object = value.toObject(); RecurrenceSeries row;
        if (!object.value(QStringLiteral("id")).isString() || !object.value(QStringLiteral("rrule")).isString()
            || !object.value(QStringLiteral("timeZone")).isString()) return entitycodec::fail(error, QStringLiteral("series 字段非法"));
        row.id = object.value(QStringLiteral("id")).toString(); row.rrule = object.value(QStringLiteral("rrule")).toString();
        row.timeZone = object.value(QStringLiteral("timeZone")).toString(); qint64 anchor = 0;
        if (!entitycodec::readInteger(object.value(QStringLiteral("anchorDay")), 0, 31, &anchor)
            || !readRevision(object, &row.revision, error) || !readServerRevision(object, &row.serverRevision, error)
            || !entitycodec::readDate(object.value(QStringLiteral("dtstartAtUtcMs")), &row.dtstartAt, error)
            || !entitycodec::readAllDay(object.value(QStringLiteral("dtstartDate")), &row.dtstartDate, error)
            || !entitycodec::readDate(object.value(QStringLiteral("createdAtUtcMs")), &row.createdAt, error)
            || !entitycodec::readDate(object.value(QStringLiteral("updatedAtUtcMs")), &row.updatedAt, error)) return false;
        row.anchorDay = static_cast<int>(anchor);
        if (seriesIds.contains(row.id) || !isUuid(row.id) || recurrenceRRule(row.rrule) != row.rrule
            || !entitycodec::validTimeZone(row.timeZone) || !row.createdAt.isValid() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("series 内容非法"));
        seriesIds.insert(row.id); snapshot.recurrenceSeries.append(row);
    }
    QSet<QString> occurrenceKeys;
    for (const QJsonValue &value : root.value(QStringLiteral("entities")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("实体必须为对象"));
        StoredEntity row;
        if (!entitycodec::fromJson(value.toObject(), &row.entity, error)
            || !readRevision(value.toObject(), &row.revision, error)
            || !readServerRevision(value.toObject(), &row.serverRevision, error)) return false;
        if (entityIds.contains(row.entity.id) || (!row.entity.folderId.isEmpty() && !folderIds.contains(row.entity.folderId))
            || !listIds.contains(row.entity.listId)
            || (!row.entity.sectionId.isEmpty() && (!sectionLists.contains(row.entity.sectionId)
                                                   || sectionLists.value(row.entity.sectionId) != row.entity.listId))
            || (!row.entity.recurrenceSeriesId.isEmpty() && !seriesIds.contains(row.entity.recurrenceSeriesId)))
            return entitycodec::fail(error, QStringLiteral("实体 id 或引用非法"));
        if (!row.entity.recurrenceSeriesId.isEmpty()) {
            const QString occurrence = row.entity.recurrenceSeriesId + QLatin1Char('\n') + row.entity.occurrenceKey;
            if (occurrenceKeys.contains(occurrence)) return entitycodec::fail(error, QStringLiteral("重复 occurrence 键"));
            occurrenceKeys.insert(occurrence);
            for (const RecurrenceSeries &series : snapshot.recurrenceSeries) if (series.id == row.entity.recurrenceSeriesId) {
                if (series.rrule != row.entity.rrule) return entitycodec::fail(error, QStringLiteral("实体与 series RRULE 不一致"));
                row.recurrenceAnchorDay = series.anchorDay;
            }
        }
        entityIds.insert(row.entity.id); snapshot.entities.append(row);
    }
    QHash<QString, QString> parentById;
    for (const StoredEntity &row : snapshot.entities) {
        parentById.insert(row.entity.id, row.entity.parentId);
        if (!row.entity.parentId.isEmpty()) {
            const int parent = entityPosition(snapshot, row.entity.parentId);
            if (parent < 0 || row.entity.kind != EntityKind::Task || snapshot.entities.at(parent).entity.kind != EntityKind::Task)
                return entitycodec::fail(error, QStringLiteral("子实体父引用非法"));
        }
    }
    for (const StoredEntity &row : snapshot.entities) {
        QSet<QString> path; QString id = row.entity.id;
        while (!id.isEmpty()) {
            if (path.contains(id)) return entitycodec::fail(error, QStringLiteral("子实体关系形成循环"));
            path.insert(id); id = parentById.value(id);
        }
    }

    QHash<QString, ChecklistItem> nestedChecklist;
    for (const StoredEntity &entity : snapshot.entities) for (const ChecklistItem &item : entity.entity.checklist) {
        if (nestedChecklist.contains(item.id)) return entitycodec::fail(error, QStringLiteral("嵌套清单项 id 重复"));
        nestedChecklist.insert(item.id, item);
    }
    QSet<QString> checklistIds;
    for (const QJsonValue &value : root.value(QStringLiteral("checklistItems")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("独立清单项必须为对象"));
        ChecklistItem row; qint64 revision = 0;
        if (!entitycodec::readChecklist(value.toObject(), &row, error)
            || !readRevision(value.toObject(), &revision, error)
            || !readServerRevision(value.toObject(), &row.serverRevision, error))
            return entitycodec::fail(error, QStringLiteral("独立清单项字段或 revision 非法"));
        row.revision = revision;
        const auto nested = nestedChecklist.constFind(row.id);
        if (checklistIds.contains(row.id) || !entityIds.contains(row.entityId) || nested == nestedChecklist.constEnd()
            || nested->entityId != row.entityId || nested->text != row.text || nested->done != row.done
            || nested->sortOrder != row.sortOrder || nested->createdAt != row.createdAt || nested->updatedAt != row.updatedAt
            || !row.createdAt.isValid() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("独立清单项与实体嵌套内容不一致"));
        checklistIds.insert(row.id); snapshot.checklistItems.append(row);
    }
    if (checklistIds.size() != nestedChecklist.size()) return entitycodec::fail(error, QStringLiteral("独立清单项数量不完整"));

    QSet<QString> tagIds, tagNames;
    QHash<QString, Tag> tagsById;
    for (const QJsonValue &value : root.value(QStringLiteral("tags")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("标签必须为对象"));
        const QJsonObject object = value.toObject(); Tag row;
        row.id = object.value(QStringLiteral("id")).toString(); row.name = object.value(QStringLiteral("name")).toString();
        row.normalizedName = object.value(QStringLiteral("normalizedName")).toString(); row.color = object.value(QStringLiteral("color")).toString();
        if (!object.value(QStringLiteral("id")).isString() || !object.value(QStringLiteral("name")).isString()
            || !object.value(QStringLiteral("normalizedName")).isString() || !object.value(QStringLiteral("color")).isString()
            || !readRevision(object, &row.revision, error) || !readServerRevision(object, &row.serverRevision, error)
            || !isUuid(row.id) || row.name.trimmed().isEmpty()
            || row.normalizedName != row.name.trimmed().toLower() || tagIds.contains(row.id) || tagNames.contains(row.normalizedName))
            return entitycodec::fail(error, QStringLiteral("标签字段、revision 或唯一性非法"));
        tagIds.insert(row.id); tagNames.insert(row.normalizedName); tagsById.insert(row.id, row); snapshot.tags.append(row);
    }
    QSet<QString> entityTagIds, entityTagPairs;
    QHash<QString, QStringList> linkedTagNames;
    for (const QJsonValue &value : root.value(QStringLiteral("entityTags")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("实体标签关系必须为对象"));
        const QJsonObject object = value.toObject(); TaskTag row;
        row.id = object.value(QStringLiteral("id")).toString(); row.entityId = object.value(QStringLiteral("entityId")).toString();
        row.tagId = object.value(QStringLiteral("tagId")).toString();
        const QString pair = row.entityId + QLatin1Char('\n') + row.tagId;
        if (!isUuid(row.id) || !entityIds.contains(row.entityId) || !tagsById.contains(row.tagId)
            || !readRevision(object, &row.revision, error) || !readServerRevision(object, &row.serverRevision, error)
            || entityTagIds.contains(row.id) || entityTagPairs.contains(pair))
            return entitycodec::fail(error, QStringLiteral("实体标签关系非法或重复"));
        entityTagIds.insert(row.id); entityTagPairs.insert(pair); linkedTagNames[row.entityId].append(tagsById.value(row.tagId).name);
        snapshot.taskTags.append(row);
    }
    for (const StoredEntity &entity : snapshot.entities) {
        QStringList expected = linkedTagNames.value(entity.entity.id), actual = entity.entity.tags;
        expected.sort(); actual.sort();
        if (expected != actual) return entitycodec::fail(error, QStringLiteral("实体标签与 entityTags 不一致"));
    }

    QHash<QString, TaskReminder> nestedReminders;
    for (const StoredEntity &entity : snapshot.entities) for (const TaskReminder &reminder : entity.entity.reminders) {
        if (nestedReminders.contains(reminder.id)) return entitycodec::fail(error, QStringLiteral("嵌套提醒 id 重复"));
        nestedReminders.insert(reminder.id, reminder);
    }
    QSet<QString> reminderIds;
    for (const QJsonValue &value : root.value(QStringLiteral("reminders")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("独立提醒必须为对象"));
        TaskReminder row;
        if (!entitycodec::readReminder(value.toObject(), &row, error)
            || !readRevision(value.toObject(), &row.revision, error)
            || !readServerRevision(value.toObject(), &row.serverRevision, error)) return false;
        const auto nested = nestedReminders.constFind(row.id);
        if (reminderIds.contains(row.id) || !entityIds.contains(row.entityId) || nested == nestedReminders.constEnd()
            || nested->entityId != row.entityId || nested->triggerAt != row.triggerAt || nested->sortOrder != row.sortOrder
            || nested->createdAt != row.createdAt || nested->updatedAt != row.updatedAt
            || !row.createdAt.isValid() || !row.updatedAt.isValid())
            return entitycodec::fail(error, QStringLiteral("独立提醒与实体嵌套内容不一致"));
        reminderIds.insert(row.id); snapshot.reminders.append(row);
    }
    if (reminderIds.size() != nestedReminders.size()) return entitycodec::fail(error, QStringLiteral("独立提醒数量不完整"));

    QSet<QString> viewIds;
    for (const QJsonValue &value : root.value(QStringLiteral("viewDefs")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("视图定义必须为对象"));
        const QJsonObject object = value.toObject(); ViewDef row; qint64 order = 0;
        row.id = object.value(QStringLiteral("id")).toString(); row.name = object.value(QStringLiteral("name")).toString();
        row.specJson = object.value(QStringLiteral("specJson")).toString();
        if (!isUuid(row.id) || row.name.trimmed().isEmpty() || !object.value(QStringLiteral("specJson")).isString()
            || !entitycodec::readInteger(object.value(QStringLiteral("sortOrder")), 0, 1000000, &order)
            || !readRevision(object, &row.revision, error)
            || !entitycodec::readInteger(object.value(QStringLiteral("serverRevision")), 0, kMaximumRevision, &row.serverRevision)
            || !entitycodec::readDate(object.value(QStringLiteral("createdAtUtcMs")), &row.createdAt, error)
            || !entitycodec::readDate(object.value(QStringLiteral("updatedAtUtcMs")), &row.updatedAt, error)
            || !row.createdAt.isValid() || !row.updatedAt.isValid() || viewIds.contains(row.id))
            return entitycodec::fail(error, QStringLiteral("视图定义字段非法"));
        QJsonParseError parseError; const QJsonDocument spec = QJsonDocument::fromJson(row.specJson.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !spec.isObject()) return entitycodec::fail(error, QStringLiteral("视图定义 JSON 非法"));
        row.sortOrder = static_cast<int>(order); viewIds.insert(row.id); snapshot.viewDefs.append(row);
    }

    QSet<QString> sources, targets, linkIds;
    for (const QJsonValue &value : root.value(QStringLiteral("recurrenceLinks")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("重复凭证必须为对象"));
        const QJsonObject object = value.toObject(); RecurrenceLink row;
        if (!object.value(QStringLiteral("id")).isString() || !isUuid(object.value(QStringLiteral("id")).toString())
            || !object.value(QStringLiteral("sourceId")).isString() || !entitycodec::optionalId(object, QStringLiteral("nextId"), &row.nextId)
            || !entitycodec::readInteger(object.value(QStringLiteral("nextRevision")), 1, kMaximumRevision, &row.nextRevision)
            || !readRevision(object, &row.revision, error) || !readServerRevision(object, &row.serverRevision, error))
            return entitycodec::fail(error, QStringLiteral("重复凭证字段非法"));
        row.id = object.value(QStringLiteral("id")).toString(); row.sourceId = object.value(QStringLiteral("sourceId")).toString();
        const int source = entityPosition(snapshot, row.sourceId), next = entityPosition(snapshot, row.nextId);
        if (source < 0 || row.sourceId == row.nextId || snapshot.entities.at(source).entity.status != TaskStatus::Completed || sources.contains(row.sourceId)
            || linkIds.contains(row.id)
            || (!row.nextId.isEmpty() && (next < 0 || targets.contains(row.nextId) || row.nextRevision > snapshot.entities.at(next).revision)))
            return entitycodec::fail(error, QStringLiteral("重复凭证引用非法"));
        linkIds.insert(row.id); sources.insert(row.sourceId); if (!row.nextId.isEmpty()) targets.insert(row.nextId);
        snapshot.recurrenceLinks.append(row);
    }
    QSet<QString> attachmentPaths;
    for (const QJsonValue &value : root.value(QStringLiteral("attachments")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("附件元数据必须为对象"));
        Attachment row;
        if (!attachmentcodec::fromJson(value.toObject(), &row, error)
            || !readRevision(value.toObject(), &row.revision, error)
            || !readServerRevision(value.toObject(), &row.serverRevision, error)) return false;
        if (attachmentIds.contains(row.id) || attachmentPaths.contains(row.relativePath) || !entityIds.contains(row.entityId))
            return entitycodec::fail(error, QStringLiteral("附件 id、路径或实体引用非法"));
        attachmentIds.insert(row.id); attachmentPaths.insert(row.relativePath); snapshot.attachments.append(row);
    }
    QVector<EntityEvent> events; QSet<QString> eventIds;
    for (const QJsonValue &value : root.value(QStringLiteral("events")).toArray()) {
        if (!value.isObject()) return entitycodec::fail(error, QStringLiteral("事件必须为对象"));
        EntityEvent event; if (!readEvent(value.toObject(), &event, error)) return false;
        if (eventIds.contains(event.id)) return entitycodec::fail(error, QStringLiteral("事件 id 重复"));
        eventIds.insert(event.id); events.append(event);
    }
    *out = snapshot;
    if (eventsOut) *eventsOut = events;
    if (defaultTimezoneOut) *defaultTimezoneOut = defaultTimezone;
    if (error) error->clear(); return true;
}
} // namespace backupcodec
} // namespace kl

#endif // KYLINLIST_BACKUPCODEC_H
