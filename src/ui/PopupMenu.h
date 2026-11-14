#pragma once

#include <QImage>
#include <QMenu>

namespace kl {

// 保留 QMenu 的动作和键盘行为；透明边距内绘制缓存的高斯阴影，卡片与动作不裁阴影。
class PopupMenu : public QMenu {
    Q_OBJECT
public:
    explicit PopupMenu(QWidget* parent = nullptr);
    explicit PopupMenu(const QString& title, QWidget* parent = nullptr);

    // 文本重载创建同类子菜单；指针重载保留普通 QMenu 的接入能力。
    PopupMenu* addMenu(const QString& title);
    PopupMenu* addMenu(const QIcon& icon, const QString& title);
    QAction* addMenu(QMenu* menu);
    static int shadowMargin();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void rebuildShadow(const QColor& color);
    QRectF cardRect() const;
    QImage m_shadow;
    QColor m_shadowColor;
};

}  // namespace kl
