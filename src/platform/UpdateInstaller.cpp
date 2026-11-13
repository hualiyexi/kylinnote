#include "UpdateInstaller.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace kl {
namespace updates {
namespace {

QString sha256OfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return QString();
    return QString::fromLatin1(hash.result().toHex());
}

} // namespace

bool verifyInstallerFile(const QString &filePath, qint64 expectedSize,
                         const QString &expectedSha256, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile())
        return fail(QStringLiteral("找不到已下载的安装包"));
    if (info.size() != expectedSize)
        return fail(QStringLiteral("安装包大小与发布清单不符"));
    const QString actual = sha256OfFile(filePath);
    if (actual.isEmpty())
        return fail(QStringLiteral("无法读取安装包进行校验"));
    if (actual.compare(expectedSha256, Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("安装包校验失败，文件内容与发布清单不符"));
    if (error) error->clear();
    return true;
}

bool handOffToInstaller(const QString &filePath, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile())
        return fail(QStringLiteral("找不到安装包：%1").arg(filePath));

    const QString fakeInstaller = qEnvironmentVariable("KYLINLIST_UPDATE_FAKE_INSTALLER");
    if (!fakeInstaller.isEmpty()) {
        // 测试钩子：用指定脚本代替真实系统安装器。脚本以安装包路径作为唯一参数。
#ifdef Q_OS_WIN
        const bool started = QProcess::startDetached(
            QStringLiteral("cmd"), {QStringLiteral("/C"), fakeInstaller, filePath});
#else
        const bool started = QProcess::startDetached(
            QStringLiteral("/bin/sh"), {fakeInstaller, filePath});
#endif
        return started ? true
            : fail(QStringLiteral("无法启动测试安装器：%1").arg(fakeInstaller));
    }

    const QString suffix = info.suffix().toLower();
#ifdef Q_OS_WIN
    if (suffix == QStringLiteral("exe")) {
        // Inno Setup 安装器自行处理文件占用与用户级覆盖安装。
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(filePath))) return true;
        return fail(QStringLiteral("无法启动安装程序，请手动运行：%1").arg(filePath));
    }
    if (suffix == QStringLiteral("zip")) {
        // 便携版不做自动替换，交给用户手动解压覆盖。
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()))) return true;
        return fail(QStringLiteral("无法打开安装包所在文件夹：%1").arg(info.absolutePath()));
    }
    return fail(QStringLiteral("无法识别的 Windows 安装包类型：.%1").arg(suffix));
#else
    if (suffix == QStringLiteral("deb")) {
        // 交给系统默认的图形化 deb 安装器完成授权、依赖与覆盖安装。
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(filePath))) return true;
        return fail(QStringLiteral("系统没有可用的安装程序打开该 deb，请手动安装：%1").arg(filePath));
    }
    return fail(QStringLiteral("无法识别的 Linux 安装包类型：.%1").arg(suffix));
#endif
}

} // namespace updates
} // namespace kl
