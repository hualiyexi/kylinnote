#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include "../domain/Task.h"

namespace kn {

// 桌面便签专用的轻量清单视图。它不使用 Fusion 列表/滚动条，避免破坏纸面绘制；
// 完整编辑仍在任务详情中进行。这里只负责阅读和勾选已有项。
class NoteChecklist final : public QWidget {
    Q_OBJECT
public:
    explicit NoteChecklist(QWidget* parent = nullptr);
    void setItems(const QVector<ChecklistItem>& items);
    void setInkColor(const QColor& color);

signals:
    void itemToggled(int index, bool done);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QVector<ChecklistItem> m_items;
    QColor m_ink;
    int m_hoverRow = -1;

    void leaveEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    int rowAt(const QPoint& position) const;
    void updateHeight();
};

}  // namespace kn
