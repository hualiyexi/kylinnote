#ifndef KYLINLIST_TASKLISTMODEL_H
#define KYLINLIST_TASKLISTMODEL_H

#include <QAbstractListModel>
#include "../domain/ViewSpec.h"

class QMimeData;

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
        HeaderRole, // 仅模型/委托内部使用的段头行
        ListIdRole,
        ListNameRole,
        ListIconRole,
        ListColorRole,
        ScheduleRole,
        ScheduleTooltipRole,
        SectionKeyRole,
        HeaderActionRole,
        HeaderEntityIdsRole,
        ParentIdRole,
        DepthRole,
        HasChildrenRole,
        ChildrenCollapsedRole,
        UnpinnedSectionRole,
        SectionCollapsedRole,
        DetailRole,          // 单行纯文本摘要：正文与检查项进度。
        DetailsVisibleRole
    };
    explicit TaskListModel(TaskStore* store, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    Qt::DropActions supportedDragActions() const override;
    Qt::DropActions supportedDropActions() const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    void setFilter(const ViewSpec& filter);
    ViewSpec filter() const { return m_filter; }
    QModelIndex indexForTask(QString id) const;
    QString entityId(const QModelIndex& index) const;
    int entityCount() const { return m_entityCount; }
    void toggleChildren(const QString& id);
    void expandChildren(const QString& id);
    void toggleSection(const QString& key);
    void setDragActive(bool active, const QString& sourceId = QString());
public slots:
    void refresh();

private:
    struct Row {
        Entity entity;
        QString header;
        QString sectionKey;
        QStringList sectionEntityIds;
        int depth = 0;
        bool hasChildren = false;
        bool isHeader = false;
    };
    QString unpinnedSection(const Entity& entity) const;

    TaskStore* m_store;
    ViewSpec m_filter;
    QVector<Row> m_rows;
    QSet<QString> m_collapsedTasks;
    QSet<QString> m_collapsedSections;
    QString m_dragSourceId;
    bool m_dragActive = false;
    int m_entityCount = 0;
};
}  // namespace kl

#endif // KYLINLIST_TASKLISTMODEL_H
