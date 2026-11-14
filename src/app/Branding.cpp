#include "Branding.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSize>

#include "../Common.h"

namespace kl {
namespace Branding {
namespace {

const QString kDefaultDisplayName = QStringLiteral("麒麟清单");
const QString kManifestName = QStringLiteral("branding.json");
const QString kDisplayNameKey = QStringLiteral("displayName");
const QString kLogoKey = QStringLiteral("logo");
// displayName 去除首尾空白后非空且最长 80 个字符，否则视为无效并保持默认。
const int kMaxDisplayNameLength = 80;

// load() 之后缓存的结果，仅主线程使用。
struct State {
    QString displayName = kDefaultDisplayName;  // 未 load 过时也是默认名
    QString configDir;   // 命中的配置目录；未命中为空
    QString logoPath;    // 解析后的 logo 绝对路径；未配置为空
    bool iconResolved = false;  // 当前 load 周期内是否已尝试构建图标
    QIcon icon;          // 缓存图标；未配置或加载失败时为 null
};

State &state()
{
    static State cache;
    return cache;
}

// 按优先级返回第一个存在 branding.json 的目录；都没有则返回空串。
// 某个候选目录缺文件不算错误，继续找下一个；全部未命中走默认。
QString resolveConfigDirectory()
{
    QStringList candidates;
    const QString overrideDir = qEnvironmentVariable("KYLINLIST_BRANDING_DIR").trimmed();
    if (!overrideDir.isEmpty())
        candidates << QDir(overrideDir).absolutePath();
    candidates << QDir(dataDirPath() + QStringLiteral("/branding")).absolutePath();
    if (QCoreApplication::instance())
        candidates << QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/branding")).absolutePath();
#ifdef Q_OS_LINUX
    candidates << QStringLiteral("/etc/kylinlist/branding");
#endif
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty()) continue;
        if (QFileInfo(candidate, kManifestName).isFile())
            return candidate;
    }
    return QString();
}

// 单个键无效只影响该键（保持默认），不阻止其余键生效，也不阻止启动。
void applyDisplayName(const QString &manifestPath, const QJsonValue &value)
{
    if (!value.isString()) {
        qWarning() << "[kylinlist] Branding: displayName 必须是字符串，保持默认:" << manifestPath;
        return;
    }
    const QString name = value.toString().trimmed();
    if (name.isEmpty()) {
        qWarning() << "[kylinlist] Branding: displayName 不能为空白，保持默认:" << manifestPath;
        return;
    }
    if (name.size() > kMaxDisplayNameLength) {
        qWarning() << "[kylinlist] Branding: displayName 超过" << kMaxDisplayNameLength
                   << "个字符，保持默认:" << manifestPath;
        return;
    }
    state().displayName = name;
}

void applyLogo(const QString &configDir, const QString &manifestPath, const QJsonValue &value)
{
    if (!value.isString()) {
        qWarning() << "[kylinlist] Branding: logo 必须是字符串，忽略自定义图标:" << manifestPath;
        return;
    }
    const QString logo = value.toString().trimmed();
    if (logo.isEmpty()) {
        qWarning() << "[kylinlist] Branding: logo 不能为空字符串，忽略自定义图标:" << manifestPath;
        return;
    }
    // 相对路径相对配置目录解析；也允许指向本机任意绝对路径。
    state().logoPath = QDir::isAbsolutePath(logo)
        ? QDir::cleanPath(logo)
        : QDir::cleanPath(configDir + QLatin1Char('/') + logo);
}

} // namespace

void load()
{
    State &s = state();
    // 先整体恢复默认，保证重复 load 总是反映当前配置。
    s.displayName = kDefaultDisplayName;
    s.configDir.clear();
    s.logoPath.clear();
    s.iconResolved = false;
    s.icon = QIcon();

    const QString configDir = resolveConfigDirectory();
    if (configDir.isEmpty()) return;  // 未部署品牌配置属正常情况，静默走默认
    s.configDir = configDir;

    const QString manifestPath = QDir(configDir).filePath(kManifestName);
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[kylinlist] Branding: 无法读取配置，使用默认品牌:" << manifestPath
                   << ":" << file.errorString();
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "[kylinlist] Branding: 配置不是合法 JSON，使用默认品牌:" << manifestPath
                   << ":" << parseError.errorString();
        return;
    }
    if (!doc.isObject()) {
        qWarning() << "[kylinlist] Branding: 配置根节点必须是 JSON 对象，使用默认品牌:" << manifestPath;
        return;
    }
    const QJsonObject object = doc.object();
    if (object.contains(kDisplayNameKey))
        applyDisplayName(manifestPath, object.value(kDisplayNameKey));
    if (object.contains(kLogoKey))
        applyLogo(configDir, manifestPath, object.value(kLogoKey));
}

QString displayName()
{
    return state().displayName;
}

QIcon appIcon()
{
    State &s = state();
    if (!s.iconResolved) {
        s.iconResolved = true;
        s.icon = QIcon();
        if (!s.logoPath.isEmpty()) {
            const QFileInfo info(s.logoPath);
            if (!info.isFile()) {
                qWarning() << "[kylinlist] Branding: logo 文件不存在，使用默认图标:" << s.logoPath;
            } else {
                // QIcon 对损坏文件不会主动报错，必须真实解码一次才能判断
                // 是否为可用图片（png/svg/ico 等交给 QIcon 引擎处理）。
                const QIcon candidate(info.absoluteFilePath());
                if (candidate.pixmap(QSize(32, 32)).isNull()) {
                    qWarning() << "[kylinlist] Branding: logo 无法解码为图标，使用默认图标:" << s.logoPath;
                } else {
                    s.icon = candidate;
                }
            }
        }
    }
    return s.icon;
}

QString configDirectory()
{
    return state().configDir;
}

QString logoPath()
{
    return state().logoPath;
}

} // namespace Branding
} // namespace kl
