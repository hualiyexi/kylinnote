#pragma once
// AppStyle 现在是设计系统的兼容门面。
//
// 真正的令牌定义在 design/DesignTokens.h（ds::raw / ds::semantic），
// 主题切换在 design/ThemeManager.h，字体在 design/TypeSystem.h，QSS 在 design/QssBuilder.h。
// 本文件保留旧的 Color/Metric/Font 枚举与 color()/metric()/font()/apply()/previewCss()
// 签名，把调用转发到新系统，使既有业务代码零改动即可获得主题切换能力。
// 新代码应直接使用 ds:: / ThemeManager / AppIcons。
#include <QColor>
#include <QFont>
#include <QString>
class QApplication;
namespace kl {
namespace AppStyle {

// 颜色令牌（旧枚举，映射到 ds::ColorToken）。
enum class Color {
    Text, Muted, Faint, Accent, AccentDark, OnAccent,
    Selection, Sidebar, SidebarHover, Surface, Separator,
    Hover, Field, FieldHover, Danger, Warning, Star
};
// 布局令牌：像素值集中在此调整。
enum class Metric {
    ContentMargin, SidebarWidth, ControlHeight, TaskRowHeight,
    NavRowHeight, RadiusRow, RadiusControl, Hairline
};
enum class Font { Body, Small, Heading, Brand };

QColor color(Color role);
int metric(Metric role);
QFont font(Font role);

void apply(QApplication& app);
QString previewCss();
}  // namespace AppStyle
}  // namespace kl
