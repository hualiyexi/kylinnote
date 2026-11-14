#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <memory>

#include "Common.h"
#include "app/StatsService.h"
#include "app/TaskStore.h"
#include "app/UsageStatistics.h"
#include "db/Database.h"
#include "db/TaskRepository.h"

using namespace kl;

namespace {

// 等待条件成立或超时（全部请求都指向本机夹具，绝不连真实服务器）。
bool waitFor(int timeoutMs, const std::function<bool()> &condition)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (condition()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qSleep(10);
    }
    return condition();
}

// 极简本地 HTTP 夹具：按路径返回预设响应，支持挂起、断连、延迟与请求捕获。
// 只监听 127.0.0.1，请求体按 Content-Length 读完再应答，便于校验 POST 载荷。
class FixtureHttpServer : public QObject {
    Q_OBJECT
public:
    struct Response {
        int status = 200;
        QByteArray body;
        int delayMs = 0;
        bool hang = false;              // 收到请求后不回应
        bool abortConnection = false;   // 立即断开
    };
    struct Request {
        QString method;
        QString path;
        QByteArray body;
        QByteArray contentType;
    };

    bool start()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) takeRequest(m_server.nextPendingConnection());
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
    int serverPort() const { return m_server.serverPort(); }
    void setRoute(const QString &path, const Response &response) { m_routes.insert(path, response); }
    const QList<Request> &requests() const { return m_requests; }
    void clearRequests() { m_requests.clear(); }
    int requestCount(const QString &path) const
    {
        int count = 0;
        for (const Request &request : m_requests) if (request.path == path) ++count;
        return count;
    }
    bool waitForRequests(int count, int timeoutMs = 5000)
    {
        return waitFor(timeoutMs, [this, count]() { return m_requests.size() >= count; });
    }

private:
    struct RequestState {
        QByteArray buffer;
        bool headersParsed = false;
        QString method;
        QString path;
        QByteArray contentType;
        int contentLength = 0;
    };

    void takeRequest(QTcpSocket *socket)
    {
        socket->setParent(this);
        auto *state = new RequestState();
        connect(socket, &QObject::destroyed, socket, [state]() { delete state; });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, state]() {
            state->buffer.append(socket->readAll());
            const int headerEnd = state->buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) return;
            if (!state->headersParsed) {
                state->headersParsed = true;
                const QList<QByteArray> lines = state->buffer.left(headerEnd).split('\n');
                const QList<QByteArray> parts = lines.value(0).trimmed().split(' ');
                state->method = QString::fromLatin1(parts.value(0));
                state->path = QString::fromLatin1(parts.value(1));
                for (int i = 1; i < lines.size(); ++i) {
                    const QByteArray line = lines.at(i);
                    const int colon = line.indexOf(':');
                    if (colon < 0) continue;
                    const QByteArray name = line.left(colon).trimmed().toLower();
                    const QByteArray value = line.mid(colon + 1).trimmed();
                    if (name == "content-length") state->contentLength = value.toInt();
                    else if (name == "content-type") state->contentType = value;
                }
            }
            if (state->buffer.size() < headerEnd + 4 + state->contentLength) return;  // 请求体未到齐
            Request captured;
            captured.method = state->method;
            captured.path = state->path;
            captured.body = state->buffer.mid(headerEnd + 4, state->contentLength);
            captured.contentType = state->contentType;
            m_requests.append(captured);
            // 一次连接只处理一个请求（应答一律 Connection: close）。
            state->buffer.clear();
            state->headersParsed = false;
            state->contentLength = 0;
            if (!m_routes.contains(state->path)) {
                socket->write(QStringLiteral("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n"
                                             "Connection: close\r\n\r\n").toLatin1());
                socket->disconnectFromHost();
                return;
            }
            respond(socket, m_routes.value(state->path));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
    void respond(QTcpSocket *socket, const Response &response)
    {
        if (response.abortConnection) { socket->abort(); return; }
        if (response.hang) return;
        QByteArray packet = QStringLiteral("HTTP/1.0 %1 X\r\n").arg(response.status).toLatin1();
        packet += "Content-Type: application/json\r\n";
        packet += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        packet += "Connection: close\r\n\r\n";
        packet += response.body;
        const auto write = [socket, packet]() { socket->write(packet); socket->disconnectFromHost(); };
        if (response.delayMs > 0) QTimer::singleShot(response.delayMs, socket, write);
        else write();
    }

    QTcpServer m_server;
    QMap<QString, Response> m_routes;
    QList<Request> m_requests;
};

