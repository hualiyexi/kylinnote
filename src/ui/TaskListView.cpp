#include "TaskListView.h"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>

#include "AppStyle.h"
#include "TaskListModel.h"

namespace kl {

TaskListView::TaskListView(QWidget* parent) : QListView(parent)
{
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false);
}

QString TaskListView::mimeType()
{
    return QStringLiteral("application/x-kylinlist-entity-id");
}

void TaskListView::startDrag(Qt::DropActions supportedActions)
{
    QListView::startDrag(supportedActions);
    if (auto* taskModel = qobject_cast<TaskListModel*>(model())) taskModel->setDragActive(false);
    clearDropIndicator();
}

void TaskListView::paintEvent(QPaintEvent* event)
{
    QListView::paintEvent(event);
    if (m_dropIndicator.isEmpty()) return;
    QPainter painter(viewport());
    painter.fillRect(m_dropIndicator, AppStyle::color(AppStyle::Color::Accent));
}

void TaskListView::mouseMoveEvent(QMouseEvent* event)
{
    QListView::mouseMoveEvent(event);
    const QModelIndex index = indexAt(event->pos());
    const bool header = index.isValid()
        && index.data(TaskListModel::HeaderRole).toBool();
    viewport()->setCursor(header ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void TaskListView::dragEnterEvent(QDragEnterEvent* event)
{
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    const QString sourceId = QString::fromUtf8(event->mimeData()->data(mimeType()));
    const QModelIndex source = taskModel ? taskModel->indexForTask(sourceId) : QModelIndex();
    if (!event->mimeData()->hasFormat(mimeType()) || !source.isValid()
        || !(source.flags() & Qt::ItemIsDragEnabled)) {
        event->ignore();
        return;
    }
    QListView::dragEnterEvent(event);
    taskModel->setDragActive(true, sourceId);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TaskListView::dragMoveEvent(QDragMoveEvent* event)
{
    QListView::dragMoveEvent(event); // 保留原生视图的边缘自动滚动。
    const DropTarget target = dropTarget(event->mimeData(), event->pos());
    m_dropIndicator = target.valid ? target.indicator : QRect();
    viewport()->update();
    if (!target.valid) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

void TaskListView::dragLeaveEvent(QDragLeaveEvent* event)
{
    clearDropIndicator();
    if (auto* taskModel = qobject_cast<TaskListModel*>(model())) taskModel->setDragActive(false);
    QListView::dragLeaveEvent(event);
}

void TaskListView::dropEvent(QDropEvent* event)
{
    const DropTarget target = dropTarget(event->mimeData(), event->pos());
    clearDropIndicator();
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    if (!target.valid) {
        if (taskModel) taskModel->setDragActive(false);
        event->ignore();
        return;
    }
    emit entityDropRequested(target.sourceId, target.orderedIds, target.targetSection,
                             target.pinned, target.detachParent);
    if (taskModel) taskModel->setDragActive(false);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

TaskListView::DropTarget TaskListView::dropTarget(const QMimeData* mime, const QPoint& position) const
{
    DropTarget result;
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    if (!taskModel || !mime || !mime->hasFormat(mimeType())) return result;
    result.sourceId = QString::fromUtf8(mime->data(mimeType()));
    const QModelIndex sourceIndex = taskModel->indexForTask(result.sourceId);
    if (!sourceIndex.isValid() || !(sourceIndex.flags() & Qt::ItemIsDragEnabled)) return result;
    const QString sourceSection = sourceIndex.data(TaskListModel::SectionKeyRole).toString();
    const QString sourceParent = sourceIndex.data(TaskListModel::ParentIdRole).toString();
    const int sourceDepth = sourceIndex.data(TaskListModel::DepthRole).toInt();

    QStringList block;
    for (int row = sourceIndex.row(); row < taskModel->rowCount(); ++row) {
        const QModelIndex index = taskModel->index(row, 0);
        if (index.data(TaskListModel::HeaderRole).toBool()) break;
        if (row > sourceIndex.row() && index.data(TaskListModel::DepthRole).toInt() <= sourceDepth) break;
        block.append(taskModel->entityId(index));
    }
    if (block.isEmpty()) return result;

    const QModelIndex targetIndex = indexAt(position);
    if (!targetIndex.isValid()) return result;
    const bool header = targetIndex.data(TaskListModel::HeaderRole).toBool();
    const QString targetId = taskModel->entityId(targetIndex);
    if (block.contains(targetId)) return result;
    result.targetSection = targetIndex.data(TaskListModel::SectionKeyRole).toString();
    if (result.targetSection.isEmpty()) return result;
    result.pinned = result.targetSection == QLatin1String("pinned");
    const QString targetParent = header ? QString()
        : targetIndex.data(TaskListModel::ParentIdRole).toString();
    result.detachParent = !sourceParent.isEmpty()
        && (sourceSection != result.targetSection || sourceParent != targetParent);

    if (result.targetSection.startsWith(QLatin1String("list:"))
        && result.targetSection.mid(5) != sourceIndex.data(TaskListModel::ListIdRole).toString())
        return result;
    const bool timeView = taskModel->filter().renderer == ViewRenderer::Time;
    const bool sectionChanged = sourceSection != result.targetSection;
    if (sectionChanged) {
        if (timeView) {
            const bool targetTime = result.targetSection == QLatin1String("overdue")
                || result.targetSection == QLatin1String("today")
                || result.targetSection == QLatin1String("tomorrow")
                || result.targetSection == QLatin1String("next7days");
            if (!result.pinned && !targetTime) return result;
        } else if (!result.pinned) {
            // 普通分区只接收从置顶拖回原日期/清单的任务，不隐式改期或跨清单移动。
            if (sourceSection != QLatin1String("pinned")
                || result.targetSection != sourceIndex.data(TaskListModel::UnpinnedSectionRole).toString())
                return result;
        }
    }

    QStringList ordered;
    for (int row = 0; row < taskModel->rowCount(); ++row) {
        const QModelIndex index = taskModel->index(row, 0);
        if (index.data(TaskListModel::HeaderRole).toBool()
            || index.data(TaskListModel::SectionKeyRole).toString() != result.targetSection)
            continue;
        const QString id = taskModel->entityId(index);
        if (!block.contains(id)) ordered.append(id);
    }
    const QRect targetRect = visualRect(targetIndex);
    const bool after = header || position.y() > targetRect.center().y();
    if (header) {
        ordered.append(block);
    } else {
        int insertAt = ordered.indexOf(targetId);
        if (insertAt < 0) return result;
        if (after) ++insertAt;
        for (const QString& id : block) ordered.insert(insertAt++, id);
    }
    if (ordered.isEmpty()) return result;
    result.orderedIds = ordered;
    result.indicator = QRect(20, after ? targetRect.bottom() - 1 : targetRect.top(),
                             qMax(0, viewport()->width() - 32), 2);
    result.valid = true;
    return result;
}

void TaskListView::clearDropIndicator()
{
    m_dropIndicator = QRect();
    viewport()->update();
}

} // namespace kl
