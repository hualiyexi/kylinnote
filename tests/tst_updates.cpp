#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "Common.h"
#include "app/AutostartSettings.h"
#include "app/Settings.h"
#include "app/updates/InstallIdentity.h"
#include "platform/Autostart.h"
#include "platform/UpdateInstaller.h"
#include "app/updates/UpdateManifest.h"
#include "app/updates/UpdateService.h"

using namespace kl;

namespace {

// 极简本地 HTTP 夹具：按路径返回预设响应，支持挂起、断连与部分发送。
class FixtureHttpServer : public QObject {
    Q_OBJECT
public:
    struct Response {
        int status = 200;
        QByteArray body;
        int delayMs = 0;
        bool hang = false;              // 收到请求后不回应
        bool abortConnection = false;   // 立即断开
        int partialBytes = -1;          // >=0 时只发送前 N 字节后停住
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
    void setRoute(const QString &path, const Response &response) { m_routes.insert(path, response); }

private:
    void takeRequest(QTcpSocket *socket)
    {
        socket->setParent(this);
        auto *buffer = new QByteArray();
        connect(socket, &QObject::destroyed, socket, [buffer]() { delete buffer; });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (!buffer->contains("\r\n\r\n")) return;
            const QString path = QString::fromLatin1(buffer->split(' ').value(1));
            buffer->clear();
            if (!m_routes.contains(path)) {
                socket->write(QStringLiteral("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n"
                                             "Connection: close\r\n\r\n").toLatin1());
                socket->disconnectFromHost();
                return;
            }
            respond(socket, m_routes.value(path));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
    }
    void respond(QTcpSocket *socket, const Response &response)
    {
        if (response.abortConnection) { socket->abort(); return; }
        if (response.hang) return;
        const QByteArray body = response.partialBytes >= 0
            ? response.body.left(response.partialBytes) : response.body;
        QByteArray packet = QStringLiteral("HTTP/1.0 %1 X\r\n").arg(response.status).toLatin1();
        packet += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
        packet += "Connection: close\r\n\r\n";
        packet += body;
        const auto write = [socket, packet, response]() {
            socket->write(packet);
            if (response.partialBytes < 0) socket->disconnectFromHost();
        };
        if (response.delayMs > 0) QTimer::singleShot(response.delayMs, socket, write);
        else write();
    }

    QTcpServer m_server;
    QMap<QString, Response> m_routes;
};

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

updates::InstallIdentity makeIdentity(const QString &version)
{
    updates::InstallIdentity identity;
    identity.product = QStringLiteral("kylinlist");
    identity.platform = updates::currentBuildPlatform();
    identity.arch = updates::currentBuildArch();
    identity.variant = identity.platform == QStringLiteral("linux")
        ? QStringLiteral("system-qt") : QStringLiteral("setup");
    identity.version = version;
    return identity;
}

QString artifactSuffix()
{
    return updates::currentBuildPlatform() == QStringLiteral("linux")
        ? QStringLiteral(".deb") : QStringLiteral(".exe");
}

QByteArray buildManifestJson(const QString &version, const QString &relativePath,
                             const QByteArray &artifactBody)
{
    const QString sha = sha256Hex(artifactBody);
    const QString json = QStringLiteral(
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"channel\": \"stable\",\n"
        "  \"version\": \"%1\",\n"
        "  \"publishedAt\": \"2026-11-13T08:00:00Z\",\n"
        "  \"notes\": \"测试版本说明\",\n"
        "  \"artifacts\": [\n"
        "    {\"platform\": \"%2\", \"arch\": \"%3\", \"variant\": \"%4\", "
        "\"packageVersion\": \"%1\",\n"
        "     \"path\": \"%5\", \"size\": %6, \"sha256\": \"%7\"}\n"
        "  ]\n"
        "}")
        .arg(version,
             updates::currentBuildPlatform(),
             updates::currentBuildArch(),
             updates::currentBuildPlatform() == QStringLiteral("linux")
                 ? QStringLiteral("system-qt") : QStringLiteral("setup"),
             relativePath)
        .arg(artifactBody.size())
        .arg(sha);
    return json.toUtf8();
}

QString manifestRoute(const QString &prefix = QString())
{
    return prefix + QStringLiteral("/updates/v1/stable/manifest.json");
}

// 带可选 history 的清单：historyValue 为原样 JSON 值（null QString 表示完全省略该键）。
QByteArray manifestWithHistory(const QString &version, const QString &historyValue,
                               const QByteArray &artifactBody)
{
    const QString sha = sha256Hex(artifactBody);
    const QString relative = QStringLiteral("files/%1/%2-%3-%4/pkg%5")
        .arg(version, updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    QString historyLine;
    if (!historyValue.isNull())
        historyLine = QStringLiteral("  \"history\": %1,\n").arg(historyValue);
    const QString json = QStringLiteral(
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"channel\": \"stable\",\n"
        "  \"version\": \"%1\",\n"
        "  \"publishedAt\": \"2026-11-14T08:00:00Z\",\n"
        "  \"notes\": \"顶部公告\",\n"
        "%2"
        "  \"artifacts\": [\n"
        "    {\"platform\": \"%3\", \"arch\": \"%4\", \"variant\": \"%5\", "
        "\"packageVersion\": \"%1\",\n"
        "     \"path\": \"%6\", \"size\": %7, \"sha256\": \"%8\"}\n"
        "  ]\n"
        "}")
        .arg(version, historyLine,
             updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, relative)
        .arg(artifactBody.size())
        .arg(sha);
    return json.toUtf8();
}

// 等待条件成立或超时。
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

// 自启策略替身后端：内存登记状态 + 调用计数 + 可编程失败。
// 生命周期必须覆盖用它构造的 AutostartSettings（先声明、后销毁）。
struct FakeAutostart {
    int installedCalls = 0;
    int setCalls = 0;
    bool installed = false;
    bool failSet = false;        // setEnabled 直接失败，状态不变
    bool mutateThenFail = false; // 先改状态再失败（模拟半写成功）

    AutostartSettings::Backend backend()
    {
        return {
            [this]() { ++installedCalls; return installed; },
            [this](bool enabled, QString *error) {
                ++setCalls;
                if (mutateThenFail) installed = enabled;
                if (failSet || mutateThenFail) {
                    if (error) *error = QStringLiteral("写入自启失败(测试)");
                    return false;
                }
                installed = enabled;
                return true;
            }
        };
    }
};

// Settings 单例在本测试进程内共享，逐用例显式设定自启一次性标记。
void setAutostartMarker(bool done)
{
    Settings &settings = Settings::instance();
    settings.setAutostartInitialized(done);
    settings.sync();
}

} // namespace

class UpdatesTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init() { m_dir.reset(new QTemporaryDir); QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8()); }
    void cleanup() { qputenv("KYLINLIST_UPDATE_INSTALL_INFO_PATH", "");
        qputenv("KYLINLIST_UPDATE_FAKE_INSTALLER", ""); m_dir.reset(); }