QByteArray summaryBody()
{
    return QStringLiteral(
        "{\"schemaVersion\":1,\"generatedAt\":\"2026-11-14T08:00:00+00:00\",\"users\":42,"
        "\"usage\":{\"tasksCreated\":1000,\"notesCreated\":200,\"tasksCompleted\":900,"
        "\"longestUsageDays\":77},"
        "\"downloads\":{\"total\":55,\"byVersion\":["
        "{\"version\":\"1.2.0\",\"count\":31},{\"version\":\"1.1.0\",\"count\":24}]}}")
        .toUtf8();
}

// 合成一条合法事件（仓储校验要求 UUID、非空标题、String 值、同批次 batch_id 一致）。
EntityEvent syntheticEvent(const QString &entityId, EntityKind kind, const QString &title,
                           const QString &type, const QString &field, const QVariant &oldValue,
                           const QVariant &newValue, const QDateTime &occurredAt,
                           const QString &batchId)
{
    EntityEvent event;
    event.id = newUuid();
    event.entityId = entityId;
    event.entityKind = kind;
    event.entityTitle = title;
    event.batchId = batchId;
    event.type = type;
    event.field = field;
    event.oldValue = oldValue;
    event.newValue = newValue;
    event.occurredAt = occurredAt;
    return event;
}

} // namespace

class StatsTest : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        Database::instance().close();
        m_directory.reset(new QTemporaryDir);
        QVERIFY(m_directory->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_directory->path().toUtf8());
    }
    void cleanup()
    {
        Database::instance().close();
        qunsetenv("KYLINLIST_DATA_DIR");
        m_directory.reset();
    }

    void emptyStatistics();
    void countingAccumulationAndCompletion();
    void permanentDeleteKeepsEvents();
    void legacyBackfillNoDoubleCount();
    void duplicateAndOutOfOrderEvents();
    void payloadWhitelistOnly();
    void disabledWithoutBaseUrl();
    void reportPostPathBodyAndSuccess();
    void reportWithPrefixAndDefaultScheme();
    void reportFailureRetriesNextTime();
    void reportTimeout();
    void noOverlappingReports();
    void automaticHourlyCycle();
    void stopPreventsAutomaticReports();
    void summaryHappyPath();
    void summary404DoesNotAffectLocalStats();
    void summaryBadJson();
    void summaryWrongSchema();
    void summaryTooLarge();
    void summaryTimeout();

private:
    bool openDatabase()
    {
        QString error;
        const bool ok = Database::instance().openAt(
            m_directory->filePath(QStringLiteral("kylinlist.db")), &error);
        if (!ok) qWarning() << error;
        return ok;
    }
    std::unique_ptr<QTemporaryDir> m_directory;
};

// 空库：全 0、无首任务、firstTaskDays 为 null 语义。
void StatsTest::emptyStatistics()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    QString error = QStringLiteral("哨兵");
    const UsageStatistics stats = store.usageStatistics(&error);
    QCOMPARE(error, QString());
    QCOMPARE(stats.tasksCreated, qint64(0));
    QCOMPARE(stats.notesCreated, qint64(0));
    QCOMPARE(stats.tasksCompleted, qint64(0));
    QVERIFY(!stats.hasFirstTask());
    QCOMPARE(stats.firstTaskDays(utcNow()), qint64(-1));
    const QJsonObject payload = StatsService::buildUsagePayload(stats, utcNow());
    QVERIFY(payload.value(QStringLiteral("firstTaskDays")).isNull());
}

