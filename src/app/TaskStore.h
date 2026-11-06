#ifndef KYLINNOTE_TASKSTORE_H
#define KYLINNOTE_TASKSTORE_H

#include "../db/TaskRepository.h"

#include <QObject>
#include <QSqlDatabase>

namespace kn {

class TaskStore : public QObject
{
    Q_OBJECT
public:
    explicit TaskStore(const QSqlDatabase &database, QObject *parent = nullptr);
    bool load(QString *error = nullptr);
    QVector<Task> tasks() const;             // 包括回收站任务，由 UI 决定筛选
    QVector<TaskGroup> groups() const;
    Task task(qint64 id) const;               // 未找到返回 id=0 的 Task
    qint64 createTask(const QString &title, qint64 groupId = 0);
    bool saveTask(Task task);
    bool setCompleted(qint64 id, bool completed);
    bool setPinned(qint64 id, bool pinned);
    bool trashTask(qint64 id);
    bool restoreTask(qint64 id);
    bool deletePermanently(qint64 id);
    qint64 createGroup(const QString &name);
    bool renameGroup(qint64 id, const QString &name);
    bool deleteGroup(qint64 id);
    bool exportBackup(const QString &path);
    bool importBackup(const QString &path);

    QString lastError() const { return m_lastError; }
    // 调度器的持久 claim：只在当前 reminder 仍匹配且尚未通知时保存标记。
    // 返回 true 才能发 reminderDue。此函数不负责 UI。
    bool markReminderNotified(qint64 id, const QDateTime &expectedReminder, const QDateTime &now);

signals:
    void taskChanged(qint64 id);
    void tasksReset();
    void groupsChanged();
    void errorOccurred(const QString &message);

private:
    const StoredTask *find(qint64 id) const;
    const RecurrenceLink *linkFor(qint64 sourceId) const;
    bool hasGroup(qint64 id) const;
    bool fail(const QString &message, QString *error = nullptr);
    bool ensureLoaded();
    bool commit(TaskChanges *changes, bool reset = false);
    bool prepareSave(Task task, TaskChanges *changes);

    TaskRepository m_repository;
    TaskSnapshot m_snapshot;
    QString m_lastError;
    bool m_loaded = false;
};

} // namespace kn

#endif // KYLINNOTE_TASKSTORE_H
