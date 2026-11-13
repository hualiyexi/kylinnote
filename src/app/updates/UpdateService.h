#ifndef KYLINLIST_UPDATE_SERVICE_H
#define KYLINLIST_UPDATE_SERVICE_H

#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "InstallIdentity.h"
#include "UpdateManifest.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace kl {
namespace updates {

// 下次启动时对“待安装记录”的判定。安装器交接成功不等于安装成功，
// 由启动时的实际版本决定结果。
enum class PendingOutcome { None, Completed, StillPending, Superseded };
PendingOutcome evaluatePendingInstall(const QString &appVersion, const QString &pendingVersion);

// 更新客户端服务：检查清单、下载安装包、校验完整性并暴露状态。
// 网络不可用只影响更新，不影响本地数据；所有请求都是普通 HTTP。
class UpdateService : public QObject {
    Q_OBJECT
public:
    enum class State {
        Disabled,        // 未配置服务器地址（或身份不可用）
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading,
        Ready,           // 安装包已下载并校验通过
        Failed
    };

    explicit UpdateService(InstallIdentity identity, QObject *parent = nullptr);
    ~UpdateService() override;

    void setBaseUrl(const QString &baseUrl);  // 空字符串表示禁用；无协议时按 http:// 处理
    QString baseUrl() const { return m_baseUrl; }
    bool enabled() const { return m_state != State::Disabled; }
    bool identityValid() const { return m_identity.isValid(); }
    QString identityError() const { return m_identityError; }

    // 测试注入点；生产使用默认值。
    void setRequestTimeoutMs(int ms) { m_requestTimeoutMs = ms; }
    void setStallTimeoutMs(int ms) { m_stallTimeoutMs = ms; }

    void startAutomaticChecks();   // 启动后延时首查 + 周期复查
    void stopAutomaticChecks();

    State state() const { return m_state; }
    QString errorText() const { return m_error; }
    const UpdateManifest &manifest() const { return m_manifest; }
    const UpdateArtifact *targetArtifact() const;
    QString verifiedFilePath() const { return m_verifiedFilePath; }

public slots:
    void checkNow();
    void download();
    void cancelDownload();

signals:
    void stateChanged();
    void checkFinished(bool updateAvailable, const QString &message);
    void downloadProgress(qint64 received, qint64 total);
    void downloadSucceeded(const QString &filePath);
    void downloadFailed(const QString &error);

private:
    void setState(State state, const QString &error = QString());
    void finishCheck(const QByteArray &body);
    void startStallWatchdog();
    void abortDownload(const QString &reason);
    QString cacheFilePath(const UpdateArtifact &artifact) const;
    QString cacheDirPath() const;

    InstallIdentity m_identity;
    QString m_identityError;
    QString m_baseUrl;
    QUrl m_base;
    State m_state = State::Disabled;
    QString m_error;
    UpdateManifest m_manifest;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_checkReply;
    QPointer<QNetworkReply> m_downloadReply;
    QTimer m_autoTimer;
    QTimer m_stallTimer;
    QFile m_downloadFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_verifiedFilePath;
    qint64 m_downloadedBytes = 0;
    int m_requestTimeoutMs = 10000;
    int m_stallTimeoutMs = 30000;
    static const int ManifestMaxBytes = 1024 * 1024;
};

} // namespace updates
} // namespace kl

#endif // KYLINLIST_UPDATE_SERVICE_H
