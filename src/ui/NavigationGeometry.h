#pragma once

namespace kl {
namespace NavigationGeometry {

// 第二列共用同一份行几何：顶层图标对齐“清单”标题原有的 14px 左边距，
// 文件夹箭头独立占位；子清单图标盒再对齐文件夹文字。
enum : int {
    RowHorizontalInset = 10,
    SectionHorizontalInset = 6,
    SectionTitleInset = 8,
    TitleLeft = SectionHorizontalInset + SectionTitleInset,
    ListIconOffset = TitleLeft - RowHorizontalInset,
    FolderIconOffset = ListIconOffset,
    FolderDisclosureWidth = 16,
    IconBoxSize = 20,
    IconLabelGap = 6,
    FolderChildIndent = FolderIconOffset + IconBoxSize + IconLabelGap - ListIconOffset,
    ChildIconGlyphSize = 16,
    ChildIconGlyphInset = (IconBoxSize - ChildIconGlyphSize) / 2,
    ChildIconGlyphVerticalOffset = 1
};

}  // namespace NavigationGeometry
}  // namespace kl
