#ifndef KYLINLIST_UPDATE_MANIFEST_H
#define KYLINLIST_UPDATE_MANIFEST_H

#include <QByteArray>
#include <QString>
#include <QVector>

namespace kl {
namespace updates {

// 更新清单协议 v1（纯 HTTP 内网发布；SHA-256 仅用于下载完整性）。
// artifact.path 相对于“服务器基础地址 + /updates/”，
// 例如 files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb。
struct UpdateArtifact {
    QString platform;       // linux / windows
    QString arch;           // arm64 / amd64
    QString variant;        // system-qt / bundled-qt / setup / portable
    QString packageVersion; // 协议要求与清单 version 相同
    QString path;
    QString sha256;         // 64 位小写 hex
    qint64 size = -1;       // 字节数
};

struct UpdateManifest {
    int schemaVersion = 0;
    QString channel;
    QString version;
    QString publishedAt;
    QString notes;
    QVector<UpdateArtifact> artifacts;

    const UpdateArtifact *findArtifact(const QString &platform, const QString &arch,
                                       const QString &variant) const;
};

// 正式版本只接受三段 X.Y.Z（各段无前导零，纯数字）。
struct VersionTriple {
    int major = 0, minor = 0, patch = 0;
};
bool parseStrictVersion(const QString &text, VersionTriple *out);
int compareVersions(const VersionTriple &left, const VersionTriple &right);  // -1 / 0 / 1

// 解析并完整校验清单。任何结构错误都会返回空清单并给出中文原因；
// 未知字段忽略，schemaVersion 不同则拒绝。
UpdateManifest parseManifest(const QByteArray &json, QString *error = nullptr);

// path 合法性：必须是 files/<版本>/<目录>/<文件> 形式的相对路径，禁止 .. 与绝对路径。
bool isValidArtifactPath(const QString &path, const QString &version);

} // namespace updates
} // namespace kl

#endif // KYLINLIST_UPDATE_MANIFEST_H
