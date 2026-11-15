#pragma once

namespace kl {
namespace NavigationGeometry {

// 顶层图标对齐“清单”标题的 14px 左边距；箭头在更靠左的独立区域。
enum : int {
    RowHorizontalInset = 10,
    TitleLeft = 14,
    ListIconOffset = TitleLeft - RowHorizontalInset,
    FolderDisclosureWidth = 16,
    DisclosureLeft = 0,
    DisclosureGlyphSize = 10,
    IconBoxSize = 20,
    IconLabelGap = 6,
    IconGlyphVerticalOffset = 1,
    FolderChildIndent = 22,
    ChildIconGlyphSize = 16,
    ChildIconGlyphInset = (IconBoxSize - ChildIconGlyphSize) / 2,
    ChildIconGlyphVerticalOffset = 1
};

}  // namespace NavigationGeometry
}  // namespace kl