// 累计口径：创建任务/笔记计数、不同任务完成数、完成再打开不重复计。
void StatsTest::countingAccumulationAndCompletion()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString taskA = store.createTask(QStringLiteral("MARKER-TASK-A"));
    const QString taskB = store.createTask(QStringLiteral("MARKER-TASK-B"));
    const QString taskC = store.createTask(QStringLiteral("MARKER-TASK-C"));
    const QString note = store.createNote(QStringLiteral("MARKER-NOTE-N"));
    QVERIFY(!taskA.isEmpty() && !taskB.isEmpty() && !taskC.isEmpty() && !note.isEmpty());

    UsageStatistics stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(3));
    QCOMPARE(stats.notesCreated, qint64(1));
    QCOMPARE(stats.tasksCompleted, qint64(0));
    QVERIFY(stats.hasFirstTask());

    QVERIFY(store.setCompleted(taskA, true));
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCompleted, qint64(1));
    // 完成后再打开、再次完成：不同任务口径不重复计。
    QVERIFY(store.setCompleted(taskA, false));
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCompleted, qint64(1));
    QVERIFY(store.setCompleted(taskA, true));
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCompleted, qint64(1));
    QVERIFY(store.setCompleted(taskB, true));
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCompleted, qint64(2));
    // 未变的口径不受影响。
    QCOMPARE(stats.tasksCreated, qint64(3));
    QCOMPARE(stats.notesCreated, qint64(1));

    // 使用天数：刚创建为 0；首任务 3 天多 1 小时后为 3。
    QCOMPARE(stats.firstTaskDays(utcNow()), qint64(0));
    const QDateTime later = stats.firstTaskAt.addDays(3).addSecs(3600);
    QCOMPARE(stats.firstTaskDays(later), qint64(3));
    // 时钟早于首任务：非负。
    QCOMPARE(stats.firstTaskDays(stats.firstTaskAt.addSecs(-60)), qint64(0));
    // 重复计算结果稳定。
    const UsageStatistics again = store.usageStatistics();
    QCOMPARE(again.tasksCreated, stats.tasksCreated);
    QCOMPARE(again.tasksCompleted, stats.tasksCompleted);
    QCOMPARE(again.firstTaskAt, stats.firstTaskAt);

    Q_UNUSED(taskC);
    Q_UNUSED(note);
}

// 永久删除后事件仍在：计数不回退。
void StatsTest::permanentDeleteKeepsEvents()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString doomed = store.createTask(QStringLiteral("MARKER-DOOMED"));
    const QString kept = store.createTask(QStringLiteral("MARKER-KEPT"));
    QVERIFY(store.setCompleted(doomed, true));
    UsageStatistics stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(2));
    QCOMPARE(stats.tasksCompleted, qint64(1));

    QVERIFY(store.trashTask(doomed));
    QVERIFY(store.deletePermanently(doomed));
    QCOMPARE(store.entities().size(), 1);
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(2));       // 事件仍在，计数不回退
    QCOMPARE(stats.tasksCompleted, qint64(1));
    QVERIFY(stats.hasFirstTask());
    Q_UNUSED(kept);
}

// 早期无事件实体：用快照补齐，且与事件不重复计数。
void StatsTest::legacyBackfillNoDoubleCount()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString task = store.createTask(QStringLiteral("MARKER-LEGACY-T"));
    const QString note = store.createNote(QStringLiteral("MARKER-LEGACY-N"));
    QVERIFY(store.setCompleted(task, true));
    QCOMPARE(store.usageStatistics().tasksCompleted, qint64(1));

    // 模拟事件日志缺失的旧库：清空 entity_events，仅靠快照补齐。
    QSqlQuery purge(Database::instance().connection());
    QVERIFY2(purge.exec(QStringLiteral("DELETE FROM entity_events")),
             qPrintable(purge.lastError().text()));

    UsageStatistics stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(1));
    QCOMPARE(stats.notesCreated, qint64(1));
    QCOMPARE(stats.tasksCompleted, qint64(1));      // 完成态补计一次
    QVERIFY(stats.hasFirstTask());
    const Entity taskEntity = store.entity(task);
    QCOMPARE(stats.firstTaskAt, taskEntity.createdAt);

    // 事件恢复后（新的状态事件）不与补齐口径双计。
    QVERIFY(store.setCompleted(task, false));
    QVERIFY(store.setCompleted(task, true));
    stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(1));
    QCOMPARE(stats.notesCreated, qint64(1));
    QCOMPARE(stats.tasksCompleted, qint64(1));
    Q_UNUSED(note);
}

