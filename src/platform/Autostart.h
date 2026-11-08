#ifndef KYLINLIST_AUTOSTART_H
#define KYLINLIST_AUTOSTART_H

#include <QString>
#include <QStringList>

namespace kl {

// 开机自启（当前用户，无需管理员权限）：
//   Windows：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的 KylinList 值
//   Linux  ：~/.config/autostart/kylinlist.desktop
// 本类只提供读写能力，是否开启完全由调用方（用户设置）决定，
// 程序不会在未获用户授权时默认强制开启自启。
class Autostart
{
public:
    // Linux：desktop 文件路径。
    // Windows：对应的注册表值位置（仅用于诊断展示，并非真实文件）。
    static QString filePath();

    // 当前是否已登记自启
    static bool isInstalled();

    // 开启/关闭自启；失败返回 false 并填充 error（失败不影响主程序）
    static bool setEnabled(bool enabled, QString *error = nullptr);

    // 确保自启项存在且内容正确；等价于 setEnabled(true, error)
    static bool ensureInstalled(QString *error = nullptr);

    // 自启启动参数，默认 {"--background"}（登录自拉时不抢焦点）。
    // 传入空列表可关闭该参数；对 isInstalled/filePath 无影响。
    static QStringList arguments();
    static void setArguments(const QStringList &arguments);
};

} // namespace kl

#endif // KYLINLIST_AUTOSTART_H
