#pragma once
// 设计令牌单一真源。
//
// 分层（借鉴 CaseLawLibrary / public-interest-litigation-fund 的 tokens→theme→base 三层）：
//   ds::raw       原料层：裸色值 QRgb，唯一允许出现字面量的地方，业务禁引用。
//   ds::semantic  语义层：ColorToken 枚举 + 每主题值表，业务唯一入口。
//   ds::component 组件层：由语义色派生的组合（如标签三件套）。
//
// 主题变体 = 同一 ColorToken 键、每主题一张稀疏 delta 表（等价前端 [data-theme] 覆盖块）。
// 新增语义色：在 ColorToken 末尾追加（Count 之前），编译器会强制补齐所有主题表。
#include <QColor>
#include <QHash>
#include <array>

namespace kl {
namespace ds {

// ---------------------------------------------------------------------------
// 原料层：裸色值。只有本命名空间允许出现颜色字面量。
// 浅色值来源：考据静态资源（ticktick-windows-6.3.3/02-layout-and-visual-style.md、
// resource-evidence.json color_resources/default_theme）与 Web 现场（01:62-75）。
// 深色值为 KylinList 自定，非滴答实测（考据无深色色值证据）。
// ---------------------------------------------------------------------------
namespace raw {
// 中性
constexpr QRgb White = 0xFFFFFFFF;
constexpr QRgb Black = 0xFF191919;
// 品牌主色（考据 primary_blue_100 = #4772FA）
constexpr QRgb Primary = 0xFF4772FA;
constexpr QRgb PrimaryHover = 0xFF3A63E0;   // 由主色加深，hover/pressed
constexpr QRgb PrimarySoft = 0xFFEAF0FF;    // 选中底（主色极浅）
// 功能色（考据 PriorityHighColor/OutDateColor=#E13E39、PriorityMiddleColor=#FAA80C）
constexpr QRgb Danger = 0xFFE13E39;
constexpr QRgb Warning = 0xFFFAA80C;
constexpr QRgb Success = 0xFF2FA84F;
constexpr QRgb Info = 0xFF4772FA;
// 浅色中性阶
constexpr QRgb L_Text = 0xFF20242C;
constexpr QRgb L_TextSecondary = 0xFF6B7484;
constexpr QRgb L_TextTertiary = 0xFFAEB5C0;
constexpr QRgb L_Surface = 0xFFFFFFFF;
constexpr QRgb L_Bg = 0xFFF5F7FC;
constexpr QRgb L_Sidebar = 0xFFF5F7FC;
constexpr QRgb L_SidebarHover = 0xFFEBEFF8;
constexpr QRgb L_Hover = 0xFFF2F4F8;
constexpr QRgb L_Field = 0xFFF2F4F8;
constexpr QRgb L_FieldHover = 0xFFECEFF5;
constexpr QRgb L_Border = 0xFFE4E8F0;
constexpr QRgb L_Divider = 0xFFEDF0F5;
constexpr QRgb L_MenuBg = 0xFFFFFFFF;
constexpr QRgb L_MenuHover = 0xFFF2F4F8;
constexpr QRgb L_Disabled = 0xFFA8A8A8;  // 考据 disabled #a8a8a8
constexpr QRgb L_Scrollbar = 0xFFD9DFEA;
constexpr QRgb L_ScrollbarHover = 0xFFC2CCDD;
constexpr QRgb L_TooltipBg = 0xFFFFFFFF;
// 深色中性阶（KylinList 自定）
constexpr QRgb D_Text = 0xFFE8EAED;
constexpr QRgb D_TextSecondary = 0xFF9AA0A6;
constexpr QRgb D_TextTertiary = 0xFF6B7078;
constexpr QRgb D_Surface = 0xFF1E2126;
constexpr QRgb D_Bg = 0xFF16181C;
constexpr QRgb D_Sidebar = 0xFF1A1D22;
constexpr QRgb D_SidebarHover = 0xFF262A31;
constexpr QRgb D_Hover = 0xFF262A31;
constexpr QRgb D_Field = 0xFF262A31;
constexpr QRgb D_FieldHover = 0xFF2E333B;
constexpr QRgb D_Border = 0xFF343943;
constexpr QRgb D_Divider = 0xFF2A2E36;
constexpr QRgb D_MenuBg = 0xFF24272E;
constexpr QRgb D_MenuHover = 0xFF2E333B;
constexpr QRgb D_Disabled = 0xFF5A5F68;
constexpr QRgb D_Scrollbar = 0xFF3A3F49;
constexpr QRgb D_ScrollbarHover = 0xFF4A505C;
constexpr QRgb D_TooltipBg = 0xFF2A2E36;
constexpr QRgb D_PrimarySoft = 0xFF2A3350;  // 深色选中底
}  // namespace raw

// ---------------------------------------------------------------------------
// 语义层：业务唯一入口。
// ---------------------------------------------------------------------------
enum class ColorToken : int {
    Text, TextSecondary, TextTertiary, OnPrimary,
    Primary, PrimaryHover, PrimarySoft, FocusRing,
    Bg, Surface, SurfaceHover, Border, Divider,
    Sidebar, SidebarHover, SidebarText,
    Field, FieldHover,
    MenuBg, MenuHover, MenuText, MenuDisabled,
    Hover, Selection,
    Danger, Warning, Success, Info,
    DangerBg, WarningBg, SuccessBg, InfoBg,
    PriorityHigh, PriorityMedium, PriorityLow,
    Overdue,
    Scrollbar, ScrollbarHover,
    TooltipBg, TooltipText,
    Count
};

enum class Theme : int { Light = 0, Dark = 1, Count };

// 每主题一张完整值表（键 = ColorToken）。稀疏覆盖的简洁性由填表函数保证。
using ColorTable = std::array<QRgb, static_cast<size_t>(ColorToken::Count)>;

const ColorTable& colorTable(Theme theme);
QColor color(ColorToken token, Theme theme);

}  // namespace ds
}  // namespace kl
