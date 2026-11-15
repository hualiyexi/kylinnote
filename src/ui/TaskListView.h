#pragma once

#include <QListView>
#include <QStringList>

namespace kl {

// 任务列表只产生拖动意图；顺序、置顶与日期由 TaskStore 原子提交。
class TaskListView : public QListView {
    Q_OBJECT
public:
    explicit TaskListView(QWidget* parent = nullptr);

    static QString mimeType();

signals:
    void entityDropRequested(QString sourceId, QStringList orderedIds,
                             QString targetSection, bool pinned, bool detachParent,
                             QString targetParentId);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct DropTarget {
        QString sourceId;
        QStringList orderedIds;
        QString targetSection;
        QString targetParentId;
        QRect indicator;
        bool pinned = false;
        bool detachParent = false;
        bool valid = false;
    };

    DropTarget dropTarget(const QMimeData* mime, const QPoint& position) const;
    void clearDropIndicator();

    QRect m_dropIndicator;
};

} // namespace kl
