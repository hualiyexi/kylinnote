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
    set(ColorToken::FocusRing, L_Border);
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
    set(ColorToken::TooltipText, White);
    set(ColorToken::DetailDivider, 0xFFE6E6E6);
    set(ColorToken::HighlightYellow, 0xFFFFED99);
    set(ColorToken::HighlightGreen, 0xFFD6EFC0);
    set(ColorToken::HighlightBlue, 0xFFCFE6FA);
    set(ColorToken::HighlightPink, 0xFFF9D6DF);
    set(ColorToken::HighlightPurple, 0xFFE7DAF7);
    set(ColorToken::WindowBorder, 0xFFADB3BC);
    set(ColorToken::Shadow, Black);
    set(ColorToken::RowHover, L_RowHover);
    set(ColorToken::RowSelected, L_RowSelected);
    set(ColorToken::RailBg, L_RailBg);
    set(ColorToken::QuickAddBg, L_QuickAddBg);
    set(ColorToken::AttachmentDocumentBg, 0xFFE8F4FF);
    set(ColorToken::AttachmentSpreadsheetBg, 0xFFEAFBF2);
    set(ColorToken::AttachmentTextBg, 0xFFEFF2F3);
    set(ColorToken::AttachmentOtherBg, 0xFFEDF0FF);
    set(ColorToken::AppIconBg, White);
    set(ColorToken::SettingsRailBg, 0xFFF8F8F8);
    set(ColorToken::SettingsSelectedBg, 0x0A191919);
    set(ColorToken::SettingsContentBg, White);
    set(ColorToken::SettingsSectionBg, 0x05191919);
    QColor rail(Black);
    rail.setAlphaF(0.4);
    set(ColorToken::RailIcon, rail.rgba());
    rail.setAlphaF(0.6);
    set(ColorToken::RailIconHover, rail.rgba());
    set(ColorToken::NotePaperYellow, L_NoteYellow);
    set(ColorToken::NotePaperBlue, L_NoteBlue);
    set(ColorToken::NotePaperGreen, L_NoteGreen);
    set(ColorToken::NotePaperPink, L_NotePink);
    set(ColorToken::NotePaperPurple, L_NotePurple);
    set(ColorToken::NotePaperWhite, White);
    set(ColorToken::NoteText, L_Text);
    set(ColorToken::NoteMuted, L_TextSecondary);
    set(ColorToken::NoteBorder, L_TextTertiary);
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
    set(ColorToken::FocusRing, D_Border);
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
    set(ColorToken::TooltipText, White);
    set(ColorToken::DetailDivider, D_Divider);
    set(ColorToken::HighlightYellow, 0xFF675523);
    set(ColorToken::HighlightGreen, 0xFF345236);
    set(ColorToken::HighlightBlue, 0xFF2D4F6A);
    set(ColorToken::HighlightPink, 0xFF683F50);
    set(ColorToken::HighlightPurple, 0xFF514167);
    set(ColorToken::WindowBorder, 0xFF555B65);
    set(ColorToken::Shadow, Black);
    set(ColorToken::RowHover, D_RowHover);
    set(ColorToken::RowSelected, D_RowSelected);
    set(ColorToken::RailBg, D_RailBg);
    set(ColorToken::QuickAddBg, D_Field);
    set(ColorToken::AttachmentDocumentBg, 0xFF203144);
    set(ColorToken::AttachmentSpreadsheetBg, 0xFF1D352A);
    set(ColorToken::AttachmentTextBg, 0xFF282D33);
    set(ColorToken::AttachmentOtherBg, 0xFF2A2E48);
    set(ColorToken::AppIconBg, White);
    set(ColorToken::SettingsRailBg, D_RailBg);
    set(ColorToken::SettingsSelectedBg, D_Hover);
    set(ColorToken::SettingsContentBg, D_Surface);
    set(ColorToken::SettingsSectionBg, D_Field);
    QColor rail(White);
    rail.setAlphaF(0.4);
    set(ColorToken::RailIcon, rail.rgba());
    rail.setAlphaF(0.6);
    set(ColorToken::RailIconHover, rail.rgba());
    set(ColorToken::NotePaperYellow, D_NoteYellow);
    set(ColorToken::NotePaperBlue, D_NoteBlue);
    set(ColorToken::NotePaperGreen, D_NoteGreen);
    set(ColorToken::NotePaperPink, D_NotePink);
    set(ColorToken::NotePaperPurple, D_NotePurple);
    set(ColorToken::NotePaperWhite, D_Surface);
    set(ColorToken::NoteText, D_Text);
    set(ColorToken::NoteMuted, D_TextSecondary);
    set(ColorToken::NoteBorder, t[static_cast<size_t>(ColorToken::WindowBorder)]);
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
