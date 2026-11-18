#ifndef KYLINLIST_TASKSTORE_H
#define KYLINLIST_TASKSTORE_H

#include "../db/TaskRepository.h"
#include "../domain/ScheduleValue.h"
#include "../domain/TaskDrop.h"
#include "../domain/TextDraft.h"
#include "AttachmentStore.h"
#include "Journal.h"
#include "UsageStatistics.h"

#include <QDate>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
#include <functional>
#include <memory>

namespace kl {

class EntityTextSession;

class TaskStore : public QObject
{
    Q_OBJECT
public:
    explicit TaskStore(const QSqlDatabase &database, QObject *parent = nullptr);
    ~TaskStore() override;
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
    // 完整日期编辑入口；可保存尚未创建的草稿，创建时返回新实体 id。
    bool saveSchedule(Entity task, const ScheduleValue& schedule, QString* savedId = nullptr);
    ScheduleValue schedule(const QString& entityId) const;
    bool recurrenceSpec(const QString& entityId, RecurrenceSpec* spec) const;
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
    // 类型化拖放：从 repository 重读完整快照，验证落点后一次原子保存。
    // orderedIds 只提供可见顺序，清单迁移的后代闭包绝不依赖可见行；Created 与
    // 终态目标拒绝。跨组写真实日期/优先级/清单及置顶属性，独立子项脱父；
    // 同组仅排序，AsChild 仅设父和排序，保留日期/优先级/置顶及源子树关系。
    bool applyDropIntent(const TaskDrop::Intent &intent);
    // 详情清单选择器与 MoveTo 共用：独立子项脱父，完整子树（包括隐藏、终态
    // 和回收站后代）一起迁移、清空旧 section；幂等零写，CAS 失败整批回滚。
    bool moveEntityToList(const QString &entityId, const QString &listId);
    QString createSection(const QString &listId, const QString &name);
    bool renameSection(const QString &id, const QString &name);
    bool deleteSection(const QString &id);

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
    QSet<QString> attachmentEntityIds() const;
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
    // 会话 id 是否仍处于活动状态（endAllSessions/导入等外部终结后返回 false）。
    bool isEditSessionActive(const QString &sessionId) const;
    // 每实体一个共享文本编辑会话（TaskDetailWidget 与桌面便签共用同一权威草稿）。
    // 首次调用按当前快照创建并以本 store 为父；之后同 id 返回同一实例。目标不存在/
    // 已删除（含回收站）或未加载时返回 nullptr。
    EntityTextSession *textSession(const QString &entityId);
    // 正文字段（title/body）按 baseline/proposed 三方校验提交，供 TaskDetailWidget 与桌面便签共用：
    // 不相关字段一律沿用当前快照；本地未变不覆盖远端；远端已等于本地视为成功零写入；
    // 同字段双方都改且不同返回 Conflict（零写入，不碰 revision/updated_at/事件）；
    // 目标不存在或已删除（含回收站）返回 Unavailable，不可复活；有效改变走既有
    // saveTask/CAS/Journal 链，失败（CAS/SQL/校验）返回 Failed 且事务回滚。
    // draft.sessionId 可选：成功写入后登记到该会话，供 cancelEditSession 精确归因事件。
    TextCommitResult commitTextDraft(const TextDraft &draft);
    // 丢弃编辑会话（重载草稿/目标被永久删除后关闭窗口）：只终结该 sessionId，不影响其他
    // 编辑器会话；不写实体。未保存草稿取消零事件（目标已消失也幂等成功）；本会话早先有
    // 成功写入则只补记自己真正写入的字段终值，该日志提交失败返回 false 并保留会话供重试
    //（与 endEditSession 口径一致）。未知/已结束会话幂等返回 true。
    bool cancelEditSession(const QString &sessionId);

    QString localMeta(const QString &key) const;
    bool setLocalMeta(const QString &key, const QString &value);
    QVector<OutboxItem> outbox() const { return m_snapshot.outbox; }

    // 仅支持当前 KylinList.Entities v6 + KLB1 容器；开发期不做旧格式导入。
    bool exportBackup(const QString &path);
    bool importBackup(const QString &path);
    QString lastError() const { return m_lastError; }

signals:
    void taskChanged(QString id);
    void tasksReset();
    void groupsChanged();
    void attachmentsChanged(QString entityId);
    void eventsChanged(QString entityId);
    void desktopNoteChanged(QString entityId);
    void errorOccurred(const QString &message);

private:
    // 安全清空共享文本会话注册表（析构与成功导入复用）：先换出哈希再删除，
    // 会话 destroyed 钩子只会清理已换出的空表，绝不迭代中被改写。
    void destroyTextSessions();
    const StoredEntity *find(const QString &id) const;
    const TaskList *findList(const QString &id) const;
    const RecurrenceLink *linkFor(const QString &sourceId) const;
    const Attachment *findAttachment(const QString &id) const;
    const TaskReminder *findReminder(const QString &id) const;
    int nextAttachmentSortOrder(const QString &entityId) const;
    int nextListSortOrder(const QString &folderId) const;   // 容器尾部；顶层同时考虑文件夹共用序列
    int nextFolderSortOrder() const;                        // 顶层清单/文件夹共用序列尾部
    bool hasGroup(const QString &id) const;
    bool hasFolder(const QString &id) const;
    bool hasSection(const QString &id) const;
    bool fail(const QString &message, QString *error = nullptr);
    bool ensureLoaded();
    // beforeSignals：事务落库成功后、替换快照与发出 taskChanged 等对外信号前执行的一次性钩子
    //（commitTextDraft 用于登记编辑会话写入归因，不受信号回调的后续改动影响）。
    bool commit(EntityChanges *changes, bool reset = false,
                const std::function<void()> &beforeSignals = std::function<void()>());
    enum class SavePurpose { Edit, GroupMove };
    bool commitSnapshotUpdates(const EntitySnapshot &snapshot, const QVector<Entity> &updates);
    bool prepareSave(Entity task, EntityChanges *changes, SavePurpose purpose = SavePurpose::Edit,
                     const ScheduleValue* schedule = nullptr);
    void recordSaveEvents(const StoredEntity *previous, const Entity &value, EntityChanges *changes, const QDateTime &now);
    void applyDesktopFacade(Entity *task) const;

    TaskRepository m_repository;
    EntitySnapshot m_snapshot;
    Journal m_journal;
    std::unique_ptr<AttachmentStore> m_attachmentStore;
    QHash<QString, EntityTextSession *> m_textSessions;   // 每实体共享文本会话注册表
    QString m_lastError;
    bool m_loaded = false;
    // 仅在 commitTextDraft 携带有效会话归因保存期间为 true：这类保存的 title/body
    // 差异由所属编辑会话结束/取消时统一记账，recordSaveEvents 据此跳过直存事件。
    bool m_textCommitSave = false;
};

} // namespace kl

#endif // KYLINLIST_TASKSTORE_H
