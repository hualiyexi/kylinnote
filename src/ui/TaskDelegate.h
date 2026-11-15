#pragma once
#include <QStyledItemDelegate>
#include <QStringList>
#include "AppIcons.h"
namespace kl {

// 保留旧枚举名作为兼容别名，新代码用 IconId。
using NavIcon = IconId;
inline void drawNavIcon(QPainter* painter, IconId icon, const QRectF& rect, const QColor& color) {
    AppIcons::paint(painter, icon, rect, color);
}

// 任务行：圆角 checkbox + 标题 + 元信息，选中为柔和蓝。
class TaskDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit TaskDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool editorEvent(QEvent* event,
                     QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
    // 行内日期点击区域；与 editorEvent 共用同一布局，供视图设置手型指针。
    QRect dateHitRect(const QStyleOptionViewItem& option, const QModelIndex& index) const;
signals:
    void dateActivated(QString entityId, QRect anchor);
    void listActivated(QString listId);
    void postponeActivated(QStringList entityIds);
    void childrenToggleActivated(QString entityId);
    void sectionToggleActivated(QString sectionKey);
};

// 侧栏导航行：分区标题、SVG 图标、独立右对齐数量。
class NavigationDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // 与 TaskPanel 现有 UserRole(view)/+1(group)/+2(label) 并存。
    enum Role {
        CountRole = Qt::UserRole + 3,
        SectionRole = Qt::UserRole + 4,
        IconRole = Qt::UserRole + 5,
        FolderIdRole = Qt::UserRole + 6,
        IconNameRole = Qt::UserRole + 7,
        ListColorRole = Qt::UserRole + 8,
        IndentRole = Qt::UserRole + 9,
        ListKindRole = Qt::UserRole + 10,
        DividerRole = Qt::UserRole + 11,
        ParentFolderRole = Qt::UserRole + 12,
        DropTargetRole = Qt::UserRole + 13,
        DropLineRole = Qt::UserRole + 14,
        FolderExpandedRole = Qt::UserRole + 15,
        OrganizationEndRole = Qt::UserRole + 16
    };
    explicit NavigationDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view,
                   const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;
signals:
    void folderToggleActivated(QString folderId);
};
}  // namespace kl