// 重复 / 乱序事件：按实体 id 去重，firstTaskAt 取最早。
void StatsTest::duplicateAndOutOfOrderEvents()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString first = store.createTask(QStringLiteral("MARKER-DUP-FIRST"));
    const QString second = store.createTask(QStringLiteral("MARKER-DUP-SECOND"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    const QDateTime early = QDateTime(QDate(2020, 1, 1), QTime(1, 30), Qt::UTC);
    const QDateTime late = QDateTime(QDate(2026, 11, 14), QTime(9, 0), Qt::UTC);

    TaskRepository repository(Database::instance().connection());
    const QString batchId = newUuid();   // 同一次 appendEvents 的事件 batch_id 必须一致
    QVector<EntityEvent> events;
    // 同一实体重复 created 事件（时间乱序：late 与 early 混排）。
    events.append(syntheticEvent(first, EntityKind::Task, QStringLiteral("MARKER-DUP-FIRST"),
                                 QStringLiteral("created"), QString(), QVariant(),
                                 QStringLiteral("{}"), late, batchId));
    events.append(syntheticEvent(first, EntityKind::Task, QStringLiteral("MARKER-DUP-FIRST"),
                                 QStringLiteral("created"), QString(), QVariant(),
                                 QStringLiteral("{}"), early, batchId));
    // 同一实体两条相同的状态完成事件。
    events.append(syntheticEvent(second, EntityKind::Task, QStringLiteral("MARKER-DUP-SECOND"),
                                 QStringLiteral("changed"), QStringLiteral("status"),
                                 QStringLiteral("active"), QStringLiteral("completed"), late, batchId));
    events.append(syntheticEvent(second, EntityKind::Task, QStringLiteral("MARKER-DUP-SECOND"),
                                 QStringLiteral("changed"), QStringLiteral("status"),
                                 QStringLiteral("active"), QStringLiteral("completed"), late, batchId));
    QString appendError;
    QVERIFY2(repository.appendEvents(events, &appendError), qPrintable(appendError));

    const UsageStatistics stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(2));       // 重复 created 只计一次
    QCOMPARE(stats.tasksCompleted, qint64(1));     // 重复 completed 只计一次
    QVERIFY(stats.hasFirstTask());
    QCOMPARE(stats.firstTaskAt, early);            // 乱序取最早
}

// payload 白名单：只含 5 个字段，绝无标题/id/正文。
void StatsTest::payloadWhitelistOnly()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString taskTitle = QStringLiteral("MARKER-PAYLOAD-任务标题-九八七六");
    const QString noteTitle = QStringLiteral("MARKER-PAYLOAD-笔记标题-五四三二");
    const QString task = store.createTask(taskTitle);
    const QString note = store.createNote(noteTitle);
    QVERIFY(store.setCompleted(task, true));

    const UsageStatistics stats = store.usageStatistics();
    const QJsonObject payload = StatsService::buildUsagePayload(stats, utcNow());
    QStringList keys = payload.keys();
    keys.sort();
    QStringList expected;
    expected << QStringLiteral("firstTaskDays") << QStringLiteral("notesCreated")
             << QStringLiteral("schemaVersion") << QStringLiteral("tasksCompleted")
             << QStringLiteral("tasksCreated");
    QCOMPARE(keys, expected);
    QCOMPARE(payload.value(QStringLiteral("schemaVersion")).toDouble(), 1.0);
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("tasksCreated")).toDouble()), qint64(1));
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("notesCreated")).toDouble()), qint64(1));
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("tasksCompleted")).toDouble()), qint64(1));
    QVERIFY(payload.value(QStringLiteral("firstTaskDays")).isDouble());

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QVERIFY(!QString::fromUtf8(body).contains(taskTitle));
    QVERIFY(!QString::fromUtf8(body).contains(noteTitle));
    QVERIFY(!QString::fromUtf8(body).contains(QStringLiteral("MARKER")));
    for (const Entity &entity : store.entities())
        QVERIFY(!QString::fromUtf8(body).contains(entity.id));
    QVERIFY(!task.isEmpty());
    QVERIFY(!note.isEmpty());
}

