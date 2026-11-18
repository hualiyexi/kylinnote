#include "Autostart.h"
#include "../app/Branding.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace kl {
namespace {

QStringList &argumentsStorage()
{
    // 默认登录自拉走后台模式，不抢用户焦点；调用方可覆盖
    static QStringList args = { QStringLiteral("--background") };
    return args;
}

QString executablePath()
{
    // 自包含 Linux 包里真身位于 /usr/lib/kylinlist，自启必须走 /usr/bin 的
    // 启动脚本才能拿到随包 Qt 库与插件路径
#ifdef KYLINLIST_AUTOSTART_EXEC
    return QStringLiteral(KYLINLIST_AUTOSTART_EXEC);
#else
    return QCoreApplication::applicationFilePath();
#endif
}

#ifdef Q_OS_WIN

const QString kRunKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kValueName = QStringLiteral("KylinList");
const QString kPreviewValueName = QStringLiteral("KylinListPreview");

// Windows 命令行引用：空参数写成 ""；含空白或双引号的参数整体加引号，
// 并按 CommandLineToArgvW 的规则把参数末尾及引号前的反斜杠加倍。
QString quoteWindowsArgument(const QString &arg)
{
    if (arg.isEmpty())
        return QStringLiteral("\"\"");
    bool needQuote = false;
    for (const QChar &c : arg) {
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t') || c == QLatin1Char('"')) {
            needQuote = true;
            break;
        }
    }
    if (!needQuote)
        return arg;

    QString quoted;
    quoted.reserve(arg.size() + 2);
    quoted.append(QLatin1Char('"'));
    int backslashes = 0;
    for (const QChar &c : arg) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            quoted.append(QLatin1Char('"'));
        } else {
            quoted.append(QString(backslashes, QLatin1Char('\\')));
            quoted.append(c);
        }
        backslashes = 0;
    }
    quoted.append(QString(backslashes * 2, QLatin1Char('\\')));
    quoted.append(QLatin1Char('"'));
    return quoted;
}

QString windowsCommandLine()
{
    // Run 键按命令行解析：路径必须加双引号（支持中文/空格路径）
    return Autostart::commandLineFor(executablePath(), argumentsStorage());
}

