#include "StatsService.h"

#include "TaskStore.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace kl {
namespace {

// 与 server/stats/app.py 的解析上限保持一致：单计数最大 1e9，使用天数最大 100000。
const qint64 kCountMax = 1000000000LL;
const qint64 kFirstTaskDaysMax = 100000LL;

QString networkErrorText(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("找不到统计服务器主机");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("统计服务器拒绝了连接");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("统计服务器关闭了连接");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("连接统计服务器超时");
    default:
        return QStringLiteral("网络错误（%1）").arg(static_cast<int>(error));
    }
}

bool readCount(const QJsonValue &value, qint64 *out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number < 0 || number > 1e12) return false;
    *out = static_cast<qint64>(number);
    return true;
}

bool parseSummary(const QByteArray &body, StatsSummary *out, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("统计摘要不是有效的 JSON");
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonValue schema = root.value(QStringLiteral("schemaVersion"));
    if (!schema.isDouble() || schema.toDouble() != 1.0) {
        *error = QStringLiteral("统计摘要 schemaVersion 不受支持");
        return false;
    }
    StatsSummary summary;
    summary.schemaVersion = 1;
    const QJsonValue generatedAt = root.value(QStringLiteral("generatedAt"));
    if (!generatedAt.isString()) { *error = QStringLiteral("统计摘要缺少 generatedAt"); return false; }
    summary.generatedAt = generatedAt.toString();
    if (!readCount(root.value(QStringLiteral("users")), &summary.users)) {
        *error = QStringLiteral("统计摘要 users 数值非法");
        return false;
    }
    const QJsonObject usage = root.value(QStringLiteral("usage")).toObject();
    if (usage.isEmpty()) { *error = QStringLiteral("统计摘要缺少 usage 聚合"); return false; }
    if (!readCount(usage.value(QStringLiteral("tasksCreated")), &summary.tasksCreated)
        || !readCount(usage.value(QStringLiteral("notesCreated")), &summary.notesCreated)
        || !readCount(usage.value(QStringLiteral("tasksCompleted")), &summary.tasksCompleted)
        || !readCount(usage.value(QStringLiteral("longestUsageDays")), &summary.longestUsageDays)) {
        *error = QStringLiteral("统计摘要 usage 数值非法");
        return false;
    }
    const QJsonObject downloads = root.value(QStringLiteral("downloads")).toObject();
    if (downloads.isEmpty()) { *error = QStringLiteral("统计摘要缺少 downloads 聚合"); return false; }
    if (!readCount(downloads.value(QStringLiteral("total")), &summary.downloadsTotal)) {
        *error = QStringLiteral("统计摘要 downloads.total 非法");
        return false;
    }
    const QJsonValue byVersion = downloads.value(QStringLiteral("byVersion"));
    if (!byVersion.isArray()) { *error = QStringLiteral("统计摘要缺少 downloads.byVersion"); return false; }
    for (const QJsonValue &entryValue : byVersion.toArray()) {
        const QJsonObject entry = entryValue.toObject();
        const QJsonValue version = entry.value(QStringLiteral("version"));
        if (!version.isString() || version.toString().isEmpty()) {
            *error = QStringLiteral("统计摘要下载版本号非法");
            return false;
        }
        DownloadCount row;
        row.version = version.toString();
        if (!readCount(entry.value(QStringLiteral("count")), &row.count)) {
            *error = QStringLiteral("统计摘要下载数量非法");
            return false;
        }
        summary.downloadsByVersion.append(row);
    }
    *out = summary;
    return true;
}

} // namespace

StatsService::StatsService(TaskStore *store, QObject *parent)
    : QObject(parent), m_store(store), m_nam(new QNetworkAccessManager(this))
{
    m_autoTimer.setInterval(m_reportIntervalMs);
    connect(&m_autoTimer, &QTimer::timeout, this, [this]() {
        // 上报在途时跳过本次周期，等下一小时再试，绝不并发重叠。
        if (m_baseUrl.isEmpty() || m_reportReply) return;
        reportNow();
    });
}