// 未配置地址：Disabled，不发任何网络请求。
void StatsTest::disabledWithoutBaseUrl()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    StatsService service(&store);
    QCOMPARE(service.state(), StatsService::State::Disabled);
    QVERIFY(!service.enabled());
    QVERIFY(!service.hasSummary());

    QSignalSpy finishedSpy(&service, &StatsService::reportFinished);
    service.reportNow();
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
    QVERIFY(!service.errorText().isEmpty());
    QVERIFY(!service.lastReportAt().isValid());

    service.fetchSummary();
    QVERIFY(!service.summaryError().isEmpty());
    QVERIFY(!service.hasSummary());

    // 显式空串同样禁用。
    service.setBaseUrl(QStringLiteral("http://127.0.0.1:1"));
    QVERIFY(service.enabled());
    service.setBaseUrl(QString());
    QCOMPARE(service.state(), StatsService::State::Disabled);
    QVERIFY(!service.enabled());
    // 无效地址保持禁用并记录原因。
    service.setBaseUrl(QStringLiteral("http://"));
    QCOMPARE(service.state(), StatsService::State::Disabled);
    QVERIFY(!service.errorText().isEmpty());
}

// POST 成功路径：路径、方法、Content-Type、载荷白名单与状态推进。
void StatsTest::reportPostPathBodyAndSuccess()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString taskTitle = QStringLiteral("MARKER-REPORT-TITLE-七七七");
    const QString task = store.createTask(taskTitle);
    const QString note = store.createNote(QStringLiteral("MARKER-REPORT-NOTE"));
    QVERIFY(store.setCompleted(task, true));

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response ok;
    ok.status = 200;
    ok.body = QStringLiteral("{\"ok\":true}").toUtf8();
    fixture.setRoute(QStringLiteral("/api/v1/stats/usage"), ok);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    QCOMPARE(service.state(), StatsService::State::Idle);
    service.reportNow();
    QVERIFY(fixture.waitForRequests(1));
    QVERIFY(waitFor(5000, [&service]() {
        return service.state() == StatsService::State::Reported;
    }));
    QCOMPARE(service.errorText(), QString());
    QVERIFY(service.lastReportAt().isValid());

    QCOMPARE(fixture.requests().size(), 1);
    const FixtureHttpServer::Request request = fixture.requests().first();
    QCOMPARE(request.method, QStringLiteral("POST"));
    QCOMPARE(request.path, QStringLiteral("/api/v1/stats/usage"));
    QVERIFY(request.contentType.startsWith(QByteArrayLiteral("application/json")));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(request.body, &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject payload = document.object();
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("tasksCreated")).toDouble()), qint64(1));
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("notesCreated")).toDouble()), qint64(1));
    QCOMPARE(static_cast<qint64>(payload.value(QStringLiteral("tasksCompleted")).toDouble()), qint64(1));
    QCOMPARE(payload.value(QStringLiteral("schemaVersion")).toDouble(), 1.0);
    QVERIFY(payload.contains(QStringLiteral("firstTaskDays")));
    // 请求体绝无合成标题或实体 id。
    const QString body = QString::fromUtf8(request.body);
    QVERIFY(!body.contains(taskTitle));
    QVERIFY(!body.contains(QStringLiteral("MARKER")));
    for (const Entity &entity : store.entities())
        QVERIFY(!body.contains(entity.id));
    QVERIFY(!note.isEmpty());
}

// 基础路径前缀 + 省略协议默认补 http。
void StatsTest::reportWithPrefixAndDefaultScheme()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response ok;
    ok.body = QStringLiteral("{\"ok\":true}").toUtf8();
    const QString route = QStringLiteral("/prefix/api/v1/stats/usage");
    fixture.setRoute(route, ok);

    StatsService service(&store);
    service.setBaseUrl(QStringLiteral("127.0.0.1:%1/prefix").arg(fixture.serverPort()));
    QVERIFY(service.enabled());
    QVERIFY(service.baseUrl().startsWith(QStringLiteral("http://127.0.0.1:")));
    QVERIFY(service.baseUrl().endsWith(QStringLiteral("/prefix/")));
    service.reportNow();
    QVERIFY(fixture.waitForRequests(1));
    QVERIFY(waitFor(5000, [&service]() {
        return service.state() == StatsService::State::Reported;
    }));
    QCOMPARE(fixture.requests().first().path, route);
}