    void versionParsing_data();
    void versionParsing();
    void versionCompare();
    void manifestParsing();
    void manifestHistoryParsing();
    void releaseNotesForUpgrade();
    void artifactPathValidation();
    void fullReleaseManifestParses();
    void manifestTargetSelection();
    void identityParsing();
    void identityReadFromOverride();
    void pendingEvaluation();
    void postponeDecision();
    void verifyInstallerFileChecks();
    void settingsUpdateDefaults();
    void baseUrlNormalization();
    void serviceCheckFindsUpdate();
    void serviceCheckUpToDate();
    void serviceCheckHttpError();
    void serviceCheckTimeout();
    void serviceCheckMissingTarget();
    void serviceCheckNotConfigured();
    void serviceCheckAutomaticSource();
    void serviceAutomaticSkipsWhenReady();
    void serviceDownloadSucceeds();
    void serviceDownloadHashMismatch();
    void serviceDownloadSizeMismatch();
    void serviceDownloadCancel();
    void serviceDownloadStall();
    void serviceRespectsBasePrefix();
    void installerHandOffFake();

    // ---------- 自启共享策略（AutostartSettings，替身后端） ----------
    void autostartFirstDefaultRegistersOnce();
    void autostartAlreadyRegisteredMarksWithoutWrite();
    void autostartFailedInitializationRetries();
    void autostartExplicitDisablePersists();
    void autostartIsolatedAndUnavailableInert();
    void autostartPreviewOptInWithoutDefault();
    void autostartWindowsCommandQuotesArguments();
    void autostartPortableOptInOnly();
    void autostartBackendFailureSurfacesError();

private:
    QScopedPointer<QTemporaryDir> m_settingsDir;
    QScopedPointer<QTemporaryDir> m_dir;
};

void UpdatesTest::initTestCase()
{
    // 把 QSettings(IniFormat, UserScope) 重定向到本测试专属目录：
    // Settings 单例只构造一次，此后的所有读写都不触碰真实用户配置。
    m_settingsDir.reset(new QTemporaryDir);
    QVERIFY2(m_settingsDir->isValid(), "cannot create settings temp dir");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir->path() + QStringLiteral("/settings"));
}

void UpdatesTest::versionParsing_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::newRow("plain") << QStringLiteral("1.2.0") << true;
    QTest::newRow("zeros") << QStringLiteral("0.0.0") << true;
    QTest::newRow("large") << QStringLiteral("10.20.30") << true;
    QTest::newRow("two-parts") << QStringLiteral("1.2") << false;
    QTest::newRow("four-parts") << QStringLiteral("1.2.3.4") << false;
    QTest::newRow("leading-zero") << QStringLiteral("1.02.0") << false;
    QTest::newRow("prefix-v") << QStringLiteral("v1.2.0") << false;
    QTest::newRow("suffix") << QStringLiteral("1.2.0-rc1") << false;
    QTest::newRow("empty") << QString() << false;
}

void UpdatesTest::versionParsing()
{
    QFETCH(QString, text);
    QFETCH(bool, valid);
    updates::VersionTriple triple;
    QCOMPARE(updates::parseStrictVersion(text, &triple), valid);
    if (!valid) return;
    QCOMPARE(triple.major, text.split(QLatin1Char('.')).at(0).toInt());
    QCOMPARE(triple.minor, text.split(QLatin1Char('.')).at(1).toInt());
    QCOMPARE(triple.patch, text.split(QLatin1Char('.')).at(2).toInt());
}

void UpdatesTest::versionCompare()
{
    updates::VersionTriple a, b;
    QVERIFY(updates::parseStrictVersion(QStringLiteral("1.9.0"), &a));
    QVERIFY(updates::parseStrictVersion(QStringLiteral("1.10.0"), &b));
    QCOMPARE(updates::compareVersions(a, b), -1);
    QVERIFY(updates::parseStrictVersion(QStringLiteral("2.0.0"), &a));
    QVERIFY(updates::parseStrictVersion(QStringLiteral("1.99.99"), &b));
    QCOMPARE(updates::compareVersions(a, b), 1);
    QVERIFY(updates::parseStrictVersion(QStringLiteral("3.4.5"), &a));
    QVERIFY(updates::parseStrictVersion(QStringLiteral("3.4.5"), &b));
    QCOMPARE(updates::compareVersions(a, b), 0);
}

void UpdatesTest::manifestParsing()
{
    const QByteArray body = QByteArrayLiteral("fake-installer-bytes");
    QByteArray good = buildManifestJson(QStringLiteral("1.2.0"),
        QStringLiteral("files/1.2.0/") + updates::currentBuildPlatform() + QStringLiteral("-")
            + updates::currentBuildArch() + QStringLiteral("-var/pkg") + artifactSuffix(),
        body);
    QString error;
    updates::UpdateManifest manifest = updates::parseManifest(good, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(manifest.version, QStringLiteral("1.2.0"));
    QCOMPARE(manifest.artifacts.size(), 1);

    auto expectFail = [&body](const QByteArray &json, const char *what) {
        QString error;
        updates::UpdateManifest manifest = updates::parseManifest(json, &error);
        QVERIFY2(!error.isEmpty(), what);
        QVERIFY2(manifest.artifacts.isEmpty(), what);
    };
    expectFail(QByteArrayLiteral("{}"), "empty object");
    expectFail(QByteArrayLiteral("not json"), "invalid json");
    expectFail("[]", "not an object");
    // schemaVersion 错误
    expectFail(good.replace("\"schemaVersion\": 1", "\"schemaVersion\": 2"), "schema");
    // channel 错误
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2.0"), QStringLiteral("files/1.2.0/a/b/c.deb"), body);
        expectFail(json.replace("\"channel\": \"stable\"", "\"channel\": \"beta\""), "channel");
    }
    // 版本不合法
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2"), QStringLiteral("files/1.2/a/b/c.deb"), body);
        expectFail(json, "bad version");
    }
    // packageVersion 不一致
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2.0"), QStringLiteral("files/1.2.0/a/b/c.deb"), body);
        expectFail(json.replace("\"packageVersion\": \"1.2.0\"", "\"packageVersion\": \"1.2.1\""),
                   "packageVersion");
    }
    // 路径越界
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2.0"), QStringLiteral("files/1.2.0/a/b/../../../c.deb"), body);
        expectFail(json, "path escape");
    }
    // 路径绝对
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2.0"), QStringLiteral("/etc/passwd"), body);
        expectFail(json, "absolute path");
    }
    // 路径版本前缀不符
    {
        QByteArray json = buildManifestJson(QStringLiteral("1.2.0"), QStringLiteral("files/1.1.0/a/b/c.deb"), body);
        expectFail(json, "wrong version prefix");
    }
    // 重复目标
    {
        const QString zero = QString(64, QLatin1Char('0'));
        const QByteArray json = QStringLiteral(
            "{\"schemaVersion\":1,\"channel\":\"stable\",\"version\":\"1.2.0\","
            "\"publishedAt\":\"t\",\"notes\":\"\",\"artifacts\":["
            "{\"platform\":\"linux\",\"arch\":\"arm64\",\"variant\":\"system-qt\","
            "\"packageVersion\":\"1.2.0\",\"path\":\"files/1.2.0/linux-arm64-system-qt/a.deb\","
            "\"size\":1,\"sha256\":\"%1\"},"
            "{\"platform\":\"linux\",\"arch\":\"arm64\",\"variant\":\"system-qt\","
            "\"packageVersion\":\"1.2.0\",\"path\":\"files/1.2.0/linux-arm64-system-qt/b.deb\","
            "\"size\":1,\"sha256\":\"%1\"}]}")
            .arg(zero)
            .toUtf8();
        expectFail(json, "duplicate target");
    }
}

