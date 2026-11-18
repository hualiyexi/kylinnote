#include "TaskListView.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QStyleOptionViewItem>

#include "AppStyle.h"
#include "TaskDelegate.h"
#include "TaskListModel.h"

namespace kl {

TaskListView::TaskListView(QWidget* parent) : QListView(parent)
{
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false);
    qRegisterMetaType<TaskDrop::Intent>();
    // 日期区域未按键时也要更新手型指针，viewport 必须开启鼠标跟踪。
    viewport()->setMouseTracking(true);
}

QString TaskListView::mimeType()
{
    return QStringLiteral("application/x-kylinlist-entity-id");
}

void TaskListView::startDrag(Qt::DropActions supportedActions)
{
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    auto* delegate = qobject_cast<TaskDelegate*>(itemDelegate());
    const QModelIndex source = currentIndex();
    // 禁拖兜底：手势层已发 dragBlocked；这里绝不启动 QDrag，也不触发任何提交。
    if (taskModel && source.isValid() && !taskModel->dragBlockReason(source).isEmpty()) {
        taskModel->setDragActive(false);
        clearDropIndicator();
        return;
    }
    QMimeData* mime = taskModel && source.isValid()
        ? taskModel->mimeData(QModelIndexList{source}) : nullptr;
    if (delegate && source.isValid() && mime) {
        const QRect row = visualRect(source);
        const qreal dpr = devicePixelRatioF();
        QPixmap preview(qMax(1, qRound(row.width() * dpr)),
                        qMax(1, qRound(row.height() * dpr)));
        preview.setDevicePixelRatio(dpr);
        preview.fill(Qt::transparent);
        QStyleOptionViewItem option = viewOptions();
        option.index = source;
        option.state.setFlag(QStyle::State_Selected, selectionModel()->isSelected(source));
        option.rect = QRect(QPoint(0, 0), row.size());
        QPainter painter(&preview);
        painter.scale(dpr, dpr);
        delegate->paintDragPreview(&painter, option, source);
        painter.end();

        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(preview);
        QPoint hotSpot = viewport()->mapFromGlobal(QCursor::pos()) - row.topLeft();
        hotSpot.setX(qBound(0, hotSpot.x(), row.width() - 1));
        hotSpot.setY(qBound(0, hotSpot.y(), row.height() - 1));
        drag->setHotSpot(hotSpot);
        drag->exec(supportedActions, Qt::MoveAction);
    } else {
        delete mime;
        QListView::startDrag(supportedActions);
    }
    if (taskModel) taskModel->setDragActive(false);
    clearDropIndicator();
}

void TaskListView::paintEvent(QPaintEvent* event)
{
    QListView::paintEvent(event);
    if (m_dropIndicator.isEmpty()) return;
    QPainter painter(viewport());
    painter.fillRect(m_dropIndicator, AppStyle::color(AppStyle::Color::Accent));
}

void TaskListView::mousePressEvent(QMouseEvent* event)
{
    // 记录手势起点：禁拖反馈每次手势只发一次 dragBlocked。
    m_pressPos = event->pos();
    m_pressIndex = indexAt(event->pos());
    m_blockNotified = false;
    QListView::mousePressEvent(event);
}

void TaskListView::mouseMoveEvent(QMouseEvent* event)
{
    QListView::mouseMoveEvent(event);
    const QModelIndex index = indexAt(event->pos());
    bool clickable = index.isValid() && index.data(TaskListModel::HeaderRole).toBool();
    if (index.isValid() && !clickable) {
        if (auto* delegate = qobject_cast<TaskDelegate*>(itemDelegate())) {
            QStyleOptionViewItem option;
            option.rect = visualRect(index);
            clickable = delegate->dateHitRect(option, index).contains(event->pos());
        }
    }
    viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    // 手势级拖动拦截：按下并越过 startDragDistance 却不允许拖动时，
    // 每次手势仅反馈一次；不 flush、不启动 QDrag。
    if (!m_blockNotified && m_pressIndex.isValid()
        && (event->buttons() & Qt::LeftButton)
        && (event->pos() - m_pressPos).manhattanLength() > QApplication::startDragDistance()) {
        auto* taskModel = qobject_cast<TaskListModel*>(model());
        const QString reason = taskModel ? taskModel->dragBlockReason(m_pressIndex) : QString();
        if (!reason.isEmpty()) {
            m_blockNotified = true;
            emit dragBlocked(reason);
        }
    }
}

