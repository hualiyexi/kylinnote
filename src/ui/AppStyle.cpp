#include "AppStyle.h"
#include <QApplication>
#include "design/DesignTokens.h"
#include "design/QssBuilder.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace AppStyle {
namespace {

ds::ColorToken mapColor(Color role) {
    using C = ds::ColorToken;
    switch (role) {
        case Color::Text: return C::Text;
        case Color::Muted: return C::TextSecondary;
        case Color::Faint: return C::TextTertiary;
        case Color::Accent: return C::Primary;
        case Color::AccentDark: return C::PrimaryHover;
        case Color::OnAccent: return C::OnPrimary;
        case Color::Selection: return C::Selection;
        case Color::Sidebar: return C::Sidebar;
        case Color::SidebarHover: return C::SidebarHover;
        case Color::Surface: return C::Surface;
        case Color::Separator: return C::Divider;
        case Color::Hover: return C::Hover;
        case Color::Field: return C::Field;
        case Color::FieldHover: return C::FieldHover;
        case Color::Danger: return C::Danger;
        case Color::Warning: return C::Warning;
        case Color::Star: return C::Warning;
    }
    return C::Text;
}

}  // namespace

QColor color(Color role) {
    return ThemeManager::instance().color(mapColor(role));
}

int metric(Metric role) {
    // 考据基线（01-layout-and-implementation.md:19-30）：
    // 任务行 40px、导航行 36px、详情底栏 48px、圆角分层 14/12/10/8。
    // 适配中文密度与系统缩放，保留 ≥40×40 命中区。
    switch (role) {
        case Metric::ContentMargin: return 20;
        case Metric::SidebarWidth: return 232;   // 考据组织栏 249px，适配中文略收
        case Metric::ControlHeight: return 36;
        case Metric::TaskRowHeight: return 44;   // 考据 40px，中文+元信息两行略增
        case Metric::NavRowHeight: return 36;    // 考据 36px
        case Metric::RadiusRow: return 8;        // 考据悬停菜单项 8px
        case Metric::RadiusControl: return 6;
        case Metric::Hairline: return 1;
    }
    return 0;
}

QFont font(Font role) {
    switch (role) {
        case Font::Body: return ds::qfont(ds::FontScene::Body);
        case Font::Small: return ds::qfont(ds::FontScene::Caption);
        case Font::Heading: return ds::qfont(ds::FontScene::Display);
        case Font::Brand: {
            QFont f = ds::qfont(ds::FontScene::Section);
            f.setPointSizeF(11.0);
            return f;
        }
    }
    return ds::qfont(ds::FontScene::Body);
}

void apply(QApplication& app) {
    ThemeManager::instance().applyBootstrap(app);
    app.setFont(ds::qfont(ds::FontScene::Base));
}

QString previewCss() {
    return ds::buildPreviewCss(ThemeManager::instance().resolvedTheme());
}

}  // namespace AppStyle
}  // namespace kl