void UpdatesTest::manifestHistoryParsing()
{
    const QByteArray body = QByteArrayLiteral("history-pkg");
    // 乱序书写也会按版本升序规范化。
    const QString entries = QStringLiteral(
        "[{\"version\":\"1.4.0\",\"publishedAt\":\"2026-11-05T08:00:00Z\",\"notes\":\"1.4 公告\"},"
        "{\"version\":\"1.2.0\",\"publishedAt\":\"2026-10-01T08:00:00Z\",\"notes\":\"1.2 公告\"},"
        "{\"version\":\"1.3.0\",\"publishedAt\":\"2026-10-20T08:00:00Z\",\"notes\":\"1.3 公告\"}]");
    QString error;
    updates::UpdateManifest manifest = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.4.0"), entries, body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(manifest.history.size(), 3);
    QCOMPARE(manifest.history.at(0).version, QStringLiteral("1.2.0"));
    QCOMPARE(manifest.history.at(1).version, QStringLiteral("1.3.0"));
    QCOMPARE(manifest.history.at(2).version, QStringLiteral("1.4.0"));
    QCOMPARE(manifest.history.at(1).notes, QStringLiteral("1.3 公告"));
    QCOMPARE(manifest.history.at(1).publishedAt, QStringLiteral("2026-10-20T08:00:00Z"));
    // notes 允许为空字符串（但键必须存在且是字符串）。
    const QString emptyNotes = QStringLiteral(
        "[{\"version\":\"1.4.0\",\"publishedAt\":\"2026-11-05T08:00:00Z\",\"notes\":\"\"}]");
    error.clear();
    manifest = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.4.0"), emptyNotes, body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(manifest.history.size(), 1);
    QVERIFY(manifest.history.at(0).notes.isEmpty());
    // 空 history 数组等价于旧清单：无逐版本公告。
    error.clear();
    manifest = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.4.0"), QStringLiteral("[]"), body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(manifest.history.isEmpty());

    auto expectFail = [body](const QString &historyValue, const char *what) {
        QString error;
        updates::UpdateManifest manifest = updates::parseManifest(
            manifestWithHistory(QStringLiteral("1.4.0"), historyValue, body), &error);
        QVERIFY2(!error.isEmpty(), what);
        QVERIFY2(manifest.artifacts.isEmpty(), what);
    };
    // history 不是数组
    expectFail(QStringLiteral("\"text\""), "history not array");
    expectFail(QStringLiteral("3"), "history number");
    // 项不是对象
    expectFail(QStringLiteral("[\"1.2.0\"]"), "item not object");
    // 版本不合法 / 缺失
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2\",\"publishedAt\":\"t\",\"notes\":\"n\"}]"), "bad item version");
    expectFail(QStringLiteral(
        "[{\"publishedAt\":\"t\",\"notes\":\"n\"}]"), "missing item version");
    // publishedAt 为空 / 缺失
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"\",\"notes\":\"n\"}]"), "empty publishedAt");
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2.0\",\"notes\":\"n\"}]"), "missing publishedAt");
    // notes 缺失或不是字符串
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"t\"}]"), "missing notes");
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"t\",\"notes\":3}]"), "notes not string");
    // 重复版本（与书写顺序无关）
    expectFail(QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"t\",\"notes\":\"a\"},"
        "{\"version\":\"1.2.0\",\"publishedAt\":\"t2\",\"notes\":\"b\"}]"), "duplicate version");
}

void UpdatesTest::releaseNotesForUpgrade()
{
    const QByteArray body = QByteArrayLiteral("notes-pkg");
    const QString entries = QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"2026-10-01T08:00:00Z\",\"notes\":\"1.2 公告\"},"
        "{\"version\":\"1.3.0\",\"publishedAt\":\"2026-10-20T08:00:00Z\",\"notes\":\"1.3 公告\"},"
        "{\"version\":\"1.4.0\",\"publishedAt\":\"2026-11-05T08:00:00Z\",\"notes\":\"1.4 公告\"}]");
    QString error;
    const updates::UpdateManifest manifest = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.4.0"), entries, body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    using updates::ReleaseNote;
    auto versionsOf = [](const QVector<ReleaseNote> &notes) {
        QStringList versions;
        for (const ReleaseNote &note : notes) versions << note.version;
        return versions;
    };
    // 当前 < 版本 <= 目标：升序返回。
    QCOMPARE(versionsOf(updates::releaseNotesForUpgrade(
                  manifest, QStringLiteral("1.1.0"))),
             QStringList({QStringLiteral("1.2.0"), QStringLiteral("1.3.0"), QStringLiteral("1.4.0")}));
    QCOMPARE(versionsOf(updates::releaseNotesForUpgrade(
                  manifest, QStringLiteral("1.2.0"))),
             QStringList({QStringLiteral("1.3.0"), QStringLiteral("1.4.0")}));
    // 当前已是目标版本：无公告。
    QVERIFY(updates::releaseNotesForUpgrade(manifest, QStringLiteral("1.4.0")).isEmpty());
    QVERIFY(updates::releaseNotesForUpgrade(manifest, QStringLiteral("2.0.0")).isEmpty());
    // 高于目标的 history 条目不纳入。
    const QString withFuture = QStringLiteral(
        "[{\"version\":\"1.2.0\",\"publishedAt\":\"2026-10-01T08:00:00Z\",\"notes\":\"1.2 公告\"},"
        "{\"version\":\"1.9.0\",\"publishedAt\":\"2026-12-01T08:00:00Z\",\"notes\":\"未来版本\"}]");
    error.clear();
    const updates::UpdateManifest capped = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.3.0"), withFuture, body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(versionsOf(updates::releaseNotesForUpgrade(
                  capped, QStringLiteral("1.1.0"))),
             QStringList({QStringLiteral("1.2.0")}));

    // 旧清单（无 history）：目标比当前新时回退为清单自身公告。
    error.clear();
    const updates::UpdateManifest legacy = updates::parseManifest(
        manifestWithHistory(QStringLiteral("1.4.0"), QString(), body), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVector<ReleaseNote> fallback =
        updates::releaseNotesForUpgrade(legacy, QStringLiteral("1.3.0"));
    QCOMPARE(fallback.size(), 1);
    QCOMPARE(fallback.at(0).version, QStringLiteral("1.4.0"));
    QCOMPARE(fallback.at(0).publishedAt, QStringLiteral("2026-11-14T08:00:00Z"));
    QCOMPARE(fallback.at(0).notes, QStringLiteral("顶部公告"));
    // 旧清单且当前不低于目标：空。
    QVERIFY(updates::releaseNotesForUpgrade(legacy, QStringLiteral("1.4.0")).isEmpty());
    QVERIFY(updates::releaseNotesForUpgrade(legacy, QStringLiteral("2.0.0")).isEmpty());
    // 当前版本无法比较时按“未知当前”处理：旧清单回退为清单自身。
    QCOMPARE(updates::releaseNotesForUpgrade(legacy, QStringLiteral("abc")).size(), 1);
    QVERIFY(updates::releaseNotesForUpgrade(manifest, QStringLiteral("abc")).isEmpty());
}

void UpdatesTest::artifactPathValidation()
{
    QVERIFY(updates::isValidArtifactPath(QStringLiteral("files/1.2.0/linux-arm64-system-qt/a.deb"),
                                         QStringLiteral("1.2.0")));
    QVERIFY(!updates::isValidArtifactPath(QStringLiteral("files/1.2.0/a.deb"), QStringLiteral("1.2.0")));
    QVERIFY(!updates::isValidArtifactPath(QStringLiteral("files/1.2.0/"), QStringLiteral("1.2.0")));
    QVERIFY(!updates::isValidArtifactPath(QStringLiteral("/files/1.2.0/a/b"), QStringLiteral("1.2.0")));
    QVERIFY(!updates::isValidArtifactPath(QStringLiteral("files/1.2.0/a/../b/c"), QStringLiteral("1.2.0")));
    QVERIFY(!updates::isValidArtifactPath(QStringLiteral("files/1.3.0/a/b/c"), QStringLiteral("1.2.0")));
}

