#ifndef KYLINLIST_STATSSERVICE_H
#define KYLINLIST_STATSSERVICE_H

#include "UsageStatistics.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace kl {

class TaskStore;

// GET api/v1/stats/summary 里 downloads.byVersion 的一行。
struct DownloadCount {
    QString version;
    qint64 count = 0;
};

// 服务器聚合摘要的只读快照；字段契约见 server/stats/app.py（schemaVersion 1）。
struct StatsSummary {
    int schemaVersion = 0;
    QString generatedAt;
    qint64 users = 0;
    qint64 tasksCreated = 0;
    qint64 notesCreated = 0;
    qint64 tasksCompleted = 0;
    qint64 longestUsageDays = 0;
    qint64 downloadsTotal = 0;
    QVector<DownloadCount> downloadsByVersion;
};

// 匿名使用统计客户端：定期把本机累计计数（不含任何 id/标题/正文/路径）
// POST 到 base + api/v1/stats/usage，并按需拉取聚合摘要。
// 网络失败只保存 errorText 供页面查看，绝不影响本地数据；
// 未配置地址时服务处于 Disabled，任何请求都不会发出。
class StatsService : public QObject {
    Q_OBJECT
public:
    enum class State {
        Disabled,   // 未配置服务器地址
        Idle,       // 已配置，尚无上报结果
        Reporting,  // 上报请求在途
        Reported,   // 最近一次上报成功
        Failed      // 最近一次上报失败（errorText 保存原因）
    };

    explicit StatsService(TaskStore *store, QObject *parent = nullptr);
    ~StatsService() override;

    void setBaseUrl(const QString &baseUrl);  // 空字符串表示禁用；无协议时按 http:// 处理
    QString baseUrl() const { return m_baseUrl; }
    bool enabled() const { return m_state != State::Disabled; }

    // 测试注入点；生产使用默认值（请求 10 秒超时，启动 30 秒后首报，每 1 小时再报）。
    void setRequestTimeoutMs(int ms) { m_requestTimeoutMs = ms; }
    void setReportTiming(int firstDelayMs, int intervalMs);

    void startAutomaticReports();
    void stopAutomaticReports();
    bool automaticReportsActive() const { return m_autoTimer.isActive(); }

    State state() const { return m_state; }
    QString errorText() const { return m_error; }        // 最近一次上报失败原因
    QDateTime lastReportAt() const { return m_lastReportAt; }

    const StatsSummary &summary() const { return m_summary; }
    bool hasSummary() const { return m_summary.schemaVersion > 0; }
    QString summaryError() const { return m_summaryError; }
    bool fetchingSummary() const;   // 实现在 .cpp：避免头文件里需要完整 QNetworkReply 类型

    // 组装上报 JSON：白名单字段 schemaVersion/tasksCreated/notesCreated/
    // tasksCompleted/firstTaskDays（int 或 null）。测试直接用它校验不含标题/id。
    static QJsonObject buildUsagePayload(const UsageStatistics &stats, const QDateTime &now);

public slots:
    void reportNow();     // 立即上报；在途请求不重复发送
    void fetchSummary();  // 拉取聚合摘要；失败只写 summaryError

signals:
    void stateChanged();
    void reportFinished(bool success);
    void summaryChanged();

private:
    void setState(State state, const QString &error = QString());
    void finishReport(QNetworkReply *reply);
    void finishSummary(QNetworkReply *reply);

    TaskStore *m_store = nullptr;
    QString m_baseUrl;
    QUrl m_base;
    State m_state = State::Disabled;
    QString m_error;
    QString m_summaryError;
    QDateTime m_lastReportAt;
    StatsSummary m_summary;
    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reportReply;
    QPointer<QNetworkReply> m_summaryReply;
    QTimer m_autoTimer;
    bool m_firstReportPending = false;
    int m_requestTimeoutMs = 10000;
    int m_firstReportDelayMs = 30000;
    int m_reportIntervalMs = 60 * 60 * 1000;
    static const int SummaryMaxBytes = 1024 * 1024;
};

} // namespace kl

#endif // KYLINLIST_STATSSERVICE_H
