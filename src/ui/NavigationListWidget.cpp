#include "NavigationListWidget.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>

#include "TaskDelegate.h"

namespace kl {

NavigationListWidget::NavigationListWidget(QWidget* parent) : QListWidget(parent)
{
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false);
}

QString NavigationListWidget::mimeType()
{
    return QStringLiteral("application/x-kylinlist-list-id");
}

void NavigationListWidget::startDrag(Qt::DropActions supportedActions)
{
    QListWidgetItem* current = currentItem();
    if (!current || !(current->flags() & Qt::ItemIsDragEnabled)) return;
    const QString sourceId = current->data(Qt::UserRole + 1).toString();
    if (sourceId.isEmpty()) return;
    auto* mime = new QMimeData;
    mime->setData(mimeType(), sourceId.toUtf8());
    mime->setText(current->text());
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(supportedActions, Qt::MoveAction);
    clearDropTarget();
}

NavigationListWidget::DropTarget NavigationListWidget::dropTarget(const QPoint& position) const
{
    DropTarget result;
    QListWidgetItem* target = itemAt(position);
    if (!target) {
        // 视口空白也可移到顶层末尾；以组织区末项（或其后的分隔线）显示落点。
        for (int row = 0; row < count(); ++row) {
            const QListWidgetItem* candidate = item(row);
            if (candidate->isHidden()) continue;
            if (candidate->data(NavigationDelegate::OrganizationEndRole).toBool()
                || !candidate->data(NavigationDelegate::FolderIdRole).toString().isEmpty()
                || (!candidate->data(Qt::UserRole + 1).toString().isEmpty()
                    && (candidate->flags() & Qt::ItemIsDragEnabled)))
                result.indicatorRow = row;
        }
        if (result.indicatorRow < 0
            || position.y() < visualItemRect(item(result.indicatorRow)).bottom()) return result;
        result.topLevelEnd = true;
        result.after = true;
        result.valid = true;
        return result;
    }
    result.indicatorRow = row(target);
    if (target->data(NavigationDelegate::OrganizationEndRole).toBool()) {
        result.topLevelEnd = true;
        result.after = true;
        result.valid = true;
        return result;
    }
    const QString targetFolderRow = target->data(NavigationDelegate::FolderIdRole).toString();
    const QString targetListId = target->data(Qt::UserRole + 1).toString();
    if (targetFolderRow.isEmpty()
        && (targetListId.isEmpty() || !(target->flags() & Qt::ItemIsDragEnabled))) return result;

    const QRect rect = visualItemRect(target);
    result.targetListId = targetListId;
    result.targetFolderId = targetFolderRow.isEmpty()
        ? target->data(NavigationDelegate::ParentFolderRole).toString() : targetFolderRow;
    const int center = rect.center().y();
    const bool centerZone = !targetFolderRow.isEmpty()
        || (position.y() >= center - 6 && position.y() <= center + 6);
    result.after = position.y() > center + 6;
    // 平铺清单中心合并；文件夹内中心仅表示移到该清单之前，不创建嵌套文件夹。
    result.merge = targetFolderRow.isEmpty() && result.targetFolderId.isEmpty() && centerZone;
    if (!centerZone) result.after = position.y() > center;
    result.valid = true;
    return result;
}

void NavigationListWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasFormat(mimeType())) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void NavigationListWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event->mimeData()->hasFormat(mimeType())) {
        clearDropTarget();
        event->ignore();
        return;
    }
    DropTarget target = dropTarget(event->pos());
    target.sourceId = QString::fromUtf8(event->mimeData()->data(mimeType()));
    if (target.sourceId.isEmpty() || target.sourceId == target.targetListId) target.valid = false;
    clearDropTarget();
    if (!target.valid) {
        event->ignore();
        return;
    }
    QListWidgetItem* item = this->item(target.indicatorRow);
    if (!item) {
        event->ignore();
        return;
    }
    m_indicatorRow = target.indicatorRow;
    if (target.topLevelEnd)
        item->setData(NavigationDelegate::DropLineRole, QStringLiteral("after"));
    else if (target.merge || target.targetListId.isEmpty()
        || (event->pos().y() >= visualItemRect(item).center().y() - 6
            && event->pos().y() <= visualItemRect(item).center().y() + 6))
        item->setData(NavigationDelegate::DropTargetRole, QStringLiteral("target"));
    else
        item->setData(NavigationDelegate::DropLineRole,
                      target.after ? QStringLiteral("after") : QStringLiteral("before"));
    viewport()->update();
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void NavigationListWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearDropTarget();
    QListWidget::dragLeaveEvent(event);
}

void NavigationListWidget::dropEvent(QDropEvent* event)
{
    DropTarget target = dropTarget(event->pos());
    target.sourceId = QString::fromUtf8(event->mimeData()->data(mimeType()));
    clearDropTarget();
    if (!event->mimeData()->hasFormat(mimeType()) || !target.valid
        || target.sourceId.isEmpty() || target.sourceId == target.targetListId) {
        event->ignore();
        return;
    }
    if (target.merge) {
        emit folderMergeRequested(target.sourceId, target.targetListId);
    } else {
        emit listReorderRequested(target.sourceId, target.targetFolderId,
                                  target.targetListId, target.after);
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void NavigationListWidget::clearDropTarget()
{
    if (m_indicatorRow >= 0 && m_indicatorRow < count()) {
        QListWidgetItem* item = this->item(m_indicatorRow);
        item->setData(NavigationDelegate::DropTargetRole, QVariant());
        item->setData(NavigationDelegate::DropLineRole, QVariant());
    }
    m_indicatorRow = -1;
    viewport()->update();
}

} // namespace kl