// 与 packaging/generate-update-manifest.py 的完整六件套输出保持同一形状：
// 生成器与客户端之间的协议契约在这里锁定，任一侧改动都会让本用例失败。
void UpdatesTest::fullReleaseManifestParses()
{
    const QByteArray json = QByteArrayLiteral(
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"channel\": \"stable\",\n"
        "  \"version\": \"1.2.0\",\n"
        "  \"publishedAt\": \"2026-11-13T16:51:33Z\",\n"
        "  \"notes\": \"KylinList 1.2.0。\",\n"
        "  \"artifacts\": [\n"
        "    {\"platform\": \"linux\", \"arch\": \"arm64\", \"variant\": \"system-qt\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/linux-arm64-system-qt/kylinlist_1.2.0_arm64.deb\", \"size\": 555660,"
        " \"sha256\": \"c6dec06e712acf6ffae779e11445a8416c5d7e0b82f7c84e83ae9c3c1da2c1a3\"},\n"
        "    {\"platform\": \"linux\", \"arch\": \"arm64\", \"variant\": \"bundled-qt\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/linux-arm64-bundled-qt/kylinlist_1.2.0_arm64_bundle.deb\", \"size\": 17544488,"
        " \"sha256\": \"37778c0d7cce93baa8a4e0139b9962eb438d9a852158ca4c3e2990018a1f735f\"},\n"
        "    {\"platform\": \"linux\", \"arch\": \"amd64\", \"variant\": \"system-qt\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/linux-amd64-system-qt/kylinlist_1.2.0_amd64.deb\", \"size\": 627968,"
        " \"sha256\": \"5ac55e0194d98014bfc4041d64588697b8767c65951432bfa6cd79a66bb53610\"},\n"
        "    {\"platform\": \"linux\", \"arch\": \"amd64\", \"variant\": \"bundled-qt\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/linux-amd64-bundled-qt/kylinlist_1.2.0_amd64_bundle.deb\", \"size\": 18643592,"
        " \"sha256\": \"254ec427ea34bc80b6275d14e74f699046fac7518becf9b9717c268860319dce\"},\n"
        "    {\"platform\": \"windows\", \"arch\": \"amd64\", \"variant\": \"setup\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/windows-amd64-setup/KylinList-1.2.0-Windows-x64-Setup.exe\", \"size\": 13423401,"
        " \"sha256\": \"709e46d72391670740d281068c9ee8606dbf1db78ef62e285b36253b21834a2b\"},\n"
        "    {\"platform\": \"windows\", \"arch\": \"amd64\", \"variant\": \"portable\", \"packageVersion\": \"1.2.0\","
        " \"path\": \"files/1.2.0/windows-amd64-portable/KylinList-1.2.0-Windows-x64.zip\", \"size\": 16747842,"
        " \"sha256\": \"8608e8c6c0dc05d25b37941a76ea41a9b31577c35562c91d43718036312052b0\"}\n"
        "  ]\n"
        "}");
    QString error;
    const updates::UpdateManifest manifest = updates::parseManifest(json, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(manifest.artifacts.size(), 6);
    // 每种安装形式各自独立命中，不会串包。
    const QVector<QPair<QString, QString>> targets = {
        {QStringLiteral("system-qt"), QStringLiteral("kylinlist_1.2.0_arm64.deb")},
        {QStringLiteral("bundled-qt"), QStringLiteral("kylinlist_1.2.0_arm64_bundle.deb")},
    };
    for (const auto &target : targets) {
        const updates::UpdateArtifact *artifact =
            manifest.findArtifact(QStringLiteral("linux"), QStringLiteral("arm64"), target.first);
        QVERIFY(artifact);
        QVERIFY(artifact->path.endsWith(target.second));
    }
}

void UpdatesTest::manifestTargetSelection()
{
    updates::UpdateManifest manifest;
    updates::UpdateArtifact a;
    a.platform = QStringLiteral("linux"); a.arch = QStringLiteral("arm64");
    a.variant = QStringLiteral("system-qt"); a.size = 1;
    manifest.artifacts.append(a);
    QVERIFY(manifest.findArtifact(QStringLiteral("linux"), QStringLiteral("arm64"),
                                  QStringLiteral("system-qt")) != nullptr);
    QVERIFY(manifest.findArtifact(QStringLiteral("linux"), QStringLiteral("arm64"),
                                  QStringLiteral("bundled-qt")) == nullptr);
    QVERIFY(manifest.findArtifact(QStringLiteral("windows"), QStringLiteral("amd64"),
                                  QStringLiteral("setup")) == nullptr);
}

void UpdatesTest::identityParsing()
{
    const QString buildVersion = QStringLiteral(KYLINLIST_VERSION);
    auto jsonFor = [](const QString &product, const QString &platform, const QString &version,
                      const QString &arch, const QString &variant) {
        return QStringLiteral("{\"product\":\"%1\",\"platform\":\"%2\",\"version\":\"%3\","
                              "\"arch\":\"%4\",\"variant\":\"%5\"}")
            .arg(product, platform, version, arch, variant)
            .toUtf8();
    };
    const QString arch = updates::currentBuildArch();
    const QString variant = updates::currentBuildPlatform() == QStringLiteral("linux")
        ? QStringLiteral("system-qt") : QStringLiteral("setup");
    QString error;
    updates::InstallIdentity identity = updates::parseInstallInfoJson(
        jsonFor(QStringLiteral("kylinlist"), updates::currentBuildPlatform(), buildVersion,
                arch, variant), &error);
    QVERIFY2(identity.isValid(), qPrintable(error));
    QVERIFY(!updates::parseInstallInfoJson(
        jsonFor(QStringLiteral("other"), updates::currentBuildPlatform(), buildVersion,
                arch, variant), &error).isValid());
    QVERIFY(!updates::parseInstallInfoJson(
        jsonFor(QStringLiteral("kylinlist"), updates::currentBuildPlatform(),
                QStringLiteral("9.9.9"), arch, variant), &error).isValid());
    QVERIFY(!updates::parseInstallInfoJson(
        jsonFor(QStringLiteral("kylinlist"), updates::currentBuildPlatform(), buildVersion,
                arch == QStringLiteral("arm64") ? QStringLiteral("amd64")
                                                : QStringLiteral("arm64"),
                variant), &error).isValid());
    QVERIFY(!updates::parseInstallInfoJson(
        jsonFor(QStringLiteral("kylinlist"), updates::currentBuildPlatform(), buildVersion,
                arch, QStringLiteral("bundle")), &error).isValid());
    QVERIFY(!updates::parseInstallInfoJson(QByteArrayLiteral("{"), &error).isValid());
}

void UpdatesTest::identityReadFromOverride()
{
    qputenv("KYLINLIST_UPDATE_INSTALL_INFO_PATH",
            (m_dir->path() + QStringLiteral("/missing.json")).toUtf8());
    QString error;
    QVERIFY(!updates::readInstallIdentity(&error).isValid());
    QVERIFY(!error.isEmpty());

    const QString variant = updates::currentBuildPlatform() == QStringLiteral("linux")
        ? QStringLiteral("system-qt") : QStringLiteral("setup");
    const QByteArray json = QStringLiteral(
        "{\"product\":\"kylinlist\",\"platform\":\"%1\",\"version\":\"%2\","
        "\"arch\":\"%3\",\"variant\":\"%4\"}")
        .arg(updates::currentBuildPlatform(), QStringLiteral(KYLINLIST_VERSION),
             updates::currentBuildArch(), variant)
        .toUtf8();
    const QString path = m_dir->path() + QStringLiteral("/install-info.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(json);
    file.close();
    qputenv("KYLINLIST_UPDATE_INSTALL_INFO_PATH", path.toUtf8());
    QVERIFY2(updates::readInstallIdentity(&error).isValid(), qPrintable(error));
}

void UpdatesTest::pendingEvaluation()
{
    using updates::PendingOutcome;
    QCOMPARE(updates::evaluatePendingInstall(QStringLiteral("1.2.0"), QString()),
             PendingOutcome::None);
    QCOMPARE(updates::evaluatePendingInstall(QStringLiteral("1.2.0"), QStringLiteral("garbage")),
             PendingOutcome::None);
    QCOMPARE(updates::evaluatePendingInstall(QStringLiteral("1.2.0"), QStringLiteral("1.2.0")),
             PendingOutcome::Completed);
    QCOMPARE(updates::evaluatePendingInstall(QStringLiteral("1.3.0"), QStringLiteral("1.2.0")),
             PendingOutcome::Superseded);
    QCOMPARE(updates::evaluatePendingInstall(QStringLiteral("1.1.0"), QStringLiteral("1.2.0")),
             PendingOutcome::StillPending);
}

void UpdatesTest::postponeDecision()
{
    const QDateTime now(QDate(2026, 11, 14), QTime(12, 0), Qt::UTC);
    QCOMPARE(updates::updatePromptPostponed(QDateTime(), now), false);   // 从未暂缓
    QCOMPARE(updates::updatePromptPostponed(now, QDateTime()), false);   // 当前时刻无效
    QCOMPARE(updates::updatePromptPostponed(now.addDays(1), now), true); // 暂缓未到期
    QCOMPARE(updates::updatePromptPostponed(now, now), false);           // 恰好到期：不再暂缓
    QCOMPARE(updates::updatePromptPostponed(now.addSecs(-1), now), false);
    QCOMPARE(updates::UpdatePostponeDays, 7);
}

void UpdatesTest::verifyInstallerFileChecks()
{
    const QByteArray body = QByteArrayLiteral("installer-content-0123456789");
    const QString path = m_dir->path() + QStringLiteral("/pkg.bin");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(body);
    file.close();
    QString error;
    QVERIFY(updates::verifyInstallerFile(path, body.size(), sha256Hex(body), &error));
    QVERIFY(!updates::verifyInstallerFile(path, body.size() + 1, sha256Hex(body), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!updates::verifyInstallerFile(path, body.size(), QString(64, QLatin1Char('0')), &error));
    QVERIFY(!updates::verifyInstallerFile(m_dir->path() + QStringLiteral("/nope.bin"), 1,
                                          sha256Hex(body), &error));
}

void UpdatesTest::settingsUpdateDefaults()
{
    Settings& settings = Settings::instance();
    QCOMPARE(Settings::defaultUpdateBaseUrl(), QStringLiteral("http://143.147.10.31:11924"));
    // 全新安装（键不存在）：默认服务器。
    QCOMPARE(settings.updateBaseUrl(), Settings::defaultUpdateBaseUrl());
    // 显式保存空串仍表示禁用。
    settings.setUpdateBaseUrl(QString());
    settings.sync();
    QCOMPARE(settings.updateBaseUrl(), QString());
    // 用户自定义地址按原样保留。
    settings.setUpdateBaseUrl(QStringLiteral("http://10.0.0.8:8080"));
    settings.sync();
    QCOMPARE(settings.updateBaseUrl(), QStringLiteral("http://10.0.0.8:8080"));
    // 暂缓时刻：默认无效，读写往返保持同一时刻。
    QCOMPARE(settings.updatePostponeUntil().isValid(), false);
    const QDateTime until(QDate(2026, 11, 21), QTime(8, 0), Qt::UTC);
    settings.setUpdatePostponeUntil(until);
    settings.sync();
    QCOMPARE(utcMs(settings.updatePostponeUntil()), utcMs(until));
    // 默认自启的一次性键：默认未初始化，写入后持久。
    QCOMPARE(settings.autostartInitialized(), false);
    settings.setAutostartInitialized(true);
    settings.sync();
    QCOMPARE(settings.autostartInitialized(), true);
}

void UpdatesTest::baseUrlNormalization()
{
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    service.setBaseUrl(QStringLiteral("  "));
    QCOMPARE(service.state(), updates::UpdateService::State::Disabled);
    service.setBaseUrl(QStringLiteral("192.168.1.10:8080"));
    QVERIFY(service.enabled());
    QVERIFY(service.baseUrl().startsWith(QStringLiteral("http://192.168.1.10:8080")));
    service.setBaseUrl(QStringLiteral("http://192.168.1.10:8080/pfx"));
    QVERIFY(service.baseUrl().endsWith(QStringLiteral("/pfx/")));
    service.setBaseUrl(QStringLiteral("ftp://192.168.1.10"));
    QCOMPARE(service.state(), updates::UpdateService::State::Disabled);
    QVERIFY(!service.errorText().isEmpty());
}

// ---------- 检查流程 ----------

void UpdatesTest::serviceCheckFindsUpdate()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(2048, 'x');
    const QString relative = QStringLiteral("files/9.9.9/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("9.9.9"), relative, artifact)});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::UpdateAvailable);
    QCOMPARE(finishedSpy.first().at(0).toBool(), true);
    QVERIFY(service.targetArtifact() != nullptr);
    QCOMPARE(service.targetArtifact()->path, relative);
    QCOMPARE(service.currentVersion(), QStringLiteral("1.0.0"));
}

void UpdatesTest::serviceCheckUpToDate()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(16, 'y');
    const QString relative = QStringLiteral("files/1.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("1.0.0"), relative, artifact)});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::UpToDate);
}

void UpdatesTest::serviceCheckHttpError()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    server.setRoute(manifestRoute(), {404, QByteArray()});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Failed);
    QVERIFY(service.errorText().contains(QStringLiteral("404")));
}

void UpdatesTest::serviceCheckTimeout()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    FixtureHttpServer::Response response;
    response.hang = true;
    server.setRoute(manifestRoute(), response);
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    service.setRequestTimeoutMs(300);
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Failed);
    QCOMPARE(service.errorText(), QStringLiteral("检查更新超时"));
}