StatsService::~StatsService()
{
    if (m_reportReply) m_reportReply->abort();
    if (m_summaryReply) m_summaryReply->abort();
}

bool StatsService::fetchingSummary() const
{
    return m_summaryReply != nullptr;
}

void StatsService::setState(State state, const QString &error)
{
    m_state = state;
    m_error = error;
    emit stateChanged();
}

void StatsService::setBaseUrl(const QString &baseUrl)
{
    const QString trimmed = baseUrl.trimmed();
    stopAutomaticReports();
    if (trimmed.isEmpty()) {
        m_baseUrl.clear();
        m_base = QUrl();
        setState(State::Disabled);
        return;
    }
    QUrl url(trimmed, QUrl::TolerantMode);
    const QString parsedScheme = url.scheme();
    // 无协议（QUrl 当作纯路径，如 127.0.0.1:8000/x）或形如 "localhost:8000" 的写法：默认补 http://。
    // 已写明 http/https 但主机为空（如 "http://"）的输入不补，交给最终校验按无效处理。
    if (!url.isValid() || parsedScheme.isEmpty()
        || (url.host().isEmpty() && parsedScheme != QStringLiteral("http")
            && parsedScheme != QStringLiteral("https"))) {
        url = QUrl(QStringLiteral("http://") + trimmed, QUrl::TolerantMode);
    }
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https"))
        || url.host().isEmpty()) {
        m_baseUrl.clear();
        m_base = QUrl();
        setState(State::Disabled, QStringLiteral("统计服务器地址无效，应形如 http://主机:端口"));
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

void StatsService::setReportTiming(int firstDelayMs, int intervalMs)
{
    m_firstReportDelayMs = firstDelayMs;
    m_reportIntervalMs = intervalMs;
    m_autoTimer.setInterval(m_reportIntervalMs);
}

void StatsService::startAutomaticReports()
{
    if (m_baseUrl.isEmpty() || !m_store) return;
    if (!m_autoTimer.isActive()) m_autoTimer.start();
    if (m_firstReportPending) return;  // 已安排的首报不重复排队
    m_firstReportPending = true;
    QTimer::singleShot(m_firstReportDelayMs, this, [this]() {
        m_firstReportPending = false;
        // 期间被 stop()/setBaseUrl 禁用或已有请求在途则跳过。
        if (m_baseUrl.isEmpty() || !m_autoTimer.isActive() || m_reportReply) return;
        reportNow();
    });
}

void StatsService::stopAutomaticReports()
{
    m_autoTimer.stop();
}

QJsonObject StatsService::buildUsagePayload(const UsageStatistics &stats, const QDateTime &now)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("schemaVersion"), static_cast<double>(1));
    // 服务器按 double 解析 JSON 数字；本地计数远小于 2^53，无精度问题。
    payload.insert(QStringLiteral("tasksCreated"),
                   static_cast<double>(qBound<qint64>(0, stats.tasksCreated, kCountMax)));
    payload.insert(QStringLiteral("notesCreated"),
                   static_cast<double>(qBound<qint64>(0, stats.notesCreated, kCountMax)));
    payload.insert(QStringLiteral("tasksCompleted"),
                   static_cast<double>(qBound<qint64>(0, stats.tasksCompleted, kCountMax)));
    if (stats.hasFirstTask() && now.isValid())
        payload.insert(QStringLiteral("firstTaskDays"),
                       static_cast<double>(qBound<qint64>(0, stats.firstTaskDays(now), kFirstTaskDaysMax)));
    else
        payload.insert(QStringLiteral("firstTaskDays"), QJsonValue(QJsonValue::Null));
    return payload;
}