// 404 失败保存 errorText，之后（模拟下一小时）重试成功。
void StatsTest::reportFailureRetriesNextTime()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());   // 未注册路由：一律 404
    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.reportNow();
    QVERIFY(fixture.waitForRequests(1));
    QVERIFY(waitFor(5000, [&service]() {
        return service.state() == StatsService::State::Failed;
    }));
    QVERIFY(service.errorText().contains(QStringLiteral("404")));
    QVERIFY(!service.lastReportAt().isValid());

    // “下一小时再试”：路径恢复后手动重发即成功。
    FixtureHttpServer::Response ok;
    ok.body = QStringLiteral("{\"ok\":true}").toUtf8();
    fixture.setRoute(QStringLiteral("/api/v1/stats/usage"), ok);
    service.reportNow();
    QVERIFY(waitFor(5000, [&service]() {
        return service.state() == StatsService::State::Reported;
    }));
    QVERIFY(service.lastReportAt().isValid());
    QCOMPARE(service.errorText(), QString());
}

// 上报超时：10 秒默认可注入，超时后失败并可再次尝试。
void StatsTest::reportTimeout()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response hang;
    hang.hang = true;
    fixture.setRoute(QStringLiteral("/api/v1/stats/usage"), hang);

    StatsService service(&store);
    service.setRequestTimeoutMs(250);
    service.setBaseUrl(fixture.baseUrl());
    service.reportNow();
    QVERIFY(fixture.waitForRequests(1));
    QVERIFY(waitFor(5000, [&service]() {
        return service.state() == StatsService::State::Failed;
    }));
    QVERIFY(service.errorText().contains(QStringLiteral("超时")));
    QVERIFY(!service.lastReportAt().isValid());
}

// 在途请求不重叠：手动连发与自动周期都不产生并发请求。
void StatsTest::noOverlappingReports()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    const QString path = QStringLiteral("/api/v1/stats/usage");
    FixtureHttpServer::Response slow;
    slow.body = QStringLiteral("{\"ok\":true}").toUtf8();
    slow.delayMs = 200;
    fixture.setRoute(path, slow);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.reportNow();
    service.reportNow();   // 在途：直接忽略
    service.reportNow();
    QVERIFY(waitFor(5000, [&service, &fixture, path]() {
        return fixture.requestCount(path) >= 1
            && service.state() == StatsService::State::Reported;
    }));
    QTest::qWait(300);
    QCOMPARE(fixture.requestCount(path), 1);
}

// 启动后延时首报 + 周期再报（时间常量已注入缩小）。
void StatsTest::automaticHourlyCycle()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    const QString path = QStringLiteral("/api/v1/stats/usage");
    FixtureHttpServer::Response ok;
    ok.body = QStringLiteral("{\"ok\":true}").toUtf8();
    fixture.setRoute(path, ok);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.setReportTiming(60, 120);
    service.startAutomaticReports();
    QVERIFY(fixture.waitForRequests(3, 5000));
    service.stopAutomaticReports();
    const int count = fixture.requestCount(path);
    QVERIFY(count >= 3);
    for (const FixtureHttpServer::Request &request : fixture.requests())
        QCOMPARE(request.path, path);
    QVERIFY(service.lastReportAt().isValid());
}

// stop 之后周期不再触发。
void StatsTest::stopPreventsAutomaticReports()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    const QString path = QStringLiteral("/api/v1/stats/usage");
    FixtureHttpServer::Response ok;
    ok.body = QStringLiteral("{\"ok\":true}").toUtf8();
    fixture.setRoute(path, ok);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.setReportTiming(80, 100);
    service.startAutomaticReports();
    service.stopAutomaticReports();
    QTest::qWait(500);
    QCOMPARE(fixture.requestCount(path), 0);
    QVERIFY(!service.lastReportAt().isValid());
}