void UpdatesTest::serviceCheckMissingTarget()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    // 清单只包含另一个目标（arm64/amd64 互换）。
    const QString otherArch = updates::currentBuildArch() == QStringLiteral("arm64")
        ? QStringLiteral("amd64") : QStringLiteral("arm64");
    const QByteArray artifact = QByteArray(16, 'z');
    const QString json = QStringLiteral(
        "{\"schemaVersion\":1,\"channel\":\"stable\",\"version\":\"9.9.9\","
        "\"publishedAt\":\"2026-11-13T08:00:00Z\",\"notes\":\"n\",\"artifacts\":["
        "{\"platform\":\"%1\",\"arch\":\"%2\",\"variant\":\"%3\",\"packageVersion\":\"9.9.9\","
        "\"path\":\"files/9.9.9/%1-%2-%3/pkg.deb\",\"size\":%4,\"sha256\":\"%5\"}]}")
        .arg(updates::currentBuildPlatform(), otherArch,
             makeIdentity(QStringLiteral("1.0.0")).variant)
        .arg(artifact.size())
        .arg(sha256Hex(artifact));
    server.setRoute(manifestRoute(), {200, json.toUtf8()});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Failed);
    QVERIFY(service.errorText().contains(QStringLiteral("没有提供")));
}

void UpdatesTest::serviceCheckNotConfigured()
{
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy finishedSpy(&service, &updates::UpdateService::checkFinished);
    service.checkNow();
    QVERIFY(waitFor(2000, [&]() { return finishedSpy.size() >= 1; }));
    QCOMPARE(finishedSpy.first().at(1).toString(), QStringLiteral("未配置更新服务器地址"));
    QCOMPARE(service.state(), updates::UpdateService::State::Disabled);
}

