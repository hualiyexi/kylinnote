#ifndef KYLINLIST_INSTALL_IDENTITY_H
#define KYLINLIST_INSTALL_IDENTITY_H

#include <QString>
#include <QVector>

namespace kl {
namespace updates {

// 安装身份由打包阶段写入 install-info.json，与数据库里的 installation_id 无关。
// 客户端用它选择与自己平台、架构、安装形式完全匹配的更新包。
struct InstallIdentity {
    QString product;   // 固定为 kylinlist
    QString platform;  // linux / windows
    QString version;   // 必须等于程序内置版本
    QString arch;      // arm64 / amd64
    QString variant;   // linux: system-qt|bundled-qt；windows: setup|portable

    bool isValid() const;
    QString targetKey() const;  // "<platform>-<arch>-<variant>"，即发布目录名
};

QString currentBuildPlatform();
QString currentBuildArch();
QString currentBuildVariantChoices();  // 调试与提示用：当前平台允许的 variant 列表

// 解析 install-info.json 内容；不校验与当前程序的匹配关系，便于测试。
InstallIdentity parseInstallInfoJson(const QByteArray &json, QString *error = nullptr);

// 从本机安装位置读取并校验安装身份。测试可用 KYLINLIST_UPDATE_INSTALL_INFO_PATH
// 覆盖读取位置；开发构建缺失该文件时返回无效身份，并说明原因。
InstallIdentity readInstallIdentity(QString *error = nullptr);

} // namespace updates
} // namespace kl

#endif // KYLINLIST_INSTALL_IDENTITY_H