QString windowsPreviewCommandLine(const QString &dataDir)
{
    return Autostart::commandLineFor(executablePath(), Autostart::previewArguments(dataDir));
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

QString desktopExecLine(const QStringList &args)
{
    QStringList parts;
    parts.append(quoteExecArgument(executablePath()));
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

QString linuxExecLine()
{
    return desktopExecLine(argumentsStorage());
}

#endif

} // namespace

QString Autostart::filePath()
{
#ifdef Q_OS_WIN
    return kRunKey + QLatin1Char('\\') + kValueName;
#else
    return QDir::homePath() + QStringLiteral("/.config/autostart/kylinlist.desktop");
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

#ifdef Q_OS_WIN
QString Autostart::commandLineFor(const QString &executable, const QStringList &arguments)
{
    QString cmd = QLatin1Char('"') + QDir::toNativeSeparators(executable) + QLatin1Char('"');
    for (const QString &arg : arguments)
        cmd += QLatin1Char(' ') + quoteWindowsArgument(arg);
    return cmd;
}
#endif

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
        "Name=%2\n"
        "GenericName=Entity List\n"
        "Comment=%2：本地任务与桌面便签\n"
        "Exec=%1\n"
        "Icon=kylinlist\n"
        "Terminal=false\n"
        "NoDisplay=false\n"
        "Categories=Utility;\n"
        "StartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-KDE-autostart-after=panel\n"
        "Hidden=false\n").arg(linuxExecLine(), Branding::displayName());

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

// —— 预览自启：独立槽位，无状态，绝不触碰上面的正式安装槽位 ——

QString Autostart::previewFilePath()
{
#ifdef Q_OS_WIN
    return kRunKey + QLatin1Char('\\') + kPreviewValueName;
#else
    return QDir::homePath() + QStringLiteral("/.config/autostart/kylinlist-preview.desktop");
#endif
}

QStringList Autostart::previewArguments(const QString &dataDir)
{
    // 登录自拉的预览实例带同一组参数：后台恢复、同一数据目录、且仍显式
    // --preview-autostart（否则重启后的预览实例无权管理自己的自启项）。
    return QStringList{QStringLiteral("--background"),
                       QStringLiteral("--data-dir"),
                       QDir(dataDir).absolutePath(),
                       QStringLiteral("--preview-autostart")};
}

bool Autostart::isPreviewInstalled()
{
#ifdef Q_OS_WIN
    QSettings reg(kRunKey, QSettings::NativeFormat);
    return reg.contains(kPreviewValueName);
#else
    return QFile::exists(previewFilePath());
#endif
}

bool Autostart::isPreviewRegisteredFor(const QString &dataDir)
{
    if (dataDir.trimmed().isEmpty())
        return false;
#ifdef Q_OS_WIN
    QSettings reg(kRunKey, QSettings::NativeFormat);
    if (!reg.contains(kPreviewValueName))
        return false;
    // Windows 路径不区分大小写；同一命令由本程序写入，比较整串即可。
    return QString::compare(reg.value(kPreviewValueName).toString(),
                            windowsPreviewCommandLine(dataDir),
                            Qt::CaseInsensitive) == 0;
#else
    QFile file(previewFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString content = QString::fromUtf8(file.readAll());
    return content.contains(QStringLiteral("Exec=") + desktopExecLine(previewArguments(dataDir))
                            + QLatin1Char('\n'));
#endif
}

bool Autostart::setPreviewEnabled(bool enabled, const QString &dataDir, QString *error)
{
    if (error)
        error->clear();
    if (dataDir.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("预览自启需要显式指定的数据目录");
        return false;
    }
#ifdef Q_OS_WIN
    QSettings reg(kRunKey, QSettings::NativeFormat);
    if (enabled) {
        reg.setValue(kPreviewValueName, windowsPreviewCommandLine(dataDir));
    } else {
        if (!reg.contains(kPreviewValueName))
            return true;  // 槽位为空：无事可做，视为成功
        if (QString::compare(reg.value(kPreviewValueName).toString(),
                             windowsPreviewCommandLine(dataDir),
                             Qt::CaseInsensitive) != 0) {
            // 条目属于其他数据目录或被手工修改：绝不删除，交给用户处理。
            if (error)
                *error = QStringLiteral("预览自启项与当前数据目录不匹配，未删除: ")
                             + previewFilePath();
            return false;
        }
        reg.remove(kPreviewValueName);
    }
    reg.sync();
    if (reg.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("写入注册表预览自启项失败: ") + previewFilePath();
        return false;
    }
    return true;
#else
    const QString path = previewFilePath();
    if (!enabled) {
        if (!QFile::exists(path))
            return true;  // 槽位为空：无事可做，视为成功
        if (!isPreviewRegisteredFor(dataDir)) {
            // 条目属于其他数据目录或被手工修改：绝不删除，交给用户处理。
            if (error)
                *error = QStringLiteral("预览自启项与当前数据目录不匹配，未删除: ") + path;
            return false;
        }
        if (!QFile::remove(path)) {
            if (error)
                *error = QStringLiteral("无法删除预览自启文件: ") + path;
            return false;
        }
        return true;
    }

    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("无法创建目录: ") + dir.absolutePath();
        return false;
    }

    const QString content = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Version=1.0\n"
        "Name=%2 预览\n"
        "GenericName=Entity List\n"
        "Comment=%2 开发预览自启（--data-dir 隔离数据目录）\n"
        "Exec=%1\n"
        "Icon=kylinlist\n"
        "Terminal=false\n"
        "NoDisplay=false\n"
        "Categories=Utility;\n"
        "StartupNotify=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-KDE-autostart-after=panel\n"
        "Hidden=false\n").arg(desktopExecLine(previewArguments(dataDir)), Branding::displayName());

    // QSaveFile 原子替换：先写临时文件再原子提交，失败不会留下半截 desktop 文件。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法写入 ") + path + QStringLiteral(": ")
                         + file.errorString();
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        if (error)
            *error = QStringLiteral("写入预览自启文件失败: ") + path;
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("保存预览自启文件失败: ") + path + QStringLiteral(": ")
                         + file.errorString();
        return false;
    }
    // 部分桌面环境要求自启文件带可执行位（作用于已提交的最终文件）。
    QFile::setPermissions(path, QFile::permissions(path) | QFile::ReadOwner | QFile::WriteOwner
                                        | QFile::ExeOwner);
    return true;
#endif
}

QStringList Autostart::arguments()
{
    return argumentsStorage();
}

void Autostart::setArguments(const QStringList &arguments)
{
    argumentsStorage() = arguments;
}

} // namespace kl
