#include "UpdateService.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "../../Common.h"

namespace kl {
namespace updates {
namespace {

const int AutoCheckIntervalMs = 6 * 60 * 60 * 1000;  // 6 小时
const int FirstCheckDelayMs = 10 * 1000;

QString networkErrorText(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到更新服务器主机");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("更新服务器拒绝了连接");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("更新服务器关闭了连接");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("连接更新服务器超时");
    case QNetworkReply::AuthenticationRequiredError:
        return QStringLiteral("更新服务器要求认证");
    default:
        return QStringLiteral("网络错误（%1）").arg(static_cast<int>(error));
    }
}

} // namespace

PendingOutcome evaluatePendingInstall(const QString &appVersion, const QString &pendingVersion)
{
    if (pendingVersion.trimmed().isEmpty()) return PendingOutcome::None;
    VersionTriple app, pending;
    if (!parseStrictVersion(appVersion, &app) || !parseStrictVersion(pendingVersion, &pending))
        return PendingOutcome::None;
    const int order = compareVersions(app, pending);
    if (order == 0) return PendingOutcome::Completed;
    return order > 0 ? PendingOutcome::Superseded : PendingOutcome::StillPending;
}

UpdateService::UpdateService(InstallIdentity identity, QObject *parent)
    : QObject(parent), m_identity(identity), m_nam(new QNetworkAccessManager(this))
{
    if (!m_identity.isValid())
        m_identityError = QStringLiteral("当前安装缺少有效的 install-info.json 安装身份");
    m_autoTimer.setInterval(AutoCheckIntervalMs);
    m_stallTimer.setSingleShot(true);
    connect(&m_autoTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Checking || m_state == State::Downloading || m_state == State::Ready)
            return;
        checkNow();
    });
    connect(&m_stallTimer, &QTimer::timeout, this, [this]() {
        if (m_downloadReply && m_downloadReply->isRunning())
            abortDownload(QStringLiteral("下载长时间没有进展，已停止"));
    });
}

UpdateService::~UpdateService()
{
    if (m_checkReply) m_checkReply->abort();
    if (m_downloadReply) m_downloadReply->abort();
    if (m_downloadFile.isOpen()) m_downloadFile.close();
}

void UpdateService::setState(State state, const QString &error)
{
    m_state = state;
    m_error = error;
    emit stateChanged();
}

void UpdateService::setBaseUrl(const QString &baseUrl)
{
    const QString trimmed = baseUrl.trimmed();
    stopAutomaticChecks();
    if (trimmed.isEmpty()) {
        m_baseUrl.clear();
        m_base = QUrl();
        setState(State::Disabled);
        return;
    }
    QUrl url(trimmed, QUrl::TolerantMode);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
        url = QUrl(QStringLiteral("http://") + trimmed, QUrl::TolerantMode);
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty()) {
        m_baseUrl.clear();
        m_base = QUrl();
        setState(State::Disabled, QStringLiteral("更新服务器地址无效，应形如 http://主机:端口"));
        return;
    }
    QString path = url.path();
    if (path.isEmpty()) path = QStringLiteral("/");
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    url.setPath(path);
    m_baseUrl = url.toString();
    m_base = url;
    setState(State::Idle);
}

void UpdateService::startAutomaticChecks()
{
    if (m_baseUrl.isEmpty() || !m_identity.isValid()) return;
    if (!m_autoTimer.isActive()) m_autoTimer.start();
    QTimer::singleShot(FirstCheckDelayMs, this, [this]() {
        if (m_baseUrl.isEmpty() || !m_identity.isValid()) return;
        if (m_state == State::Checking || m_state == State::Downloading) return;
        checkNow();
    });
}

void UpdateService::stopAutomaticChecks()
{
    m_autoTimer.stop();
}

const UpdateArtifact *UpdateService::targetArtifact() const
{
    return m_manifest.findArtifact(m_identity.platform, m_identity.arch, m_identity.variant);
}

QString UpdateService::cacheDirPath() const
{
    return dataDirPath() + QStringLiteral("/updates/cache");
}

