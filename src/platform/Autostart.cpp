#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace kn {

QString Autostart::filePath()
{
    return QDir::homePath() + QStringLiteral("/.config/autostart/kylinnote.desktop");
}

bool Autostart::isInstalled()
{
    return QFile::exists(filePath());
}

bool Autostart::ensureInstalled(QString *error)
{
    const QString path = filePath();
    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("无法创建目录: ") + dir.absolutePath();
        }
        return false;
    }

    // 自包含包里真身位于 /usr/lib/kylinnote，自启必须走 /usr/bin 的启动脚本
    // 才能拿到随包 Qt 库与插件路径
    QString exec = QCoreApplication::applicationFilePath();
#ifdef KYLINNOTE_AUTOSTART_EXEC
    exec = QStringLiteral(KYLINNOTE_AUTOSTART_EXEC);
#endif

    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=KylinNote\n"
        "GenericName=Desktop Notes\n"
        "Comment=KylinNote \u684c\u9762\u4fbf\u7b3a\n"
        "Exec=%1\n"
        "Icon=kylinnote\n"
        "Terminal=false\n"
        "NoDisplay=false\n"
        "Categories=Utility;\n"
        "StartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-KDE-autostart-after=panel\n"
        "Hidden=false\n").arg(exec);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("无法写入 ") + path + QStringLiteral(": ") + file.errorString();
        }
        return false;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << content;
    out.flush();
    file.close();

    if (file.error() != QFile::NoError) {
        if (error) {
            *error = QStringLiteral("写入失败: ") + file.errorString();
        }
        return false;
    }

    // 部分桌面环境要求自启文件带可执行位
    file.setPermissions(file.permissions() | QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return true;
}

} // namespace kn