// ---------- 自动检查来源 ----------

void UpdatesTest::serviceCheckAutomaticSource()
{
    const QByteArray artifact = QByteArray(32, 's');
    const QString relative = QStringLiteral("files/9.9.9/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    const QByteArray manifestJson = buildManifestJson(QStringLiteral("9.9.9"), relative, artifact);

    // 手动检查：automatic == false。
    {
        FixtureHttpServer server;
        QVERIFY(server.start());
        server.setRoute(manifestRoute(), {200, manifestJson});
        updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
        QSignalSpy spy(&service, &updates::UpdateService::checkFinished);
        service.setBaseUrl(server.baseUrl());
        service.checkNow();
        QVERIFY(waitFor(5000, [&]() { return spy.size() >= 1; }));
        QCOMPARE(spy.first().at(2).toBool(), false);
        QCOMPARE(service.lastCheckAutomatic(), false);
    }
    // 自动检查（首查延时触发）：automatic == true。
    {
        FixtureHttpServer server;
        QVERIFY(server.start());
        server.setRoute(manifestRoute(), {200, manifestJson});
        updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
        service.setAutomaticCheckTiming(150, 3600 * 1000);
        QSignalSpy spy(&service, &updates::UpdateService::checkFinished);
        service.setBaseUrl(server.baseUrl());
        service.startAutomaticChecks();
        QVERIFY(waitFor(5000, [&]() { return spy.size() >= 1; }));
        QCOMPARE(spy.first().at(2).toBool(), true);
        QCOMPARE(service.lastCheckAutomatic(), true);
        service.stopAutomaticChecks();
    }
    // 自动检查在途时手动触发：不重复发请求，结果来源保持为自动。
    {
        FixtureHttpServer server;
        QVERIFY(server.start());
        FixtureHttpServer::Response response;
        response.body = manifestJson;
        response.delayMs = 900;  // 让自动检查保持在途
        server.setRoute(manifestRoute(), response);
        updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
        service.setAutomaticCheckTiming(100, 3600 * 1000);
        QSignalSpy spy(&service, &updates::UpdateService::checkFinished);
        service.setBaseUrl(server.baseUrl());
        service.startAutomaticChecks();
        QVERIFY(waitFor(2000, [&]() {
            return service.state() == updates::UpdateService::State::Checking;
        }));
        service.checkNow();  // 并发手动检查：不卡住、不错标
        QVERIFY(waitFor(5000, [&]() { return spy.size() >= 1; }));
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().at(2).toBool(), true);
        QCOMPARE(service.state(), updates::UpdateService::State::UpdateAvailable);
        service.stopAutomaticChecks();
    }
    // 首查排程后立刻停止自动检查：已排程的首查作废，不产生任何结果。
    {
        FixtureHttpServer server;
        QVERIFY(server.start());
        server.setRoute(manifestRoute(), {200, manifestJson});
        updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
        service.setAutomaticCheckTiming(150, 3600 * 1000);
        QSignalSpy spy(&service, &updates::UpdateService::checkFinished);
        service.setBaseUrl(server.baseUrl());
        service.startAutomaticChecks();
        service.stopAutomaticChecks();
        QTest::qWait(700);
        QCOMPARE(spy.size(), 0);
        QCOMPARE(service.state(), updates::UpdateService::State::Idle);
    }
    // 首查等待期间重复启动只保留一个排程，避免每次保存设置都追加请求。
    {
        FixtureHttpServer server;
        QVERIFY(server.start());
        server.setRoute(manifestRoute(), {200, manifestJson});
        updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
        service.setAutomaticCheckTiming(250, 3600 * 1000);
        QSignalSpy spy(&service, &updates::UpdateService::checkFinished);
        service.setBaseUrl(server.baseUrl());
        service.startAutomaticChecks();
        QTest::qWait(100);
        service.startAutomaticChecks();
        QVERIFY(waitFor(5000, [&]() { return spy.size() >= 1; }));
        QTest::qWait(400);
        QCOMPARE(spy.size(), 1);
        service.stopAutomaticChecks();
    }
}

void UpdatesTest::serviceAutomaticSkipsWhenReady()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(512, 'r');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("2.0.0"), relative, artifact)});
    server.setRoute(QStringLiteral("/updates/") + relative, {200, artifact});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy doneSpy(&service, &updates::UpdateService::downloadSucceeded);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return doneSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Ready);
    // Ready 状态下自动检查不得重复发起。
    service.setAutomaticCheckTiming(150, 3600 * 1000);
    service.startAutomaticChecks();
    QTest::qWait(700);
    QCOMPARE(checkSpy.size(), 1);
    QCOMPARE(service.state(), updates::UpdateService::State::Ready);
    service.stopAutomaticChecks();
}

// ---------- 下载流程 ----------

void UpdatesTest::serviceDownloadSucceeds()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(4096, 'a');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("2.0.0"), relative, artifact)});
    server.setRoute(QStringLiteral("/updates/") + relative, {200, artifact});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy doneSpy(&service, &updates::UpdateService::downloadSucceeded);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return doneSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Ready);
    const QString path = service.verifiedFilePath();
    QVERIFY(!path.isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), artifact);
    QVERIFY(!QFile::exists(path + QStringLiteral(".part")));
}

void UpdatesTest::serviceDownloadHashMismatch()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(2048, 'b');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    QByteArray manifest = buildManifestJson(QStringLiteral("2.0.0"), relative, artifact);
    manifest.replace(sha256Hex(artifact).toLatin1(), QString(64, QLatin1Char('1')).toLatin1());
    server.setRoute(manifestRoute(), {200, manifest});
    server.setRoute(QStringLiteral("/updates/") + relative, {200, artifact});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy failSpy(&service, &updates::UpdateService::downloadFailed);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return failSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::UpdateAvailable);
    QVERIFY(service.errorText().contains(QStringLiteral("校验")));
    QVERIFY(service.verifiedFilePath().isEmpty());
}

void UpdatesTest::serviceDownloadSizeMismatch()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(1024, 'c');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    QByteArray manifest = buildManifestJson(QStringLiteral("2.0.0"), relative, artifact);
    manifest.replace(QStringLiteral("\"size\": 1024").toLatin1(),
                     QStringLiteral("\"size\": 2048").toLatin1());
    server.setRoute(manifestRoute(), {200, manifest});
    server.setRoute(QStringLiteral("/updates/") + relative, {200, artifact});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy failSpy(&service, &updates::UpdateService::downloadFailed);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return failSpy.size() >= 1; }));
    QVERIFY(service.errorText().contains(QStringLiteral("大小不符")));
}

