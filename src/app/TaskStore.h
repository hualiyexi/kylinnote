#ifndef KYLINLIST_TASKSTORE_H
#define KYLINLIST_TASKSTORE_H

#include "../db/TaskRepository.h"
#include "../domain/TaskDrop.h"
#include "AttachmentStore.h"
#include "Journal.h"
#include "UsageStatistics.h"

#include <QDate>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
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
    // 在 parentId 下创建子任务：父项必须是存在、未删除、active 的任务。
    // 子任务继承 listId/sectionId/allDayDate/startAt/dueAt/timeZone（时间视图立即可见），
    // 不继承置顶、提醒、重复与完成状态；sortOrder 为同级末尾 max+1。
    // 复用 createTask 的 prepareSave/commit 与父子校验，失败返回空 id。
    QString createSubtask(const QString &title, const QString &parentId);
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

    QString createFolder(const QString &name, const QString &icon = QString());
    bool renameFolder(const QString &id, const QString &name);
    // 文件夹完整编辑入口：name trim 后 1..64（未改名的原名更新保留旧库原文）、icon（空或
    // ≤16 个 UTF-16 码元）、sortOrder；草稿 revision 必须与当前快照一致，repository 再作 CAS。
    bool saveFolder(const Folder &folder);
    bool deleteFolder(const QString &id);
    QString createList(const QString &name, const QString &folderId = QString(), EntityKind kind = EntityKind::Task,
                       const QString &icon = QString(), const QString &color = QString());
    bool renameList(const QString &id, const QString &name);
    bool setListFolder(const QString &id, const QString &folderId);
    // 清单组织原子命令：把 source 移到 targetFolderId（空 = 平铺）内 targetListId 之前/之后；
    // targetListId 空 = 容器末尾（顶层为“平铺清单+文件夹”混合序列的末尾）。source/target 都从
    // 当前快照重读；拒绝 inbox、已归档、不存在、锚点不属于目标文件夹。整批一次 EntityChanges + commit，
    // 受影响容器规范为非负连续 sortOrder（0..n-1）：顶层规范化混合序列（含文件夹，文件夹仅在
    // sortOrder 变化时保存），文件夹容器规范化纯清单序列；跨容器移动后源容器同样规范化。
    bool moveList(const QString &sourceId, const QString &targetFolderId,
                  const QString &targetListId = QString(), bool after = false);
    // 从两个清单建文件夹：target 必须是平铺普通未归档清单，source 可以是其他普通未归档清单
    // （不能是 inbox）。同一事务内创建文件夹并把顶层混合序列规范化，使新文件夹精确占据
    // target 原 UI 位置；两清单移入新文件夹并保持原相对顺序；source 原文件夹的剩余清单规范化。
    // 旧全 0 数据按旧 UI 口径（平铺清单在前、文件夹在后、各自 (sortOrder,id)）确定初始顺序。
    // 失败无半成品。返回新文件夹 id。
    QString createFolderFromLists(const QString &name, const QString &icon,
                                  const QString &sourceId, const QString &targetId);
    // 完整清单一事务更新：新建/改名 trim 后 1..64，旧长名称未改名时保留；
    // icon（空或 ≤16 个 UTF-16 码元的 emoji 文本）、color（空或 #RRGGBB）、
    // folderId（空 = 无文件夹）。不能改变 inbox/kind/归档状态。
    bool updateList(const QString &id, const QString &name, const QString &icon, const QString &color,
                    const QString &folderId);
    // saveList 要求草稿 revision 与当前快照一致；repository 再以数据库 revision 作 CAS。
    bool saveList(const TaskList &list);          // kind/inbox/archivedAtUtcMs 以当前快照为准
    // 归档只改清单本身（archivedAtUtcMs），绝不批量改任务状态、日期或生成重复实例；
    // 收集箱禁止归档。归档清单的任务不再提醒、不进活动视图。
    bool setListArchived(const QString &id, bool archived);
    bool isListArchived(const QString &id) const;
    QSet<QString> archivedListIds() const;        // 供 ViewSpec 归档过滤上下文使用
    bool deleteList(const QString &id);
    QString createGroup(const QString &name) { return createList(name); }
    bool renameGroup(const QString &id, const QString &name) { return renameList(id, name); }
    bool deleteGroup(const QString &id) { return deleteList(id); }
    // 顺延至今天：只处理 active、未删除、未归档清单且 effectiveDate<today 的任务。
    // 按 effectiveDate 到 today 的天数统一平移 allDayDate/due/start；
    // 带时刻的值在应用时区下保留时刻和跨日跨度；提醒及其余字段不动。
    // 单事务提交，失败不产生部分成功；返回 false 时可用 lastError() 查原因。
    bool postponeOverdueTasks(const QStringList &ids, const QDate &today);
    // 拖放落点的原子命令：把 sourceId 放进 targetSection 的 orderedIds 顺序。
    // orderedIds 是目标段当前显示顺序（已含 source 在目标位置）；store 逐 ID 从当前
    // 快照重读并校验存在、无重复、必含 source，禁止用公开 Entity 的陈旧草稿。
    // targetSection="list:<id>" 时，目标清单必须存在且未归档，source 与 orderedIds
    // 中所有实体必须属于该清单；跨清单投放整批拒绝。
    // 一次 EntityChanges + commit 把 orderedIds 的 sortOrder 规范为 0..n-1，只保存
    // 实际变化的行；adjustPinned=true 时才修改 source 的 pinnedAt：pinned 置位时
    // 未置顶取当前时刻、已置顶保持原值，清除时清空 pinnedAt；detachParent 清 parentId。
    // 时间分区（TaskDrop::isTimeSection）要求 targetDate 与 TaskDrop::dateForSection
    // 的映射一致，并按 source effectiveDate 到 targetDate 平移 allDayDate/startAt/dueAt
    // （应用时区下保留时刻与跨度，提醒/重复不动）；无有效原日期的时间分区拖入拒绝。
    // 非时间分区要求 targetDate 无效，不暗改日期。adjustPinned=true 且
    // targetSection="pinned" 时 pinned 必须为 true（明显矛盾拒绝）。adjustDate=false
    // 用于同段手动排序：targetDate 必须无效，保留“最近7天”段内原有的后天以后日期。
    // adjustPinned=false 用于同父、同显示分区排序，保留实际 pinnedAt（含未置顶子任务）。
    // targetParentId 非空时表示右下半区设父：源与父项必须都是同清单、未删除的进行中任务，
    // 完整祖先链不得包含源；此时 detachParent/adjustDate/adjustPinned 必须均为 false，
    // 只更新父子关系与目标顺序，不暗改日期或置顶。源已有后代保持原 parentId。
    // 失败整批无部分成功；幂等返回 true。
    bool moveEntityForDrop(const QString &sourceId, const QStringList &orderedIds,
                           const QString &targetSection, const QDate &targetDate,
                           bool pinned, bool detachParent, bool adjustDate = true,
                           bool adjustPinned = true, const QString &targetParentId = QString());
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
    // 从事件日志汇总本机累计使用统计；早期无事件的实体用当前快照补齐。
    // 只在计算内部用实体 id 去重，返回值不含任何 id 或内容。
    UsageStatistics usageStatistics(QString *error = nullptr);
    QString beginEditSession(const QString &entityId);
    bool endEditSession(const QString &sessionId);
    bool endAllSessions();

    QString localMeta(const QString &key) const;
    bool setLocalMeta(const QString &key, const QString &value);
    QVector<OutboxItem> outbox() const { return m_snapshot.outbox; }

    // 仅接受 KylinList.Entities v2/v3 + KLB1 magic 的新 .klb 容器；不兼容旧备份。
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
    const TaskList *findList(const QString &id) const;
    const RecurrenceLink *linkFor(const QString &sourceId) const;
    const Attachment *findAttachment(const QString &id) const;
    const TaskReminder *findReminder(const QString &id) const;
    const ChecklistItem *findChecklistItem(const QString &id) const;
    int nextAttachmentSortOrder(const QString &entityId) const;
    int nextListSortOrder(const QString &folderId) const;   // 容器尾部；顶层同时考虑文件夹共用序列
    int nextFolderSortOrder() const;                        // 顶层清单/文件夹共用序列尾部
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
