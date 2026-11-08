#include "ThemeManager.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>
#include "QssBuilder.h"

namespace kl {
namespace {
const char* const kSettingsKey = "appearance/theme";
}

ThemeManager& ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

ThemeManager::ThemeManager() {
    QSettings settings;
    const QString stored = settings.value(QLatin1String(kSettingsKey)).toString();
    if (stored == QLatin1String("dark")) m_preference = Preference::Dark;
    else if (stored == QLatin1String("system")) m_preference = Preference::System;
    else m_preference = Preference::Light;  // 默认或非法值回落浅色
    m_resolved = resolve(m_preference);
}

bool ThemeManager::systemPrefersDark() {
    // Qt 5.12 无统一系统暗色查询；用窗口底色亮度作启发式。
    const QColor base = QApplication::palette().color(QPalette::Window);
    return base.lightness() < 128;
}

ds::Theme ThemeManager::resolve(Preference pref) const {
    switch (pref) {
        case Preference::Dark: return ds::Theme::Dark;
        case Preference::System: return systemPrefersDark() ? ds::Theme::Dark : ds::Theme::Light;
        case Preference::Light: break;
    }
    return ds::Theme::Light;
}

QColor ThemeManager::color(ds::ColorToken token) const {
    return ds::color(token, m_resolved);
}

void ThemeManager::applyBootstrap(QApplication& app) {
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const ds::Theme theme = m_resolved;
    QPalette palette;
    palette.setColor(QPalette::Window, ds::color(ds::ColorToken::Bg, theme));
    palette.setColor(QPalette::WindowText, ds::color(ds::ColorToken::Text, theme));
    palette.setColor(QPalette::Base, ds::color(ds::ColorToken::Surface, theme));
    palette.setColor(QPalette::AlternateBase, ds::color(ds::ColorToken::Field, theme));
    palette.setColor(QPalette::Text, ds::color(ds::ColorToken::Text, theme));
    palette.setColor(QPalette::Button, ds::color(ds::ColorToken::Field, theme));
    palette.setColor(QPalette::ButtonText, ds::color(ds::ColorToken::Text, theme));
    palette.setColor(QPalette::Highlight, ds::color(ds::ColorToken::Primary, theme));
    palette.setColor(QPalette::HighlightedText, ds::color(ds::ColorToken::OnPrimary, theme));
    palette.setColor(QPalette::ToolTipBase, ds::color(ds::ColorToken::TooltipBg, theme));
    palette.setColor(QPalette::ToolTipText, ds::color(ds::ColorToken::TooltipText, theme));
    palette.setColor(QPalette::Link, ds::color(ds::ColorToken::Primary, theme));
    palette.setColor(QPalette::PlaceholderText, ds::color(ds::ColorToken::TextTertiary, theme));
    palette.setColor(QPalette::Disabled, QPalette::Text, ds::color(ds::ColorToken::MenuDisabled, theme));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, ds::color(ds::ColorToken::MenuDisabled, theme));
    app.setPalette(palette);
    app.setStyleSheet(ds::buildStyleSheet(theme));
}

void ThemeManager::setPreference(Preference pref) {
    if (pref == m_preference) return;
    m_preference = pref;
    m_resolved = resolve(pref);
    QSettings settings;
    // 默认浅色不写盘（等价前端默认主题不挂 data-theme）。
    if (pref == Preference::Light) settings.remove(QLatin1String(kSettingsKey));
    else settings.setValue(QLatin1String(kSettingsKey),
                           pref == Preference::Dark ? QStringLiteral("dark") : QStringLiteral("system"));

    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        applyBootstrap(*app);
        for (QWidget* w : QApplication::topLevelWidgets()) {
            w->style()->unpolish(w);
            w->style()->polish(w);
            w->update();
        }
    }
    emit themeChanged(m_resolved);
}

}  // namespace kl