void StatsService::reportNow()
{
    if (m_reportReply) return;  // 在途请求不重复上报
    if (m_baseUrl.isEmpty() || !m_base.isValid()) {
        m_error = QStringLiteral("未配置统计服务器地址");
        emit reportFinished(false);
        return;
    }
    QString statsError;
    const UsageStatistics stats = m_store ? m_store->usageStatistics(&statsError) : UsageStatistics();
    if (!statsError.isEmpty()) {
        setState(State::Failed, QStringLiteral("读取本地统计失败：%1").arg(statsError));
        emit reportFinished(false);
        return;
    }
    const QByteArray body = QJsonDocument(buildUsagePayload(stats, utcNow()))
                                .toJson(QJsonDocument::Compact);
    setState(State::Reporting);
    QNetworkRequest request(m_base.resolved(QUrl(QStringLiteral("api/v1/stats/usage"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("User-Agent", "KylinList-Stats");
    QNetworkReply *reply = m_nam->post(request, body);
    m_reportReply = reply;
    const QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(m_requestTimeoutMs, this, [this, guard]() {
        if (guard.data() && guard.data() == m_reportReply.data() && guard->isRunning()) {
            guard->setProperty("abortReason", QStringLiteral("上报统计超时"));
            guard->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { finishReport(reply); });
}

void StatsService::finishReport(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_reportReply.data() != reply) return;
    m_reportReply.clear();
    const QString reason = reply->property("abortReason").toString();
    if (reply->error() != QNetworkReply::NoError) {
        // QNAM 对 4xx/5xx 也设置 error()，优先用状态码描述。
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString message;
        if (status >= 400)
            message = QStringLiteral("上报统计失败（HTTP %1）").arg(status);
        else
            message = reason.isEmpty() ? networkErrorText(reply->error()) : reason;
        setState(State::Failed, message);
        emit reportFinished(false);
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        setState(State::Failed, QStringLiteral("上报统计失败（HTTP %1）").arg(status));
        emit reportFinished(false);
        return;
    }
    m_lastReportAt = utcNow();
    setState(State::Reported);
    emit reportFinished(true);
}

void StatsService::fetchSummary()
{
    if (m_summaryReply) return;
    if (m_baseUrl.isEmpty() || !m_base.isValid()) {
        m_summaryError = QStringLiteral("未配置统计服务器地址");
        emit summaryChanged();
        return;
    }
    QNetworkRequest request(m_base.resolved(QUrl(QStringLiteral("api/v1/stats/summary"))));
    request.setRawHeader("User-Agent", "KylinList-Stats");
    QNetworkReply *reply = m_nam->get(request);
    m_summaryReply = reply;
    const QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(m_requestTimeoutMs, this, [this, guard]() {
        if (guard.data() && guard.data() == m_summaryReply.data() && guard->isRunning()) {
            guard->setProperty("abortReason", QStringLiteral("获取统计摘要超时"));
            guard->abort();
        }
    });
    // 响应必须小于 1MiB：接近上限就主动中断，不把大响应整个读进内存。
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (m_summaryReply.data() != reply) return;
        if (reply->bytesAvailable() > SummaryMaxBytes) {
            reply->setProperty("abortReason", QStringLiteral("统计摘要过大，已拒绝处理"));
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { finishSummary(reply); });
}

void StatsService::finishSummary(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_summaryReply.data() != reply) return;
    m_summaryReply.clear();
    // 摘要拉取失败只影响页面展示，不改上报状态，也不影响本地统计。
    const QString reason = reply->property("abortReason").toString();
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400)
            m_summaryError = QStringLiteral("统计服务器没有返回摘要（HTTP %1）").arg(status);
        else
            m_summaryError = reason.isEmpty() ? networkErrorText(reply->error()) : reason;
        emit summaryChanged();
        return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 200) {
        m_summaryError = QStringLiteral("统计服务器没有返回摘要（HTTP %1）").arg(status);
        emit summaryChanged();
        return;
    }
    const QByteArray body = reply->readAll();
    if (body.size() > SummaryMaxBytes) {
        m_summaryError = QStringLiteral("统计摘要过大，已拒绝处理");
        emit summaryChanged();
        return;
    }
    StatsSummary summary;
    QString parseError;
    if (!parseSummary(body, &summary, &parseError)) {
        m_summaryError = parseError;
        emit summaryChanged();
        return;
    }
    m_summary = summary;
    m_summaryError.clear();
    emit summaryChanged();
}

} // namespace kl
