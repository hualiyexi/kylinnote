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
        SectionKeyRole,     // GroupKey::stableId()（委托与旧调用兼容）
        GroupKeyRole,       // QVariant 携带 kl::GroupKey（Q_DECLARE_METATYPE）
        HeaderActionRole,
        HeaderEntityIdsRole,
        ParentIdRole,
        DepthRole,
        HasChildrenRole,
        ChildrenCollapsedRole,
        SectionCollapsedRole,
        DetailRole,          // 单行纯文本正文摘要。
        DetailsVisibleRole,
        ReminderVisibleRole,
        AttachmentVisibleRole
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
    GroupMode grouping() const { return m_filter.grouping; }
    QModelIndex indexForTask(QString id) const;
    QString entityId(const QModelIndex& index) const;
    int entityCount() const { return m_entityCount; }
    void toggleChildren(const QString& id);
    void expandChildren(const QString& id);
    void toggleSection(const QString& key);
    void setDragActive(bool active, const QString& sourceId = QString());
    // 该行所在显示组；无效索引返回 Invalid 组。
    GroupKey groupKeyAt(const QModelIndex& index) const;
    // 拖动拦截原因：空 = 允许拖动。Created 分组禁拖、终态/回收站实体禁拖。
    // flags()/mimeData() 用同一规则兜底，视图手势层用它发 dragBlocked(QString)。
    QString dragBlockReason(const QModelIndex& index) const;
public slots:
    void refresh();

private:
    struct Row {
        Entity entity;
        QString header;
        GroupKey group;
        QStringList sectionEntityIds;
        int depth = 0;
        bool hasChildren = false;
        bool isHeader = false;
        bool hasAttachments = false;
    };
    void rebuildRows(const Grouping& grouping, const QDate& today);
    void appendDragTargetHeaders(const QDate& today);

    TaskStore* m_store;
    ViewSpec m_filter;
    QVector<Row> m_rows;
    QSet<QString> m_existingGroups;   // 本次刷新已出现的组（stableId），空组投放区去重用
    QSet<QString> m_collapsedTasks;
    QSet<QString> m_collapsedSections;
    QHash<QString, TaskList> m_listsById;
    QSet<QString> m_attachmentEntityIds;
    QString m_dragSourceId;
    bool m_dragActive = false;
    int m_entityCount = 0;
};
}  // namespace kl

#endif // KYLINLIST_TASKLISTMODEL_H
