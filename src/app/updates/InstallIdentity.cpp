#include "InstallIdentity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "../../Common.h"

namespace kl {
namespace updates {
namespace {

bool variantAllowed(const QString &platform, const QString &variant)
{
    if (platform == QStringLiteral("linux"))
        return variant == QStringLiteral("system-qt") || variant == QStringLiteral("bundled-qt");
    if (platform == QStringLiteral("windows"))
        return variant == QStringLiteral("setup") || variant == QStringLiteral("portable");
    return false;
}

} // namespace

bool InstallIdentity::isValid() const
{
    return product == QStringLiteral("kylinlist")
        && (platform == QStringLiteral("linux") || platform == QStringLiteral("windows"))
        && !version.isEmpty()
        && (arch == QStringLiteral("arm64") || arch == QStringLiteral("amd64"))
        && variantAllowed(platform, variant);
}

QString InstallIdentity::targetKey() const
{
    return platform + QLatin1Char('-') + arch + QLatin1Char('-') + variant;
}

QString currentBuildPlatform()
{
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    return QStringLiteral("linux");
#endif
}

QString currentBuildArch()
{
#if defined(Q_PROCESSOR_ARM64)
    return QStringLiteral("arm64");
#elif defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("amd64");
#else
    return QStringLiteral("unknown");
#endif
}

QString currentBuildVariantChoices()
{
    return currentBuildPlatform() == QStringLiteral("windows")
        ? QStringLiteral("setup, portable")
        : QStringLiteral("system-qt, bundled-qt");
}

InstallIdentity parseInstallInfoJson(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return InstallIdentity();
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("安装身份文件不是有效的 JSON：%1").arg(parseError.errorString()));
    const QJsonObject object = document.object();
    InstallIdentity identity;
    identity.product = object.value(QStringLiteral("product")).toString().toLower();
    identity.platform = object.value(QStringLiteral("platform")).toString().toLower();
    identity.version = object.value(QStringLiteral("version")).toString().trimmed();
    identity.arch = object.value(QStringLiteral("arch")).toString().toLower();
    identity.variant = object.value(QStringLiteral("variant")).toString().toLower();
    if (identity.product != QStringLiteral("kylinlist"))
        return fail(QStringLiteral("安装身份产品名不是 kylinlist"));
    if (identity.platform != currentBuildPlatform())
        return fail(QStringLiteral("安装身份平台 %1 与当前程序 %2 不符")
                        .arg(identity.platform, currentBuildPlatform()));
    if (identity.arch != currentBuildArch())
        return fail(QStringLiteral("安装身份架构 %1 与当前构建 %2 不符")
                        .arg(identity.arch, currentBuildArch()));
    if (!variantAllowed(identity.platform, identity.variant))
        return fail(QStringLiteral("安装身份形式 %1 不受支持（允许：%2）")
                        .arg(identity.variant, currentBuildVariantChoices()));
    const QString buildVersion = QStringLiteral(KYLINLIST_VERSION);
    if (identity.version != buildVersion)
        return fail(QStringLiteral("安装身份版本 %1 与程序版本 %2 不符")
                        .arg(identity.version, buildVersion));
    if (error) error->clear();
    return identity;
}

InstallIdentity readInstallIdentity(QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return InstallIdentity();
    };
    const QString overridePath = qEnvironmentVariable("KYLINLIST_UPDATE_INSTALL_INFO_PATH");
    QVector<QString> candidates;
    if (!overridePath.isEmpty()) {
        candidates.append(overridePath);
    } else {
        const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
        candidates.append(appDir + QStringLiteral("/install-info.json"));
#else
        // /usr/bin/kylinlist → /usr/share/kylinlist/install-info.json
        candidates.append(QDir(appDir + QStringLiteral("/../share/kylinlist/install-info.json"))
                              .canonicalPath());
        candidates.append(QStringLiteral("/usr/share/kylinlist/install-info.json"));
        candidates.append(QStringLiteral("/usr/local/share/kylinlist/install-info.json"));
        candidates.append(appDir + QStringLiteral("/install-info.json"));
#endif
    }
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty()) continue;
        QFile file(candidate);
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("无法读取安装身份文件 %1").arg(candidate));
        const InstallIdentity identity = parseInstallInfoJson(file.readAll(), error);
        if (identity.isValid()) return identity;
        if (error && !error->isEmpty())
            *error = QStringLiteral("%1：%2").arg(candidate, *error);
        return identity;  // 文件存在但内容非法：直接返回，不再尝试其他位置
    }
    return fail(QStringLiteral("未找到安装身份文件 install-info.json"
                               "（开发环境可设置 KYLINLIST_UPDATE_INSTALL_INFO_PATH 指向测试身份）"));
}

} // namespace updates
} // namespace kl
