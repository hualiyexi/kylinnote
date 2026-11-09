#include "DesignTokens.h"

namespace kl {
namespace ds {
namespace {

ColorTable makeLight() {
    using namespace raw;
    ColorTable t{};
    auto set = [&t](ColorToken k, QRgb v) { t[static_cast<size_t>(k)] = v; };
    set(ColorToken::Text, L_Text);
    set(ColorToken::TextSecondary, L_TextSecondary);
    set(ColorToken::TextTertiary, L_TextTertiary);
    set(ColorToken::OnPrimary, White);
    set(ColorToken::Primary, Primary);
    set(ColorToken::PrimaryHover, PrimaryHover);
    set(ColorToken::PrimarySoft, PrimarySoft);
    set(ColorToken::FocusRing, Primary);
    set(ColorToken::Bg, L_Bg);
    set(ColorToken::Surface, L_Surface);
    set(ColorToken::SurfaceHover, L_Hover);
    set(ColorToken::Border, L_Border);
    set(ColorToken::Divider, L_Divider);
    set(ColorToken::Sidebar, L_Sidebar);
    set(ColorToken::SidebarHover, L_SidebarHover);
    set(ColorToken::SidebarText, L_Text);
    set(ColorToken::Field, L_Field);
    set(ColorToken::FieldHover, L_FieldHover);
    set(ColorToken::MenuBg, L_MenuBg);
    set(ColorToken::MenuHover, L_MenuHover);
    set(ColorToken::MenuText, L_Text);
    set(ColorToken::MenuDisabled, L_Disabled);
    set(ColorToken::Hover, L_Hover);
    set(ColorToken::Selection, PrimarySoft);
    set(ColorToken::Danger, Danger);
    set(ColorToken::Warning, Warning);
    set(ColorToken::Success, Success);
    set(ColorToken::Info, Info);
    set(ColorToken::DangerBg, 0xFFFBF0EE);
    set(ColorToken::WarningBg, 0xFFFBF4E6);
    set(ColorToken::SuccessBg, 0xFFEDF7F0);
    set(ColorToken::InfoBg, PrimarySoft);
    set(ColorToken::PriorityHigh, Danger);
    set(ColorToken::PriorityMedium, Warning);
    set(ColorToken::PriorityLow, Primary);
    set(ColorToken::Overdue, Danger);
    set(ColorToken::Scrollbar, L_Scrollbar);
    set(ColorToken::ScrollbarHover, L_ScrollbarHover);
    set(ColorToken::TooltipBg, L_TooltipBg);
    set(ColorToken::TooltipText, L_Text);
    set(ColorToken::DetailDivider, 0xFFE6E6E6);
    set(ColorToken::HighlightYellow, 0xFFFFED99);
    set(ColorToken::HighlightGreen, 0xFFD6EFC0);
    set(ColorToken::HighlightBlue, 0xFFCFE6FA);
    set(ColorToken::HighlightPink, 0xFFF9D6DF);
    set(ColorToken::HighlightPurple, 0xFFE7DAF7);
    return t;
}

ColorTable makeDark() {
    using namespace raw;
    ColorTable t{};
    auto set = [&t](ColorToken k, QRgb v) { t[static_cast<size_t>(k)] = v; };
    set(ColorToken::Text, D_Text);
    set(ColorToken::TextSecondary, D_TextSecondary);
    set(ColorToken::TextTertiary, D_TextTertiary);
    set(ColorToken::OnPrimary, White);
    set(ColorToken::Primary, Primary);
    set(ColorToken::PrimaryHover, PrimaryHover);
    set(ColorToken::PrimarySoft, D_PrimarySoft);
    set(ColorToken::FocusRing, Primary);
    set(ColorToken::Bg, D_Bg);
    set(ColorToken::Surface, D_Surface);
    set(ColorToken::SurfaceHover, D_Hover);
    set(ColorToken::Border, D_Border);
    set(ColorToken::Divider, D_Divider);
    set(ColorToken::Sidebar, D_Sidebar);
    set(ColorToken::SidebarHover, D_SidebarHover);
    set(ColorToken::SidebarText, D_Text);
    set(ColorToken::Field, D_Field);
    set(ColorToken::FieldHover, D_FieldHover);
    set(ColorToken::MenuBg, D_MenuBg);
    set(ColorToken::MenuHover, D_MenuHover);
    set(ColorToken::MenuText, D_Text);
    set(ColorToken::MenuDisabled, D_Disabled);
    set(ColorToken::Hover, D_Hover);
    set(ColorToken::Selection, D_PrimarySoft);
    set(ColorToken::Danger, Danger);
    set(ColorToken::Warning, Warning);
    set(ColorToken::Success, Success);
    set(ColorToken::Info, Info);
    set(ColorToken::DangerBg, 0xFF3A2422);
    set(ColorToken::WarningBg, 0xFF3A3220);
    set(ColorToken::SuccessBg, 0xFF1E3226);
    set(ColorToken::InfoBg, D_PrimarySoft);
    set(ColorToken::PriorityHigh, Danger);
    set(ColorToken::PriorityMedium, Warning);
    set(ColorToken::PriorityLow, Primary);
    set(ColorToken::Overdue, Danger);
    set(ColorToken::Scrollbar, D_Scrollbar);
    set(ColorToken::ScrollbarHover, D_ScrollbarHover);
    set(ColorToken::TooltipBg, D_TooltipBg);
    set(ColorToken::TooltipText, D_Text);
    set(ColorToken::DetailDivider, D_Divider);
    set(ColorToken::HighlightYellow, 0xFF675523);
    set(ColorToken::HighlightGreen, 0xFF345236);
    set(ColorToken::HighlightBlue, 0xFF2D4F6A);
    set(ColorToken::HighlightPink, 0xFF683F50);
    set(ColorToken::HighlightPurple, 0xFF514167);
    return t;
}

}  // namespace

const ColorTable& colorTable(Theme theme) {
    static const ColorTable light = makeLight();
    static const ColorTable dark = makeDark();
    return theme == Theme::Dark ? dark : light;
}

QColor color(ColorToken token, Theme theme) {
    return QColor::fromRgba(colorTable(theme)[static_cast<size_t>(token)]);
}

}  // namespace ds
}  // namespace kl
