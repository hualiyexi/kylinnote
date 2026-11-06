#pragma once
#include <QStyledItemDelegate>
namespace kn {
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
}  // namespace kn
