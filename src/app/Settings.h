#pragma once
#include <QDate>
#include <QDateTime>
#include <QSettings>
#include "../Common.h"
namespace kl {
class Settings {
public:
    struct PendingInstall {
        bool valid = false;
        QString version, sha256, platform, arch, variant, fileName;
    };
    static Settings& instance();
    NoteLayer defaultLayer() const;
    void setDefaultLayer(NoteLayer layer);

    // 内网更新（纯 HTTP）：基础地址留空表示禁用检查。
    // 键不存在时（全新安装）返回 defaultUpdateBaseUrl()；
    // 用户显式保存过空串则仍表示禁用。
    static QString defaultUpdateBaseUrl();
    QString updateBaseUrl() const;
    void setUpdateBaseUrl(const QString& url);
    bool updateAutoCheckEnabled() const;
    void setUpdateAutoCheckEnabled(bool enabled);
    QDateTime lastUpdateCheckAt() const;
    void setLastUpdateCheckAt(const QDateTime& at);
    // 自动弹窗的暂缓截止时刻（UTC，由 appNow() 产生）；无效/过期表示未暂缓。
    QDateTime updatePostponeUntil() const;
    void setUpdatePostponeUntil(const QDateTime& at);
    // 默认自启的一次性初始化标记：仅在成功安装或已安装后落盘，
    // 之后用户关闭自启不会被复活。
    bool autostartInitialized() const;
    void setAutostartInitialized(bool done);
    QString lastUpdateNoticeVersion() const;
    void setLastUpdateNoticeVersion(const QString& version);
    QDate lastUpdateNoticeDate() const;
    void setLastUpdateNoticeDate(const QDate& date);

    // 待安装记录：安装器交接后写入，下次启动按实际版本确认结果。
    PendingInstall pendingInstall() const;
    void setPendingInstall(const PendingInstall& pending);
    void clearPendingInstall();

    void sync();

private:
    Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    QSettings m_store;
};
}  // namespace kl
