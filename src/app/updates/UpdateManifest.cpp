#include "UpdateManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

namespace kl {
namespace updates {
namespace {

const QRegularExpression &versionPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
    return pattern;
}

bool isHex64(const QString &text)
{
    if (text.size() != 64) return false;
    for (const QChar ch : text) {
        const ushort value = ch.unicode();
        const bool ok = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        if (!ok) return false;
    }
    return true;
}

bool variantAllowed(const QString &platform, const QString &variant)
{
    if (platform == QStringLiteral("linux"))
        return variant == QStringLiteral("system-qt") || variant == QStringLiteral("bundled-qt");
    if (platform == QStringLiteral("windows"))
        return variant == QStringLiteral("setup") || variant == QStringLiteral("portable");
    return false;
}

} // namespace

const UpdateArtifact *UpdateManifest::findArtifact(const QString &platform, const QString &arch,
                                                   const QString &variant) const
{
    for (const UpdateArtifact &artifact : artifacts) {
        if (artifact.platform == platform && artifact.arch == arch && artifact.variant == variant)
            return &artifact;
    }
    return nullptr;
}

bool parseStrictVersion(const QString &text, VersionTriple *out)
{
    const QString trimmed = text.trimmed();
    if (!versionPattern().match(trimmed).hasMatch()) return false;
    const QStringList parts = trimmed.split(QLatin1Char('.'));
    bool ok[3] = {false, false, false};
    if (out) {
        out->major = parts.at(0).toInt(&ok[0]);
        out->minor = parts.at(1).toInt(&ok[1]);
        out->patch = parts.at(2).toInt(&ok[2]);
    } else {
        for (int i = 0; i < 3; ++i) parts.at(i).toInt(&ok[i]);
    }
    return ok[0] && ok[1] && ok[2];
}

int compareVersions(const VersionTriple &left, const VersionTriple &right)
{
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    return 0;
}

bool isValidArtifactPath(const QString &path, const QString &version)
{
    if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')))
        return false;
    const QString prefix = QStringLiteral("files/") + version + QStringLiteral("/");
    if (!path.startsWith(prefix)) return false;
    const QString rest = path.mid(prefix.size());
    if (rest.isEmpty() || rest.startsWith(QLatin1Char('/')) || rest.endsWith(QLatin1Char('/')))
        return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments)
        if (segment.isEmpty() || segment == QStringLiteral("..") || segment == QStringLiteral("."))
            return false;
    return segments.size() >= 4;  // files/<version>/<target>/<文件名>
}

UpdateManifest parseManifest(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return UpdateManifest();
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("更新清单不是有效的 JSON：%1").arg(parseError.errorString()));
    const QJsonObject object = document.object();
    UpdateManifest manifest;
    const int schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt(-1);
    if (schemaVersion != 1)
        return fail(QStringLiteral("更新清单协议版本 %1 不受支持").arg(schemaVersion));
    manifest.schemaVersion = schemaVersion;
    manifest.channel = object.value(QStringLiteral("channel")).toString();
    if (manifest.channel != QStringLiteral("stable"))
        return fail(QStringLiteral("更新清单通道 %1 不受支持").arg(manifest.channel));
    manifest.version = object.value(QStringLiteral("version")).toString().trimmed();
    if (!parseStrictVersion(manifest.version, nullptr))
        return fail(QStringLiteral("更新清单版本号 %1 不是有效的 X.Y.Z").arg(manifest.version));
    manifest.publishedAt = object.value(QStringLiteral("publishedAt")).toString().trimmed();
    if (manifest.publishedAt.isEmpty())
        return fail(QStringLiteral("更新清单缺少发布时间"));
    manifest.notes = object.value(QStringLiteral("notes")).toString();
    const QJsonValue artifactsValue = object.value(QStringLiteral("artifacts"));
    if (!artifactsValue.isArray())
        return fail(QStringLiteral("更新清单缺少 artifacts 数组"));
    const QJsonArray artifacts = artifactsValue.toArray();
    if (artifacts.isEmpty())
        return fail(QStringLiteral("更新清单 artifacts 为空"));
    QVector<QString> targets;
    for (int i = 0; i < artifacts.size(); ++i) {
        const QJsonValue value = artifacts.at(i);
        if (!value.isObject())
            return fail(QStringLiteral("artifacts[%1] 不是对象").arg(i));
        const QJsonObject item = value.toObject();
        UpdateArtifact artifact;
        artifact.platform = item.value(QStringLiteral("platform")).toString().toLower();
        artifact.arch = item.value(QStringLiteral("arch")).toString().toLower();
        artifact.variant = item.value(QStringLiteral("variant")).toString().toLower();
        artifact.packageVersion = item.value(QStringLiteral("packageVersion")).toString().trimmed();
        artifact.path = item.value(QStringLiteral("path")).toString().trimmed();
        const QString hash = item.value(QStringLiteral("sha256")).toString().toLower();
        const qint64 size = item.value(QStringLiteral("size")).toDouble(-1);
        if (artifact.platform != QStringLiteral("linux") && artifact.platform != QStringLiteral("windows"))
            return fail(QStringLiteral("artifacts[%1] 平台 %2 不受支持").arg(i).arg(artifact.platform));
        if (artifact.arch != QStringLiteral("arm64") && artifact.arch != QStringLiteral("amd64"))
            return fail(QStringLiteral("artifacts[%1] 架构 %2 不受支持").arg(i).arg(artifact.arch));
        if (!variantAllowed(artifact.platform, artifact.variant))
            return fail(QStringLiteral("artifacts[%1] 安装形式 %2 不受支持").arg(i).arg(artifact.variant));
        if (artifact.packageVersion != manifest.version)
            return fail(QStringLiteral("artifacts[%1] 包版本 %2 与清单版本 %3 不一致")
                            .arg(i).arg(artifact.packageVersion, manifest.version));
        if (!isValidArtifactPath(artifact.path, manifest.version))
            return fail(QStringLiteral("artifacts[%1] 文件路径 %2 不合法").arg(i).arg(artifact.path));
        if (!isHex64(hash))
            return fail(QStringLiteral("artifacts[%1] sha256 格式不正确").arg(i));
        if (size < 0)
            return fail(QStringLiteral("artifacts[%1] size 不正确").arg(i));
        artifact.sha256 = hash;
        artifact.size = size;
        const QString target = artifact.platform + QLatin1Char('-') + artifact.arch
            + QLatin1Char('-') + artifact.variant;
        if (targets.contains(target))
            return fail(QStringLiteral("artifacts[%1] 与其他条目重复：%2").arg(i).arg(target));
        targets.append(target);
        manifest.artifacts.append(artifact);
    }
    if (error) error->clear();
    return manifest;
}

} // namespace updates
} // namespace kl