void UpdatesTest::serviceDownloadCancel()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(4096, 'd');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("2.0.0"), relative, artifact)});
    FixtureHttpServer::Response partial;
    partial.body = artifact;
    partial.partialBytes = 64;  // 只发一小段后停住，模拟慢速下载
    server.setRoute(QStringLiteral("/updates/") + relative, partial);
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy failSpy(&service, &updates::UpdateService::downloadFailed);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return service.state() == updates::UpdateService::State::Downloading; }));
    service.cancelDownload();
    QVERIFY(waitFor(5000, [&]() { return failSpy.size() >= 1; }));
    QCOMPARE(failSpy.first().at(0).toString(), QStringLiteral("下载已取消"));
    QCOMPARE(service.state(), updates::UpdateService::State::UpdateAvailable);
    QDir cache(m_dir->path() + QStringLiteral("/updates/cache"));
    QCOMPARE(cache.entryList(QDir::Files).size(), 0);
}

void UpdatesTest::serviceDownloadStall()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(4096, 'e');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(), {200, buildManifestJson(QStringLiteral("2.0.0"), relative, artifact)});
    FixtureHttpServer::Response partial;
    partial.body = artifact;
    partial.partialBytes = 64;
    server.setRoute(QStringLiteral("/updates/") + relative, partial);
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    service.setStallTimeoutMs(300);
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy failSpy(&service, &updates::UpdateService::downloadFailed);
    service.setBaseUrl(server.baseUrl());
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    service.download();
    QVERIFY(waitFor(5000, [&]() { return failSpy.size() >= 1; }));
    QVERIFY(service.errorText().contains(QStringLiteral("没有进展")));
}

void UpdatesTest::serviceRespectsBasePrefix()
{
    FixtureHttpServer server;
    QVERIFY(server.start());
    const QByteArray artifact = QByteArray(64, 'f');
    const QString relative = QStringLiteral("files/2.0.0/%1-%2-%3/pkg%4")
        .arg(updates::currentBuildPlatform(), updates::currentBuildArch(),
             makeIdentity(QStringLiteral("1.0.0")).variant, artifactSuffix());
    server.setRoute(manifestRoute(QStringLiteral("/pfx")),
                    {200, buildManifestJson(QStringLiteral("2.0.0"), relative, artifact)});
    server.setRoute(QStringLiteral("/pfx/updates/") + relative, {200, artifact});
    updates::UpdateService service(makeIdentity(QStringLiteral("1.0.0")));
    QSignalSpy checkSpy(&service, &updates::UpdateService::checkFinished);
    QSignalSpy doneSpy(&service, &updates::UpdateService::downloadSucceeded);
    service.setBaseUrl(server.baseUrl() + QStringLiteral("/pfx"));
    service.checkNow();
    QVERIFY(waitFor(5000, [&]() { return checkSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::UpdateAvailable);
    service.download();
    QVERIFY(waitFor(5000, [&]() { return doneSpy.size() >= 1; }));
    QCOMPARE(service.state(), updates::UpdateService::State::Ready);
}

void UpdatesTest::installerHandOffFake()
{
    const QString packagePath = m_dir->path() + QStringLiteral("/pkg")
        + (updates::currentBuildPlatform() == QStringLiteral("linux")
               ? QStringLiteral(".deb") : QStringLiteral(".exe"));
    QFile package(packagePath);
    QVERIFY(package.open(QIODevice::WriteOnly));
    package.write("pkg");
    package.close();

    QString scriptPath;
#ifdef Q_OS_WIN
    scriptPath = m_dir->path() + QStringLiteral("/fake-installer.bat");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("@echo off\r\n>\"%~dp0update-marker.txt\" echo %1\r\n");
    script.close();
#else
    scriptPath = m_dir->path() + QStringLiteral("/fake-installer.sh");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Text));
    script.write("#!/bin/sh\nprintf '%s' \"$1\" > \"$(dirname \"$0\")/update-marker.txt\"\n");
    script.close();
#endif
    qputenv("KYLINLIST_UPDATE_FAKE_INSTALLER", scriptPath.toUtf8());
    QString error;
    QVERIFY2(updates::handOffToInstaller(packagePath, &error), qPrintable(error));
    const QString markerPath = QFileInfo(scriptPath).absolutePath() + QStringLiteral("/update-marker.txt");
    bool appeared = false;
    for (int i = 0; i < 100 && !appeared; ++i) {
        QTest::qSleep(50);
        QCoreApplication::processEvents();
        QFile marker(markerPath);
        if (marker.exists() && marker.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(marker.readAll()).trimmed();
            if (content.contains(QStringLiteral("pkg"))) appeared = true;
        }
    }
    QVERIFY2(appeared, "fake installer marker not found");
    // 文件不存在时交接必须失败。
    QVERIFY(!updates::handOffToInstaller(m_dir->path() + QStringLiteral("/no-such-file.deb"), &error));
}

// ---------- 自启共享策略（全部走替身后端，不触碰真实注册表/自启文件） ----------

// Installed 首次运行：默认登记一次并落盘标记；重复初始化不再触碰后端。
void UpdatesTest::autostartFirstDefaultRegistersOnce()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    AutostartSettings settings(AutostartSettings::Mode::Installed, fake.backend());
    QCOMPARE(settings.mode(), AutostartSettings::Mode::Installed);
    QVERIFY(settings.isAvailable());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QString error;
    QVERIFY(settings.initializeDefault(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(fake.setCalls, 1);  // 默认登记恰好写一次
    QVERIFY(settings.isEnabled());
    QVERIFY(Settings::instance().autostartInitialized());  // 成功后标记落盘
    QCOMPARE(spy.size(), 1);     // 状态从无到有要通知界面
    // 同一会话再次初始化：标记短路，不重复登记。
    QVERIFY(settings.initializeDefault(&error));
    QCOMPARE(fake.setCalls, 1);
    QCOMPARE(spy.size(), 1);
}

// 升级前版本已留下自启项：只补标记，不重写内容，也无状态变化信号。
void UpdatesTest::autostartAlreadyRegisteredMarksWithoutWrite()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    fake.installed = true;
    AutostartSettings settings(AutostartSettings::Mode::Installed, fake.backend());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QString error;
    QVERIFY(settings.initializeDefault(&error));
    QCOMPARE(fake.setCalls, 0);  // 已存在：不重写
    QVERIFY(settings.isEnabled());
    QVERIFY(Settings::instance().autostartInitialized());
    QCOMPARE(spy.size(), 0);
}

// 默认登记写入失败：透出错误、标记不落盘；下次启动（新会话对象）可重试。
void UpdatesTest::autostartFailedInitializationRetries()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    fake.failSet = true;
    QString error;
    {
        AutostartSettings settings(AutostartSettings::Mode::Installed, fake.backend());
        QVERIFY(!settings.initializeDefault(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!Settings::instance().autostartInitialized());  // 失败保持可重试
        QVERIFY(!settings.isEnabled());
    }
    // 模拟重启：新的会话对象 + 替身后端恢复可用。
    fake.failSet = false;
    AutostartSettings next(AutostartSettings::Mode::Installed, fake.backend());
    QVERIFY(next.initializeDefault(&error));
    QCOMPARE(fake.setCalls, 2);  // 上次 1 次 + 本次 1 次
    QVERIFY(next.isEnabled());
    QVERIFY(Settings::instance().autostartInitialized());
}

