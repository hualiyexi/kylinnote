#ifndef KYLINNOTE_TASKREPOSITORY_H
#define KYLINNOTE_TASKREPOSITORY_H

#include "../domain/Task.h"

#include <QSqlDatabase>
#include <QVector>

namespace kn {

// revision/anchor 不属于 UI 领域对象，但必须持久保存以检测陈旧写入、保护重复撤销。
struct StoredTask {
    Task task;
    qint64 revision = 0;
    int recurrenceAnchorDay = 0;
};

struct RecurrenceLink {
    qint64 sourceId = 0;
    qint64 nextId = 0;          // 0 = 下一次实例已永久删除（生成凭证仍存在）
    qint64 nextRevision = 0;   // 自动生成当时的 revision，用于安全撤销
};

struct TaskSnapshot {
    QVector<StoredTask> tasks;
    QVector<TaskGroup> groups;
    QVector<RecurrenceLink> recurrenceLinks;
};

struct TaskChanges {
    QVector<StoredTask> saves;       // id=0 插入；否则按 revision 条件更新
    QVector<StoredTask> removals;    // 必须带当前 revision，避免删除已被别处改动的任务
    QVector<TaskGroup> groupSaves;   // id=0 插入，否则重命名
    QVector<qint64> groupRemovals;   // 自动把所有关联任务（包括回收站）移到未分组
    QVector<qint64> removeLinks;     // 安全撤销完成时撤回生成凭证
    qint64 generationSourceId = 0;
    int generatedTaskIndex = -1;     // saves 中新实例的位置；同一来源至多有一个凭证
};

// 所有读写必须与所属 QSqlDatabase 在同一线程。一次 apply 是一个事务，
// 仅提交成功才回填 changes 的新 id/revision 和 after；失败时调用者对象不变。
class TaskRepository
{
public:
    explicit TaskRepository(const QSqlDatabase &database);

    bool load(TaskSnapshot *out, QString *error = nullptr) const;
    bool apply(TaskChanges *changes, TaskSnapshot *after, QString *error = nullptr);
    // 完整校验后的备份合并：组和任务都分配新 id，映射内部引用并保留重复生成凭证。
    bool merge(const TaskSnapshot &incoming, TaskSnapshot *after, QString *error = nullptr);

private:
    bool readSnapshot(TaskSnapshot *out, QString *error) const;
    bool saveTask(StoredTask *task, QString *error);
    bool saveGroup(TaskGroup *group, QString *error);
    bool removeTask(const StoredTask &task, QString *error);
    bool removeGroup(qint64 id, QString *error);
    bool insertLink(const RecurrenceLink &link, QString *error);
    bool ready(QString *error) const;

    QSqlDatabase m_database;
};

} // namespace kn

#endif // KYLINNOTE_TASKREPOSITORY_H
