#ifndef KYLINLIST_TASKLISTMODEL_H
#define KYLINLIST_TASKLISTMODEL_H

#include <QAbstractListModel>
#include "../domain/ViewSpec.h"

namespace kl {
class TaskStore;
class TaskListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,
        MetaRole,
        PriorityRole,
        CompletedRole,
        DeletedRole,
        OverdueRole,
        PinnedRole,
        HeaderRole // 仅模型/委托内部使用的段头行
    };
    explicit TaskListModel(TaskStore* store, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    void setFilter(const ViewSpec& filter);
    ViewSpec filter() const { return m_filter; }
    QModelIndex indexForTask(QString id) const;
    QString entityId(const QModelIndex& index) const;
    int entityCount() const { return m_entityCount; }
public slots:
    void refresh();

private:
    struct Row {
        Entity entity;
        QString header;
        bool isHeader = false;
    };
    TaskStore* m_store;
    ViewSpec m_filter;
    QVector<Row> m_rows;
    int m_entityCount = 0;
};
}  // namespace kl

#endif // KYLINLIST_TASKLISTMODEL_H
