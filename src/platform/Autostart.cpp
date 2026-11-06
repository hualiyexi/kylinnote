#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace kn {
namespace {

QStringList &argumentsStorage()
{
    // 默认登录自拉走后台模式，不抢用户焦点；调用方可覆盖
    static QStringList args = { QStringLiteral("--background") };
    return args;
}

QString executablePath()
{
    // 自包含 Linux 包里真身位于 /usr/lib/kylinnote，自启必须走 /usr/bin 的
    // 启动脚本才能拿到随包 Qt 库与插件路径
#ifdef KYLINNOTE_AUTOSTART_EXEC
    return QStringLiteral(KYLINNOTE_AUTOSTART_EXEC);
#else
    return QCoreApplication::applicationFilePath();
#endif
}

#ifdef Q_OS_WIN

const QString kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kValueName = QStringLiteral("KylinNote");

QString windowsCommandLine()
{
    // Run 键按命令行解析：路径必须加双引号（支持中文/空格路径）
    QString cmd = QLatin1Char('"') + QDir::toNativeSeparators(executablePath()) + QLatin1Char('"');
    const QStringList args = argumentsStorage();
    if (!args.isEmpty()) {
        cmd += QLatin1Char(' ') + args.join(QLatin1Char(' '));
    }
    return cmd;
}

#else

// freedesktop Desktop Entry 规范：Exec 含空白/特殊字符的参数必须双引号包裹，
// 引号内部的 ` " $ \ 需要反斜杠转义（保留字 % 需写成 %%）
QString quoteExecArgument(const QString &arg)
{
    static const QString special = QStringLiteral(" \t\n\"'`\\$><~|&;*?#()%");
    if (arg.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    bool needQuote = false;
    for (const QChar &c : arg) {
        if (special.contains(c)) {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) {
        return arg;
    }
    QString escaped;
    escaped.reserve(arg.size() + 2);
    for (const QChar &c : arg) {
        if (c == QLatin1Char('\\') || c == QLatin1Char('"') || c == QLatin1Char('`')
            || c == QLatin1Char('$')) {
            escaped.append(QLatin1Char('\\'));
        }
        escaped.append(c);
    }
    escaped.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString linuxExecLine()
{
    QStringList parts;
    parts.append(quoteExecArgument(executablePath()));
    const QStringList args = argumentsStorage();
    for (const QString &arg : args) {
        parts.append(quoteExecArgument(arg));
    }
    // Desktop string escaping is applied before Exec's command-line unquoting.
    // In particular, a literal backslash inside a quoted argument needs four
    // backslashes in the .desktop file; a literal '$' needs two before it.
    QString value = parts.join(QLatin1Char(' '));
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return value;
}

#endif

} // namespace

QString Autostart::filePath()
{
#ifdef Q_OS_WIN
    return kRunKey + QLatin1Char('\\') + kValueName;
#else
    return QDir::homePath() + QStringLiteral("/.config/autostart/kylinnote.desktop");
#endif
}

bool Autostart::isInstalled()
{
#ifdef Q_OS_WIN
    QSettings reg(kRunKey, QSettings::NativeFormat);
    return reg.contains(kValueName);
#else
    return QFile::exists(filePath());
#endif
}

bool Autostart::setEnabled(bool enabled, QString *error)
{
#ifdef Q_OS_WIN
    QSettings reg(kRunKey, QSettings::NativeFormat);
    if (enabled) {
        reg.setValue(kValueName, windowsCommandLine());
    } else {
        reg.remove(kValueName);
    }
    reg.sync();
    if (reg.status() != QSettings::NoError) {
        if (error) {
            *error = QStringLiteral("写入注册表自启项失败: ") + filePath();
        }
        return false;
    }
    return true;
#else
    const QString path = filePath();
    if (!enabled) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            if (error) {
                *error = QStringLiteral("无法删除自启文件: ") + path;
            }
            return false;
        }
        return true;
    }

    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("无法创建目录: ") + dir.absolutePath();
        }
        return false;
    }

    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=KylinNote\n"
        "GenericName=Desktop Notes\n"
        "Comment=KylinNote 桌面便签\n"
        "Exec=%1\n"
        "Icon=kylinnote\n"
        "Terminal=false\n"
        "NoDisplay=false\n"
        "Categories=Utility;\n"
        "StartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-KDE-autostart-after=panel\n"
        "Hidden=false\n").arg(linuxExecLine());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("无法写入 ") + path + QStringLiteral(": ") + file.errorString();
        }
        return false;
    }
    if (file.write(content.toUtf8()) == -1) {
        if (error) {
            *error = QStringLiteral("写入失败: ") + file.errorString();
        }
        return false;
    }
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
#endif
}

bool Autostart::ensureInstalled(QString *error)
{
    return setEnabled(true, error);
}

QStringList Autostart::arguments()
{
    return argumentsStorage();
}

void Autostart::setArguments(const QStringList &arguments)
{
    argumentsStorage() = arguments;
}

} // namespace kn
