#ifndef KYLINLIST_AUTOSTART_H
#define KYLINLIST_AUTOSTART_H

#include <QString>
#include <QStringList>

namespace kl {

// 开机自启（当前用户，无需管理员权限），正式安装槽位：
//   Windows：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的 KylinList 值
//   Linux  ：~/.config/autostart/kylinlist.desktop
// 正式安装首次运行时由应用默认开启，用户可在设置中关闭；一次性初始化后尊重用户选择。
//
// 预览自启（--data-dir 隔离运行经 --preview-autostart 显式开启后手动登记）使用
// 独立的当前用户槽位，与正式安装槽位互不读写：
//   Windows：同一 Run 键的 KylinListPreview 值
//   Linux  ：~/.config/autostart/kylinlist-preview.desktop
// 预览接口全部无状态：数据目录由调用方显式传入，不读取/不修改 arguments()
// 全局，也绝不触碰正式安装槽位。
class Autostart
{
public:
    // Linux：desktop 文件路径。
    // Windows：对应的注册表值位置（仅用于诊断展示，并非真实文件）。
    static QString filePath();

    // 当前是否已登记自启
    static bool isInstalled();

#ifdef Q_OS_WIN
    // Windows Run 键的命令行拼接（可执行文件始终加引号；含空白/引号的
    // 参数按命令解析规则转义）。公开用于策略测试，不写注册表。
    static QString commandLineFor(const QString &executable, const QStringList &arguments);
#endif

    // 开启/关闭自启；失败返回 false 并填充 error（失败不影响主程序）
    static bool setEnabled(bool enabled, QString *error = nullptr);

    // 确保自启项存在且内容正确；等价于 setEnabled(true, error)
    static bool ensureInstalled(QString *error = nullptr);

    // 自启启动参数，默认 {"--background"}（登录自拉时不抢焦点）。
    // 传入空列表可关闭该参数；对 isInstalled/filePath 无影响。
    // 仅描述正式安装槽位的命令；预览槽位接口不使用该全局状态。
    static QStringList arguments();
    static void setArguments(const QStringList &arguments);

    // —— 预览自启（独立槽位，无状态；dataDir 为隔离数据目录）——

    // 预览槽位路径：Windows 为 Run 键 KylinListPreview 值位置（诊断展示），
    // Linux 为 ~/.config/autostart/kylinlist-preview.desktop。
    static QString previewFilePath();

    // 预览登记命令的参数，同时是状态匹配的判据：
    //   {"--background", "--data-dir", <dataDir 绝对路径>, "--preview-autostart"}
    // 登录自拉的预览实例因此同样带 --preview-autostart，可继续管理该预览项。
    static QStringList previewArguments(const QString &dataDir);

    // 预览槽位当前是否存在任何条目（不校验命令内容）。
    static bool isPreviewInstalled();

    // 预览槽位登记的命令是否与 dataDir 的登记命令完全一致（同一可执行文件、
    // 同一数据目录路径）。别的数据目录或手工修改过的条目都不算已登记。
    static bool isPreviewRegisteredFor(const QString &dataDir);

    // 写/删预览槽位（绝不读写正式安装槽位）：
    //   enabled=true ：写入 previewArguments(dataDir) 的完整命令（覆盖旧值）。
    //   enabled=false：槽位为空视为成功；登记命令与 dataDir 完全一致才删除；
    //                  不一致（属于其他数据目录或被手工修改）时绝不删除，
    //                  返回 false 并填充 error。dataDir 为空视为参数错误。
    static bool setPreviewEnabled(bool enabled, const QString &dataDir, QString *error = nullptr);
};

} // namespace kl

#endif // KYLINLIST_AUTOSTART_H