// 用户显式关闭后：标记保持落盘，下一次初始化不得复活自启。
void UpdatesTest::autostartExplicitDisablePersists()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    AutostartSettings settings(AutostartSettings::Mode::Installed, fake.backend());
    QString error;
    QVERIFY(settings.initializeDefault(&error));
    QVERIFY(settings.isEnabled());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QVERIFY(settings.setEnabled(false, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!settings.isEnabled());
    QVERIFY(Settings::instance().autostartInitialized());  // 显式选择落盘
    QCOMPARE(spy.size(), 1);
    // 模拟重启：标记已落盘，即使自启项不存在也不得自动再登记。
    FakeAutostart carried;  // installed == false，与上一会话的显式关闭一致
    AutostartSettings next(AutostartSettings::Mode::Installed, carried.backend());
    QVERIFY(next.initializeDefault(&error));
    QCOMPARE(carried.setCalls, 0);
    QCOMPARE(carried.installedCalls, 0);  // 标记短路：连后端读都不发生
    QVERIFY(!next.isEnabled());  // isEnabled 反映真实登记，而非标记
}

// Isolated/Unavailable：任何路径都不触达后端与一次性标记。
void UpdatesTest::autostartIsolatedAndUnavailableInert()
{
    setAutostartMarker(false);
    const AutostartSettings::Mode modes[] = {
        AutostartSettings::Mode::Isolated, AutostartSettings::Mode::Unavailable};
    for (const AutostartSettings::Mode mode : modes) {
        FakeAutostart fake;
        AutostartSettings settings(mode, fake.backend());
        QCOMPARE(settings.mode(), mode);
        QVERIFY(!settings.isAvailable());
        QVERIFY(!settings.isEnabled());
        QString error;
        QVERIFY(!settings.setEnabled(true, &error));  // 显式开关也不触达后端
        QVERIFY(!error.isEmpty());
        QVERIFY(settings.initializeDefault(&error));  // 无事可做：成功且不报错
        QVERIFY(error.isEmpty());
        QCOMPARE(fake.installedCalls, 0);
        QCOMPARE(fake.setCalls, 0);
        QVERIFY(!Settings::instance().autostartInitialized());  // 也不落盘标记
    }
}

// Preview：开发预览允许用户手动登记（登录命令由 KylinListApp 固定带上
// --data-dir），但 initializeDefault 绝不自动登记，不落一次性标记。
void UpdatesTest::autostartPreviewOptInWithoutDefault()
{
    setAutostartMarker(false);
    const QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    const QString previewDir = fixture.filePath(QStringLiteral("预览 合成数据"));
    const QStringList loginArgs{QStringLiteral("--background"), QStringLiteral("--data-dir"),
                                previewDir, QStringLiteral("--preview-autostart")};
    QCOMPARE(Autostart::previewArguments(previewDir), loginArgs);
    QVERIFY(Autostart::previewFilePath() != Autostart::filePath());
    AutostartSettings isolatedPreview(AutostartSettings::Mode::Preview);
    QVERIFY(!isolatedPreview.isAvailable());
    AutostartSettings explicitPreview(AutostartSettings::Mode::Preview, previewDir);
    QVERIFY(explicitPreview.isAvailable());  // 仅验证接线，不查询系统条目
    QCOMPARE(explicitPreview.previewDataDir(), previewDir);

    FakeAutostart fake;
    AutostartSettings settings(AutostartSettings::Mode::Preview, fake.backend());
    QCOMPARE(settings.mode(), AutostartSettings::Mode::Preview);
    QVERIFY(settings.isAvailable());
    QString error;
    QVERIFY(settings.initializeDefault(&error));
    QVERIFY(error.isEmpty());
    QCOMPARE(fake.setCalls, 0);                    // 预览没有默认登记
    QCOMPARE(fake.installedCalls, 0);               // 也不查询后端
    QVERIFY(!Settings::instance().autostartInitialized());
    QVERIFY(!settings.isEnabled());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QVERIFY(settings.setEnabled(true, &error));     // 用户点击后允许登记
    QVERIFY(settings.isEnabled());
    QCOMPARE(fake.setCalls, 1);
    QCOMPARE(spy.size(), 1);
    QVERIFY(Settings::instance().autostartInitialized());
}

// Windows Run 命令行：含空格的参数必须加引号，否则 --data-dir 会被拆成
// 两个参数，登录后落回默认数据目录。反斜杠按命令解析规则转义。
void UpdatesTest::autostartWindowsCommandQuotesArguments()
{
#ifdef Q_OS_WIN
    const QString cmd = Autostart::commandLineFor(
        QStringLiteral("C:/Program Files/KylinList/kylinlist.exe"),
        {QStringLiteral("--background"), QStringLiteral("--data-dir"),
         QStringLiteral("C:/Users/A B/麒麟 预览")});
    QCOMPARE(cmd, QStringLiteral("\"C:\\Program Files\\KylinList\\kylinlist.exe\" "
                                 "--background --data-dir \"C:/Users/A B/麒麟 预览\""));
    QCOMPARE(Autostart::commandLineFor(QStringLiteral("x.exe"), {QStringLiteral("a\"b")}),
             QStringLiteral("\"x.exe\" \"a\\\"b\""));
    QCOMPARE(Autostart::commandLineFor(QStringLiteral("x.exe"),
                                       {QStringLiteral("C:\\dir with space\\")}),
             QStringLiteral("\"x.exe\" \"C:\\dir with space\\\\\""));
#else
    QSKIP("Windows Run 命令行只在 Windows 构建验证");
#endif
}

// Portable：绝不默认登记（initializeDefault 零写入、零标记）；显式开启才登记，
// 成功的显式选择同样落盘标记，避免未来自动初始化覆盖用户决定。
void UpdatesTest::autostartPortableOptInOnly()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    AutostartSettings settings(AutostartSettings::Mode::Portable, fake.backend());
    QVERIFY(settings.isAvailable());
    QString error;
    QVERIFY(settings.initializeDefault(&error));
    QCOMPARE(fake.setCalls, 0);  // 便携版没有自动登记
    QVERIFY(!Settings::instance().autostartInitialized());  // 也不写标记
    QVERIFY(!settings.isEnabled());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QVERIFY(settings.setEnabled(true, &error));  // 显式开启允许
    QVERIFY(settings.isEnabled());
    QVERIFY(Settings::instance().autostartInitialized());  // 显式选择落盘
    QCOMPARE(fake.setCalls, 1);
    QCOMPARE(spy.size(), 1);
}

// 后端失败：错误透出、标记不落盘；真实状态变化（半写成功）时仍发信号供界面重读。
void UpdatesTest::autostartBackendFailureSurfacesError()
{
    setAutostartMarker(false);
    FakeAutostart fake;
    fake.mutateThenFail = true;
    AutostartSettings settings(AutostartSettings::Mode::Portable, fake.backend());
    QSignalSpy spy(&settings, &AutostartSettings::enabledChanged);
    QString error;
    QVERIFY(!settings.setEnabled(true, &error));
    QVERIFY(error.contains(QStringLiteral("测试")));  // 后端错误原样透出
    QVERIFY(settings.isEnabled());  // 以真实登记为准：确实已写入
    QCOMPARE(spy.size(), 1);        // 状态变了要通知界面重读
    QVERIFY(!Settings::instance().autostartInitialized());  // 失败不落盘标记
    // 纯失败（状态未变）：返回错误但不发信号。
    fake.mutateThenFail = false;
    fake.failSet = true;
    QVERIFY(!settings.setEnabled(false, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(settings.isEnabled());
    QCOMPARE(spy.size(), 1);
}

QTEST_MAIN(UpdatesTest)
#include "tst_updates.moc"
