#pragma once
#include <QStyledItemDelegate>
namespace kl {
// 侧栏导航的线性小图标，全部用 QPainter 绘制，不依赖图标字体或网页资源。
enum class NavIcon { None, Inbox, Today, Upcoming, Overdue, Pin, Done, Trash, Folder, Abandoned, Note, TaskList };
void drawNavIcon(QPainter* painter, NavIcon icon, const QRectF& rect, const QColor& color);

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
    bool editorEvent(QEvent* event,
                     QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override;
};

// 侧栏导航行：分区标题、图标、独立右对齐数量。
class NavigationDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    // 与 TaskPanel 现有 UserRole(view)/+1(group)/+2(label) 并存。
    enum Role { CountRole = Qt::UserRole + 3, SectionRole = Qt::UserRole + 4, IconRole = Qt::UserRole + 5 };
    explicit NavigationDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {
    }
    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
}  // namespace kl