// GET 摘要：带前缀路径、契约字段全部解析。
void StatsTest::summaryHappyPath()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    const QString route = QStringLiteral("/stats/api/v1/stats/summary");
    FixtureHttpServer::Response ok;
    ok.body = summaryBody();
    fixture.setRoute(route, ok);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl() + QStringLiteral("/stats"));
    QSignalSpy changedSpy(&service, &StatsService::summaryChanged);
    service.fetchSummary();
    QVERIFY(waitFor(5000, [&service, &changedSpy]() {
        return !service.fetchingSummary() && changedSpy.count() >= 1;
    }));
    QVERIFY(service.hasSummary());
    QCOMPARE(service.summaryError(), QString());
    const StatsSummary summary = service.summary();
    QCOMPARE(summary.schemaVersion, 1);
    QCOMPARE(summary.generatedAt, QStringLiteral("2026-11-14T08:00:00+00:00"));
    QCOMPARE(summary.users, qint64(42));
    QCOMPARE(summary.tasksCreated, qint64(1000));
    QCOMPARE(summary.notesCreated, qint64(200));
    QCOMPARE(summary.tasksCompleted, qint64(900));
    QCOMPARE(summary.longestUsageDays, qint64(77));
    QCOMPARE(summary.downloadsTotal, qint64(55));
    QCOMPARE(summary.downloadsByVersion.size(), 2);
    QCOMPARE(summary.downloadsByVersion.at(0).version, QStringLiteral("1.2.0"));
    QCOMPARE(summary.downloadsByVersion.at(0).count, qint64(31));
    QCOMPARE(summary.downloadsByVersion.at(1).version, QStringLiteral("1.1.0"));
    QCOMPARE(summary.downloadsByVersion.at(1).count, qint64(24));

    QCOMPARE(fixture.requests().size(), 1);
    QCOMPARE(fixture.requests().first().method, QStringLiteral("GET"));
    QCOMPARE(fixture.requests().first().path, route);
}

// 旧服务器 404：摘要失败但不影响本机统计。
void StatsTest::summary404DoesNotAffectLocalStats()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const QString task = store.createTask(QStringLiteral("MARKER-SUMMARY-404"));
    QVERIFY(!task.isEmpty());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());   // 无路由：404
    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    QSignalSpy changedSpy(&service, &StatsService::summaryChanged);
    service.fetchSummary();
    QVERIFY(waitFor(5000, [&service, &changedSpy]() {
        return !service.fetchingSummary() && changedSpy.count() >= 1;
    }));
    QVERIFY(!service.hasSummary());
    QVERIFY(service.summaryError().contains(QStringLiteral("404")));

    const UsageStatistics stats = store.usageStatistics();
    QCOMPARE(stats.tasksCreated, qint64(1));       // 本机统计不受影响
}

void StatsTest::summaryBadJson()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response bad;
    bad.body = QStringLiteral("not-json{").toUtf8();
    fixture.setRoute(QStringLiteral("/api/v1/stats/summary"), bad);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.fetchSummary();
    QVERIFY(waitFor(5000, [&service]() { return !service.fetchingSummary(); }));
    QVERIFY(!service.hasSummary());
    QVERIFY(service.summaryError().contains(QStringLiteral("JSON")));
}

void StatsTest::summaryWrongSchema()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response future;
    future.body = summaryBody().replace("\"schemaVersion\":1", "\"schemaVersion\":2");
    fixture.setRoute(QStringLiteral("/api/v1/stats/summary"), future);

    StatsService service(&store);
    service.setBaseUrl(fixture.baseUrl());
    service.fetchSummary();
    QVERIFY(waitFor(5000, [&service]() { return !service.fetchingSummary(); }));
    QVERIFY(!service.hasSummary());
    QVERIFY(service.summaryError().contains(QStringLiteral("schemaVersion")));
}

void StatsTest::summaryTooLarge()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response huge;
    huge.body = QByteArray(1024 * 1024 + 4096, 'x');   // 超过 1MiB 上限
    fixture.setRoute(QStringLiteral("/api/v1/stats/summary"), huge);

    StatsService service(&store);
    service.setRequestTimeoutMs(10000);
    service.setBaseUrl(fixture.baseUrl());
    service.fetchSummary();
    QVERIFY(waitFor(10000, [&service]() { return !service.fetchingSummary(); }));
    QVERIFY(!service.hasSummary());
    QVERIFY(service.summaryError().contains(QStringLiteral("过大")));
}

void StatsTest::summaryTimeout()
{
    QVERIFY(openDatabase());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());

    FixtureHttpServer fixture;
    QVERIFY(fixture.start());
    FixtureHttpServer::Response hang;
    hang.hang = true;
    fixture.setRoute(QStringLiteral("/api/v1/stats/summary"), hang);

    StatsService service(&store);
    service.setRequestTimeoutMs(250);
    service.setBaseUrl(fixture.baseUrl());
    service.fetchSummary();
    QVERIFY(waitFor(5000, [&service]() { return !service.fetchingSummary(); }));
    QVERIFY(!service.hasSummary());
    QVERIFY(service.summaryError().contains(QStringLiteral("超时")));
}

QTEST_MAIN(StatsTest)
#include "tst_stats.moc"
