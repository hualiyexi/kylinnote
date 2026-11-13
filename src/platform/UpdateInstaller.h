#ifndef KYLINLIST_UPDATE_INSTALLER_H
#define KYLINLIST_UPDATE_INSTALLER_H

#include <QString>

namespace kl {
namespace updates {

// 安装前再次核对本地安装包：字节数与 SHA-256 都一致才允许交接。
bool verifyInstallerFile(const QString &filePath, qint64 expectedSize, const QString &expectedSha256,
                         QString *error = nullptr);

// 把已校验的安装包交给系统安装流程。返回 true 只表示“已成功交给安装器”，
// 不代表安装完成；结果由下次启动时的实际版本确认。
// 测试钩子：设置 KYLINLIST_UPDATE_FAKE_INSTALLER 为脚本路径时改用该脚本执行交接。
bool handOffToInstaller(const QString &filePath, QString *error = nullptr);

} // namespace updates
} // namespace kl

#endif // KYLINLIST_UPDATE_INSTALLER_H
