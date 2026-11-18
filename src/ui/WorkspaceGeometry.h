#pragma once

namespace kl {
namespace WorkspaceGeometry {

// 滴答客户端实测：第二至第四列共享 28px 顶部窗口控制条，第二、三列不画背景。
// 坐标均相对于各列可见顶边；三列内容从控制条下方排布，首行共用中心线。
enum : int {
    WindowControlHeight = 28,
    DetailHeaderHeight = 48,
    DetailHeaderTopMargin = WindowControlHeight - 4,
    // Qt 5.12 的偶数矩形整数中心偏上；上 7 / 下 5 让可见按钮落在共享中心线。
    DetailHeaderContentTopMargin = 7,
    DetailHeaderContentBottomMargin = 5,
    FirstRowIconCenter = DetailHeaderTopMargin + DetailHeaderHeight / 2,
    RailAccountSize = 34,
    RailTopMargin = FirstRowIconCenter - RailAccountSize / 2,
    RailButtonSpacing = 10,
    RailAfterAccountExtraSpacing = 6,
    NavigationRowHeight = 35,
    NavigationTopMargin = FirstRowIconCenter - NavigationRowHeight / 2,
    ListHeaderHeight = 40,
    ListHeaderTopMargin = FirstRowIconCenter - ListHeaderHeight / 2,
    ListHeadingIconSize = 24,
    SidebarToggleSize = 32,
    SidebarToggleIconSize = 20
};

}  // namespace WorkspaceGeometry
}  // namespace kl
