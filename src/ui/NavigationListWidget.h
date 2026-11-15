#pragma once

#include <QListWidget>

namespace kl {

// 侧栏只解析拖放意图，不在视图内直接移动 item；持久化由 TaskPanel/TaskStore 完成。
class NavigationListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit NavigationListWidget(QWidget* parent = nullptr);

    static QString mimeType();

signals:
    void listReorderRequested(QString sourceId, QString targetFolderId,
                              QString targetListId, bool after);
    void folderMergeRequested(QString sourceId, QString targetListId);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct DropTarget {
        QString sourceId;
        QString targetFolderId;
        QString targetListId;
        int indicatorRow = -1;
        bool topLevelEnd = false;
        bool after = false;
        bool merge = false;
        bool valid = false;
    };

    DropTarget dropTarget(const QPoint& position) const;
    void clearDropTarget();

    int m_indicatorRow = -1;
};

} // namespace kl
