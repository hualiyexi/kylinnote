#pragma once

namespace kl {
namespace WorkspaceGeometry {

// 滴答网页实测：头像 32px / 顶边距 16px；导航首行与列表折叠图标共用中心线。
// 坐标均相对于各列可见顶边，第四列详情栏仍沿用原有 36px 顶边距。
enum : int {
    RailTopMargin = 16,
    RailAccountSize = 32,
    RailButtonSpacing = 10,
    RailAfterAccountExtraSpacing = 6,
    FirstRowIconCenter = 39,
    NavigationRowHeight = 36,
    NavigationTopMargin = FirstRowIconCenter - NavigationRowHeight / 2,
    ListHeaderHeight = 40,
    ListHeaderTopMargin = FirstRowIconCenter - ListHeaderHeight / 2,
    SidebarToggleSize = 32,
    SidebarToggleIconSize = 20
};

}  // namespace WorkspaceGeometry
}  // namespace kl