QString UpdateService::cacheFilePath(const UpdateArtifact &artifact) const
{
    QString suffix;
    const int dot = artifact.path.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) suffix = artifact.path.mid(dot);
    return cacheDirPath() + QStringLiteral("/kylinlist_%1_%2_%3%4")
        .arg(m_manifest.version, m_identity.arch, m_identity.variant, suffix);
}

void UpdateService::checkNow()
{
    if (m_state == State::Downloading) {
        emit checkFinished(false, QStringLiteral("正在下载更新，请稍后再检查"));
        return;
    }
    if (m_baseUrl.isEmpty()) {
        emit checkFinished(false, QStringLiteral("未配置更新服务器地址"));
        return;
    }
    if (!m_identity.isValid()) {
        setState(State::Failed, QStringLiteral("无法确定安装身份：%1").arg(m_identityError));
        emit checkFinished(false, m_error);
        return;
    }
    if (m_checkReply) return;
    m_verifiedFilePath.clear();
    setState(State::Checking);
    QNetworkRequest request(m_base.resolved(QUrl(QStringLiteral("updates/v1/stable/manifest.json"))));
    request.setRawHeader("User-Agent", "KylinList-Update");
    QNetworkReply *reply = m_nam->get(request);
    m_checkReply = reply;
    const QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(m_requestTimeoutMs, this, [this, guard]() {
        if (guard.data() && guard.data() == m_checkReply.data() && guard->isRunning()) {
            guard->setProperty("abortReason", QStringLiteral("检查更新超时"));
            guard->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_checkReply.data() != reply) return;
        m_checkReply.clear();
        const QString reason = reply->property("abortReason").toString();
        if (reply->error() != QNetworkReply::NoError) {
            // QNAM 对 4xx/5xx 也会设置 error()，此时优先用状态码描述。
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString message;
            if (status >= 400)
                message = QStringLiteral("服务器没有返回更新清单（HTTP %1）").arg(status);
            else
                message = reason.isEmpty() ? networkErrorText(reply->error()) : reason;
            setState(State::Failed, message);
            emit checkFinished(false, message);
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status != 200) {
            const QString message = QStringLiteral("服务器没有返回更新清单（HTTP %1）").arg(status);
            setState(State::Failed, message);
            emit checkFinished(false, message);
            return;
        }
        if (body.size() > ManifestMaxBytes) {
            const QString message = QStringLiteral("更新清单过大，已拒绝处理");
            setState(State::Failed, message);
            emit checkFinished(false, message);
            return;
        }
        finishCheck(body);
    });
}

void UpdateService::finishCheck(const QByteArray &body)
{
    QString error;
    const UpdateManifest manifest = parseManifest(body, &error);
    if (!error.isEmpty() || manifest.artifacts.isEmpty()) {
        const QString message = error.isEmpty()
            ? QStringLiteral("更新清单没有任何安装包") : error;
        setState(State::Failed, message);
        emit checkFinished(false, message);
        return;
    }
    m_manifest = manifest;
    const UpdateArtifact *artifact = targetArtifact();
    if (!artifact) {
        const QString message = QStringLiteral("服务器没有提供与当前安装匹配的更新包（%1）")
                                     .arg(m_identity.targetKey());
        setState(State::Failed, message);
        emit checkFinished(false, message);
        return;
    }
    VersionTriple current, remote;
    parseStrictVersion(m_identity.version, &current);
    parseStrictVersion(m_manifest.version, &remote);
    if (compareVersions(remote, current) > 0) {
        setState(State::UpdateAvailable);
        const QString message = QStringLiteral("发现新版本 %1（当前 %2）")
                                    .arg(m_manifest.version, m_identity.version);
        emit checkFinished(true, message);
    } else {
        setState(State::UpToDate);
        emit checkFinished(false, QStringLiteral("已是最新版本 %1").arg(m_identity.version));
    }
}

