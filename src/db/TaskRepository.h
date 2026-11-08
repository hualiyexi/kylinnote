#ifndef KYLINLIST_TASKREPOSITORY_H
#define KYLINLIST_TASKREPOSITORY_H

#include "../domain/Attachment.h"
#include "../domain/Entity.h"

#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <functional>

namespace kl {

struct StoredEntity {
    Entity entity;
    qint64 revision = 0;             // 0 = 尚未插入
    qint64 serverRevision = 0;
    int recurrenceAnchorDay = 0;
};
struct RecurrenceLink {
    QString id;
    QString sourceId;
    QString nextId;                  // 空 = 后续实例已永久删除，凭证仍保留
    qint64 nextRevision = 0;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

struct EntitySnapshot {
    QVector<Folder> folders;
    QVector<TaskList> lists;
    QVector<Section> sections;
    QVector<RecurrenceSeries> recurrenceSeries;
    QVector<StoredEntity> entities;
    QVector<RecurrenceLink> recurrenceLinks;
    QVector<ChecklistItem> checklistItems;
    QVector<Tag> tags;
    QVector<TaskTag> taskTags;
    QVector<TaskReminder> reminders;
    QVector<ReminderDelivery> reminderDeliveries;
    QVector<Attachment> attachments;
    QVector<DesktopNote> desktopNotes;
    QVector<ViewDef> viewDefs;
    QVector<SyncBinding> syncBindings;
    QVector<LocalMeta> localMeta;
    QVector<Tombstone> tombstones;
    QVector<OutboxItem> outbox;
};

struct EntityChanges {
    QVector<Folder> folderSaves;
    QStringList folderRemovals;
    QVector<TaskList> groupSaves;     // group 是现有 UI 对 list 的门面称呼
    QStringList groupRemovals;
    QVector<Section> sectionSaves;
    QStringList sectionRemovals;
    QVector<RecurrenceSeries> seriesSaves;
    QVector<StoredEntity> saves;
    QVector<StoredEntity> removals;
    // 不覆盖任务正文字段的关联写入，用 CAS 推进 revision/updated_at。
    QVector<StoredEntity> touches;
    QVector<EntityEvent> eventSaves; // 仅随 apply 追加，不进入全量快照
    QVector<ChecklistItem> checklistSaves;
    QStringList checklistRemovals;
    QVector<Tag> tagSaves;
    QVector<TaskTag> taskTagSaves;
    QVector<TaskTag> taskTagRemovals;
    QVector<TaskReminder> reminderSaves;
    QStringList reminderRemovals;
    QVector<ReminderDelivery> deliverySaves;
    QVector<Attachment> attachmentSaves;
    QVector<Attachment> attachmentRemovals;
    QVector<DesktopNote> desktopNoteSaves;
    QStringList desktopNoteRemovals;
    QVector<RecurrenceLink> recurrenceLinkSaves;
    QStringList removeLinks;
    QString generationSourceId;       // 仅用于 reset 信号；凭证在 recurrenceLinkSaves
    int generatedTaskIndex = -1;
};

class TaskRepository
{
public:
    explicit TaskRepository(const QSqlDatabase &database);
    bool load(EntitySnapshot *out, QString *error = nullptr) const;
    bool apply(EntityChanges *changes, EntitySnapshot *after, QString *error = nullptr);
    bool appendEvents(const QVector<EntityEvent> &events, QString *error = nullptr);
    QVector<EntityEvent> eventsFor(const QString &entityId, QString *error = nullptr) const;
    QVector<EntityEvent> allEvents(QString *error = nullptr) const;
    bool convertKind(const QString &id, EntityKind to, EntitySnapshot *after, QString *error = nullptr);
    bool convertKind(const QString &id, EntityKind to, QString *error)
    {
        EntitySnapshot ignored; return convertKind(id, to, &ignored, error);
    }
    using BeforeCommit = std::function<bool(QString *)>;
    bool replaceAll(const EntitySnapshot &incoming, const QVector<EntityEvent> &events,
                    const QString &defaultTimezone, const QDateTime &importedAt,
                    EntitySnapshot *after, QString *error = nullptr,
                    const BeforeCommit &beforeCommit = BeforeCommit());
    bool setLocalMeta(const QString &key, const QString &value, QString *error = nullptr);

private:
    bool readSnapshot(EntitySnapshot *out, QString *error) const;
    bool saveTask(StoredEntity *stored, QString *error);
    bool saveFolder(Folder *folder, QString *error);
    bool saveGroup(TaskList *group, QString *error);
    bool saveSection(Section *section, QString *error);
    bool saveSeries(RecurrenceSeries *series, QString *error);
    bool saveChecklist(ChecklistItem *item, QString *error);
    bool saveTag(Tag *tag, QString *error);
    bool saveReminder(TaskReminder *reminder, QString *error);
    bool saveDelivery(const ReminderDelivery &delivery, QString *error);
    bool saveAttachment(const Attachment &attachment, QString *error);
    bool saveDesktopNote(DesktopNote *note, QString *error);
    bool insertLink(const RecurrenceLink &link, QString *error);
    bool touchTask(const StoredEntity &stored, QString *error);
    bool removeTask(const StoredEntity &stored, QString *error);
    bool tombstone(const QString &type, const QString &id, QString *error);
    bool queueOutbox(const EntityChanges &changes, QString *error);
    bool appendEventsInTransaction(QVector<EntityEvent> *events, QString *error);
    bool ready(QString *error) const;

    QSqlDatabase m_database;
};

} // namespace kl

#endif // KYLINLIST_TASKREPOSITORY_H
