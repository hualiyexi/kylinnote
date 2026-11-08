#pragma once
// 主题管理器：浅色 / 深色 / 跟随系统三态，运行时切换、QSettings 持久化、无白闪。
//
// 借鉴 CaseLawLibrary useTheme.ts：模块加载即 applyBootstrap 防白闪、默认主题不写盘、
// 非法值回落、themeChanged 信号通知所有消费者（QSS 重 apply + 自绘控件 update）。
#include <QObject>
#include "DesignTokens.h"

class QApplication;
namespace kl {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    // 用户可选的主题偏好。System 表示跟随系统明暗。
    enum class Preference { Light, Dark, System };

    static ThemeManager& instance();

    Preference preference() const { return m_preference; }
    // 实际生效的解析主题（System 已解析为 Light/Dark）。
    ds::Theme resolvedTheme() const { return m_resolved; }

    // 在 QApplication 构造后、首个 show() 前调用，防白闪。
    void applyBootstrap(QApplication& app);
    // 切换主题偏好：写盘 + 重新解析 + emit themeChanged。
    void setPreference(Preference pref);

    // 当前主题下取语义色。
    QColor color(ds::ColorToken token) const;

signals:
    void themeChanged(kl::ds::Theme theme);

private:
    ThemeManager();
    ds::Theme resolve(Preference pref) const;
    static bool systemPrefersDark();

    Preference m_preference = Preference::Light;
    ds::Theme m_resolved = ds::Theme::Light;
};

}  // namespace kl
