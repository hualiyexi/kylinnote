#pragma once
#include <QAbstractListModel>
#include "TaskFilter.h"
namespace kn {
class TaskStore;
class TaskListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        MetaRole,
        PriorityRole,
        CompletedRole,
        FavoriteRole,
        PinnedRole,
        DeletedRole,
        OverdueRole
    };
    explicit TaskListModel(TaskStore* store, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    void setFilter(const TaskFilter& filter);
    TaskFilter filter() const {
        return m_filter;
    }
    QModelIndex indexForTask(qint64 id) const;
    qint64 taskId(const QModelIndex& index) const;
public slots:
    void refresh();

private:
    TaskStore* m_store;
    TaskFilter m_filter;
    QVector<Task> m_tasks;
};
}  // namespace kn
