#ifndef KYLINLIST_TASKSTORE_H
#define KYLINLIST_TASKSTORE_H

#include "../db/TaskRepository.h"
#include "AttachmentStore.h"
#include "Journal.h"

#include <QObject>
#include <QSqlDatabase>
#include <memory>

namespace kl {

class TaskStore : public QObject
{
    Q_OBJECT
public:
    explicit TaskStore(const QSqlDatabase &database, QObject *parent = nullptr);
    bool load(QString *error = nullptr);

    QVector<Entity> entities() const;
    QVector<TaskGroup> groups() const;
    QVector<TaskList> lists() const;
    QVector<Folder> folders() const;
    QVector<Section> sections() const;
    Entity entity(const QString &id) const;
    QString createTask(const QString &title, const QString &listId = QString(),
                       const QString &sectionId = QString(), const QString &parentId = QString());
    QString createNote(const QString &title, const QString &listId = QString(),
                       const QString &sectionId = QString());
    bool saveTask(Entity task);
    QString cloneTask(const QString &id);
    bool convertKind(const QString &id, EntityKind kind);
    bool setCompleted(const QString &id, bool completed);
    bool setStatus(const QString &id, TaskStatus status);
    bool abandonTask(const QString &id);
    bool setPinnedAt(const QString &id, const QDateTime &pinnedAt);
    bool trashTask(const QString &id);
    bool restoreTask(const QString &id);
    bool deletePermanently(const QString &id);

    QString createFolder(const QString &name);
    bool renameFolder(const QString &id, const QString &name);
    bool deleteFolder(const QString &id);
    QString createList(const QString &name, const QString &folderId = QString(), EntityKind kind = EntityKind::Task);
    bool renameList(const QString &id, const QString &name);
    bool setListFolder(const QString &id, const QString &folderId);
    bool deleteList(const QString &id);
    QString createGroup(const QString &name) { return createList(name); }
    bool renameGroup(const QString &id, const QString &name) { return renameList(id, name); }
    bool deleteGroup(const QString &id) { return deleteList(id); }
    QString createSection(const QString &listId, const QString &name);
    bool renameSection(const QString &id, const QString &name);
    bool deleteSection(const QString &id);

    QString addChecklistItem(const QString &entityId, const QString &text);
    bool setChecklistDone(const QString &itemId, bool done);
    bool removeChecklistItem(const QString &itemId);
    QVector<TaskReminder> reminders(const QString &entityId) const;
    TaskReminder addReminder(const QString &entityId, const QDateTime &triggerAt);
    bool removeReminder(const QString &reminderId);
    bool markReminderDelivered(const QString &reminderId, const QDateTime &expectedTrigger, const QDateTime &now);
    bool markReminderNotified(const QString &entityId, const QDateTime &expectedReminder, const QDateTime &now);
    bool isReminderDelivered(const QString &reminderId) const;
    QString installationId() const;
    QString deviceId() const;
    QString inboxListId() const;

    // 桌面状态按 device_id 独立；不再存放在 entities 表。
    DesktopNote desktopNote(const QString &entityId, const QString &device = QString()) const;
    bool saveDesktopNote(DesktopNote note);
    bool saveDesktopState(const QString &entityId, NoteColor color, const QRect &geometry,
                          int zOrder, NoteLayer layer, bool open = true);
    bool removeDesktopNote(const QString &entityId, const QString &device = QString());

    QVector<Attachment> attachments(const QString &entityId) const;
    QString attachmentFilePath(const Attachment &attachment) const;
    Attachment importAttachment(const QString &entityId, const QString &sourcePath);
    bool removeAttachment(const QString &entityId, const QString &attachmentId);
    bool refreshAttachmentState(const QString &attachmentId);
    bool openAttachment(const QString &attachmentId);
    bool copyAttachmentTo(const QString &attachmentId, const QString &destinationPath);

    QVector<EntityEvent> events(const QString &entityId);
    QString beginEditSession(const QString &entityId);
    bool endEditSession(const QString &sessionId);
    bool endAllSessions();

    QString localMeta(const QString &key) const;
    bool setLocalMeta(const QString &key, const QString &value);
    QVector<OutboxItem> outbox() const { return m_snapshot.outbox; }

    // 仅接受 KylinList.Entities v2 + KLB1 magic 的新 .klb 容器；不兼容旧备份。
    bool exportBackup(const QString &path);
    bool importBackup(const QString &path);
    QString lastError() const { return m_lastError; }

signals:
    void taskChanged(QString id);
    void tasksReset();
    void groupsChanged();
    void attachmentsChanged(QString entityId);
    void desktopNoteChanged(QString entityId);
    void errorOccurred(const QString &message);

private:
    const StoredEntity *find(const QString &id) const;
    const RecurrenceLink *linkFor(const QString &sourceId) const;
    const Attachment *findAttachment(const QString &id) const;
    const TaskReminder *findReminder(const QString &id) const;
    const ChecklistItem *findChecklistItem(const QString &id) const;
    int nextAttachmentSortOrder(const QString &entityId) const;
    bool hasGroup(const QString &id) const;
    bool hasFolder(const QString &id) const;
    bool hasSection(const QString &id) const;
    bool fail(const QString &message, QString *error = nullptr);
    bool ensureLoaded();
    bool commit(EntityChanges *changes, bool reset = false);
    bool prepareSave(Entity task, EntityChanges *changes);
    void recordSaveEvents(const StoredEntity *previous, const Entity &value, EntityChanges *changes, const QDateTime &now);
    void applyDesktopFacade(Entity *task) const;

    TaskRepository m_repository;
    EntitySnapshot m_snapshot;
    Journal m_journal;
    std::unique_ptr<AttachmentStore> m_attachmentStore;
    QString m_lastError;
    bool m_loaded = false;
};

} // namespace kl

#endif // KYLINLIST_TASKSTORE_H
