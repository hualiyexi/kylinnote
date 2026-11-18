#pragma once

#include <QListView>
#include "../domain/TaskDrop.h"
#include <QStringList>

namespace kl {

// 任务列表只产生拖动意图（TaskDrop::Intent）；顺序、置顶、日期、优先级、跨清单
// 迁移与脱父等业务全部由 TaskStore 原子提交。
class TaskListView : public QListView {
    Q_OBJECT
public:
    explicit TaskListView(QWidget* parent = nullptr);

    static QString mimeType();

signals:
    void entityDropRequested(const kl::TaskDrop::Intent& intent);
    // 禁拖手势反馈：按下行越过 startDragDistance 却不允许拖动时，
    // 每次手势仅发射一次（Created 分组、终态/回收站实体等）。
    void dragBlocked(const QString& reason);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct DropCandidate {
        kl::TaskDrop::Intent intent;
        QRect indicator;
        bool valid = false;
    };

    DropCandidate dropCandidate(const QMimeData* mime, const QPoint& position) const;
    void clearDropIndicator();

    QRect m_dropIndicator;
    QModelIndex m_pressIndex;   // 当前按下手势的起点行（禁拖反馈用）
    QPoint m_pressPos;
    bool m_blockNotified = false;   // 本次手势是否已发过 dragBlocked
};

} // namespace kl
