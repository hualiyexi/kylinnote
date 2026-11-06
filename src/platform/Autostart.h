#ifndef KYLINNOTE_AUTOSTART_H
#define KYLINNOTE_AUTOSTART_H

#include <QString>

namespace kn {

// 开机自启：写入 ~/.config/autostart/kylinnote.desktop
class Autostart
{
public:
    static QString filePath();

    // 确保自启文件存在且内容正确；返回是否成功（失败不影响主程序）
    static bool ensureInstalled(QString *error = nullptr);

    // 当前自启文件是否已存在
    static bool isInstalled();
};

} // namespace kn

#endif // KYLINNOTE_AUTOSTART_H
