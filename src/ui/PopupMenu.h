#pragma once

#include <QColor>
#include <QImage>
#include <QMenu>
#include "design/DesignTokens.h"

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
    void setCardPadding(int horizontal, int vertical);

    // 按实例主题覆盖，默认关闭：开启后卡片、边框、阴影与动作文字/图标色全部
    // 改从指定主题取值；未开启时仍走全局 ThemeManager，行为与原先完全一致。
    // 经 addMenu(QString)/addMenu(QIcon,QString) 创建的同类子菜单继承该覆盖。
    void setThemeOverride(ds::Theme theme);
    void clearThemeOverride();
    bool hasThemeOverride() const;

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void rebuildShadow(const QColor& color);
    void paintActions();
    QRectF cardRect() const;
    QColor tokenColor(ds::ColorToken token) const;
    QImage m_shadow;
    QColor m_shadowColor;
    bool m_themeOverrideActive = false;
    ds::Theme m_themeOverride = ds::Theme::Light;
};

}  // namespace kl