void TaskListView::dragEnterEvent(QDragEnterEvent* event)
{
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    const QString sourceId = QString::fromUtf8(event->mimeData()->data(mimeType()));
    const QModelIndex source = taskModel ? taskModel->indexForTask(sourceId) : QModelIndex();
    if (!event->mimeData()->hasFormat(mimeType()) || !source.isValid()
        || !(source.flags() & Qt::ItemIsDragEnabled)
        || !taskModel->dragBlockReason(source).isEmpty()) {
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
    const DropCandidate candidate = dropCandidate(event->mimeData(), event->pos());
    m_dropIndicator = candidate.valid ? candidate.indicator : QRect();
    viewport()->update();
    if (!candidate.valid) {
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
    const DropCandidate candidate = dropCandidate(event->mimeData(), event->pos());
    clearDropIndicator();
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    if (!candidate.valid) {
        if (taskModel) taskModel->setDragActive(false);
        event->ignore();
        return;
    }
    emit entityDropRequested(candidate.intent);
    if (taskModel) taskModel->setDragActive(false);
    event->setDropAction(Qt::MoveAction);
    event->accept();
}

TaskListView::DropCandidate TaskListView::dropCandidate(const QMimeData* mime, const QPoint& position) const
{
    DropCandidate result;
    auto* taskModel = qobject_cast<TaskListModel*>(model());
    if (!taskModel || !mime || !mime->hasFormat(mimeType())) return result;
    const QString sourceId = QString::fromUtf8(mime->data(mimeType()));
    const QModelIndex sourceIndex = taskModel->indexForTask(sourceId);
    if (!sourceIndex.isValid() || !(sourceIndex.flags() & Qt::ItemIsDragEnabled)) return result;
    if (!taskModel->dragBlockReason(sourceIndex).isEmpty()) return result;   // flags 之外的兜底

    const GroupKey sourceGroup = taskModel->groupKeyAt(sourceIndex);
    if (!sourceGroup.isValid()) return result;
    const bool sourceIsTask =
        sourceIndex.data(TaskListModel::KindRole).toInt() == static_cast<int>(EntityKind::Task);

    // 源子树块：自源行起、同组内连续的更深层行。
    QStringList block;
    const int sourceDepth = sourceIndex.data(TaskListModel::DepthRole).toInt();
    for (int row = sourceIndex.row(); row < taskModel->rowCount(); ++row) {
        const QModelIndex index = taskModel->index(row, 0);
        if (index.data(TaskListModel::HeaderRole).toBool()) break;
        if (row > sourceIndex.row() && index.data(TaskListModel::DepthRole).toInt() <= sourceDepth) break;
        block.append(taskModel->entityId(index));
    }
    if (block.isEmpty()) return result;

    const QModelIndex targetIndex = indexAt(position);
    bool header = false;
    GroupKey targetGroup;
    QString anchorId;
    if (targetIndex.isValid()) {
        header = targetIndex.data(TaskListModel::HeaderRole).toBool();
        targetGroup = taskModel->groupKeyAt(targetIndex);
        anchorId = header ? QString() : taskModel->entityId(targetIndex);
        if (block.contains(anchorId)) return result;   // 不能落到自身或自己的子树上
    } else if (taskModel->grouping() == GroupMode::None) {
        targetGroup = GroupKey::flat();   // None 模式空白区 = 无标题组尾部
    } else {
        return result;
    }
    if (!targetGroup.isValid()) return result;
    if (targetGroup.kind == GroupKey::Flat && taskModel->grouping() != GroupMode::None) return result;

    // 跨组目标仅支持 Time/Priority/List/Pinned/None；终态组拒绝（状态切换走命令），
    // Notes 只收既有笔记（同组排序）与置顶笔记取消置顶，任务不做隐式转换。
    if (!(targetGroup == sourceGroup)) {
        if (sourceIsTask) {
            if (!targetGroup.isModeGroup() && targetGroup.kind != GroupKey::Pinned) return result;
        } else {
            const bool toPinned = targetGroup.kind == GroupKey::Pinned;
            const bool unpinNote = sourceGroup.kind == GroupKey::Pinned
                && targetGroup.kind == GroupKey::Notes;
            if (!toPinned && !unpinNote) return result;
        }
    }

    // 目标组现有扁平顺序：可见实体行；折叠组回退段头缓存的完整 id 列表。
    QStringList ordered;
    for (int row = 0; row < taskModel->rowCount(); ++row) {
        const QModelIndex index = taskModel->index(row, 0);
        if (index.data(TaskListModel::HeaderRole).toBool()) continue;
        if (!(taskModel->groupKeyAt(index) == targetGroup)) continue;
        ordered.append(taskModel->entityId(index));
    }
    if (ordered.isEmpty() && header) {
        for (int row = 0; row < taskModel->rowCount(); ++row) {
            const QModelIndex index = taskModel->index(row, 0);
            if (!index.data(TaskListModel::HeaderRole).toBool()) continue;
            if (!(taskModel->groupKeyAt(index) == targetGroup)) continue;
            ordered = index.data(TaskListModel::HeaderEntityIdsRole).toStringList();
            break;
        }
    }
    for (const QString& id : block) ordered.removeAll(id);

    TaskDrop::Placement placement = TaskDrop::Placement::IntoGroup;
    int indicatorLeft = 20;
    QRect indicator;
    const QRect targetRect = targetIndex.isValid() ? visualRect(targetIndex) : QRect();

    if (!targetIndex.isValid()) {
        // None 模式空白区：追加到无标题组尾部。
        ordered.append(block);
        indicator = QRect(20, qBound(0, position.y(), qMax(0, viewport()->height() - 2)),
                          qMax(0, viewport()->width() - 32), 2);
    } else if (header) {
        ordered.append(block);
        indicator = QRect(20, targetRect.bottom() - 1,
                          qMax(0, viewport()->width() - 32), 2);
    } else {
        const bool after = position.y() > targetRect.center().y();
        // 右下 24px 设父：目标行右下角 24×24 区域。
        const bool childZone = position.x() >= targetRect.right() - 24
            && position.y() >= targetRect.bottom() - 24;
        const bool targetIsTask =
            targetIndex.data(TaskListModel::KindRole).toInt() == static_cast<int>(EntityKind::Task);
        bool makeChild = false;
        if (childZone && sourceIsTask && targetIsTask
            && (targetIndex.flags() & Qt::ItemIsDragEnabled)) {
            // 自身或子孙不能成为父项：沿 anchor 祖先链检查。
            bool cycle = false;
            QString walk = anchorId;
            QSet<QString> visited;
            while (!walk.isEmpty() && !visited.contains(walk)) {
                if (walk == sourceId) { cycle = true; break; }
                visited.insert(walk);
                const QModelIndex walkIndex = taskModel->indexForTask(walk);
                walk = walkIndex.isValid()
                    ? walkIndex.data(TaskListModel::ParentIdRole).toString() : QString();
            }
            makeChild = !cycle;
        }
        const int insertAt = ordered.indexOf(anchorId);
        if (insertAt < 0 && !makeChild) return result;   // 折叠子行等异常：拒绝
        const int targetDepth = targetIndex.data(TaskListModel::DepthRole).toInt();
        int at = makeChild ? (insertAt >= 0 ? insertAt + 1 : ordered.size())
                           : insertAt + (after ? 1 : 0);
        if (makeChild) {
            placement = TaskDrop::Placement::AsChild;
            result.intent.targetParentId = anchorId;
            result.intent.anchorId = anchorId;
            indicatorLeft = 20 + (targetDepth + 1) * 24;
        } else {
            placement = after ? TaskDrop::Placement::After : TaskDrop::Placement::Before;
            result.intent.anchorId = anchorId;
        }
        for (const QString& id : block) { ordered.insert(at, id); ++at; }
        indicator = QRect(indicatorLeft, after ? targetRect.bottom() - 1 : targetRect.top(),
                          qMax(0, viewport()->width() - indicatorLeft - 12), 2);
    }
    if (ordered.isEmpty()) return result;

    result.intent.sourceId = sourceId;
    result.intent.mode = taskModel->grouping();
    result.intent.sourceGroup = sourceGroup;
    result.intent.targetGroup = targetGroup;
    result.intent.placement = placement;
    result.intent.orderedIds = ordered;
    result.intent.targetDate = TaskDrop::dateForGroup(targetGroup, toAppTime(appNow()).date());
    if (!result.intent.isValid()) return result;
    result.indicator = indicator;
    result.valid = true;
    return result;
}

void TaskListView::clearDropIndicator()
{
    m_dropIndicator = QRect();
    viewport()->update();
}

} // namespace kl
