#pragma once
#include <QColor>
#include <QFont>
#include <QString>
class QApplication;
namespace kn {
namespace AppStyle {

// 颜色令牌：主面板、委托与详情共用同一套值，避免各处硬编码。
enum class Color {
    Text,          // 主文字
    Muted,         // 次要文字
    Faint,         // 占位符/极弱提示
    Accent,        // 品牌强调蓝
    AccentDark,    // 强调色 hover/pressed
    OnAccent,      // 强调底色上的文字
    Selection,     // 列表/导航选中底
    Sidebar,       // 侧栏底色
    SidebarHover,  // 侧栏条目 hover
    Surface,       // 列表/详情白底
    Separator,     // 1px 分隔线
    Hover,         // 任务行 hover
    Field,         // 输入框柔和灰底
    FieldHover,    // 输入框 hover 灰底
    Danger,        // 逾期/危险
    Warning,       // 重要优先级
    Star           // 星标
};
// 布局令牌：像素值集中在此调整。
enum class Metric {
    ContentMargin,   // 内容区左右留白
    SidebarWidth,    // 侧栏固定宽
    ControlHeight,   // 输入控件高
    TaskRowHeight,   // 任务行高
    NavRowHeight,    // 导航行高
    RadiusRow,       // 行选中圆角
    RadiusControl,   // 输入/按钮圆角
    Hairline         // 分隔线宽
};
enum class Font {
    Body,     // 正文
    Small,    // 元信息/计数
    Heading,  // 面板标题
    Brand     // 侧栏应用名
};

const QColor& color(Color role);
int metric(Metric role);
QFont font(Font role);

void apply(QApplication& app);
QString previewCss();
}  // namespace AppStyle
}  // namespace kn