void UpdateService::download()
{
    if (m_state != State::UpdateAvailable || m_downloadReply) return;
    const UpdateArtifact *artifact = targetArtifact();
    if (!artifact) return;
    QDir().mkpath(cacheDirPath());
    const QString finalPath = cacheFilePath(*artifact);
    const QString partPath = finalPath + QStringLiteral(".part");
    if (QFile::exists(finalPath)) QFile::remove(finalPath);
    QFile::remove(partPath);
    m_downloadFile.setFileName(partPath);
    if (!m_downloadFile.open(QIODevice::WriteOnly)) {
        m_error = QStringLiteral("无法写入更新缓存：%1").arg(partPath);
        emit downloadFailed(m_error);
        return;
    }
    m_hash.reset();
    m_downloadedBytes = 0;
    setState(State::Downloading);
    QUrl url = m_base.resolved(QUrl(QStringLiteral("updates/") + artifact->path));
    // 防御：解析后的路径必须仍在自己的 updates/ 前缀下。
    QString basePrefix = m_base.path();
    if (!basePrefix.endsWith(QLatin1Char('/'))) basePrefix += QLatin1Char('/');
    if (!url.path().startsWith(basePrefix + QStringLiteral("updates/"))
        || !(url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        m_downloadFile.close();
        QFile::remove(partPath);
        setState(State::UpdateAvailable, QStringLiteral("更新包地址不合法，已拒绝下载"));
        emit downloadFailed(m_error);
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "KylinList-Update");
    QNetworkReply *reply = m_nam->get(request);
    m_downloadReply = reply;
    startStallWatchdog();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, artifact]() {
        if (m_downloadReply.data() != reply) return;
        const QByteArray chunk = reply->readAll();
        if (m_downloadFile.write(chunk) != chunk.size()) {
            abortDownload(QStringLiteral("写入更新缓存失败"));
            return;
        }
        m_hash.addData(chunk);
        m_downloadedBytes += chunk.size();
        startStallWatchdog();
        emit downloadProgress(m_downloadedBytes, artifact->size);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, artifact, finalPath, partPath]() {
        reply->deleteLater();
        if (m_downloadReply.data() != reply) return;
        m_downloadReply.clear();
        m_stallTimer.stop();
        const QString reason = reply->property("abortReason").toString();
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString message;
            if (status >= 400)
                message = QStringLiteral("下载更新失败（HTTP %1）").arg(status);
            else
                message = reason.isEmpty() ? networkErrorText(reply->error()) : reason;
            m_downloadFile.close();
            QFile::remove(partPath);
            setState(State::UpdateAvailable, message);
            emit downloadFailed(message);
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            m_downloadFile.close();
            QFile::remove(partPath);
            const QString message = QStringLiteral("下载更新失败（HTTP %1）").arg(status);
            setState(State::UpdateAvailable, message);
            emit downloadFailed(message);
            return;
        }
        // 结束前可能有未读数据。
        const QByteArray rest = reply->readAll();
        if (!rest.isEmpty()) {
            m_downloadFile.write(rest);
            m_hash.addData(rest);
            m_downloadedBytes += rest.size();
            emit downloadProgress(m_downloadedBytes, artifact->size);
        }
        m_downloadFile.close();
        QString message;
        if (m_downloadedBytes != artifact->size)
            message = QStringLiteral("安装包大小不符（预期 %1 字节，收到 %2 字节）")
                          .arg(artifact->size).arg(m_downloadedBytes);
        else if (m_hash.result().toHex() != artifact->sha256.toLatin1())
            message = QStringLiteral("安装包校验失败，已丢弃下载内容");
        if (!message.isEmpty()) {
            QFile::remove(partPath);
            setState(State::UpdateAvailable, message);
            emit downloadFailed(message);
            return;
        }
        if (QFile::exists(finalPath)) QFile::remove(finalPath);
        if (!QFile::rename(partPath, finalPath)) {
            QFile::remove(partPath);
            setState(State::UpdateAvailable, QStringLiteral("保存安装包失败"));
            emit downloadFailed(m_error);
            return;
        }
        m_verifiedFilePath = finalPath;
        setState(State::Ready);
        emit downloadSucceeded(finalPath);
    });
}

void UpdateService::cancelDownload()
{
    if (!m_downloadReply) return;
    abortDownload(QStringLiteral("下载已取消"));
}

void UpdateService::startStallWatchdog()
{
    m_stallTimer.start(m_stallTimeoutMs);
}

void UpdateService::abortDownload(const QString &reason)
{
    if (!m_downloadReply) return;
    m_downloadReply->setProperty("abortReason", reason);
    m_downloadReply->abort();
}

} // namespace updates
} // namespace kl
