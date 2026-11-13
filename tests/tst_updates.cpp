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
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "Common.h"
#include "app/updates/InstallIdentity.h"
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

} // namespace

class UpdatesTest : public QObject {
    Q_OBJECT
private slots:
    void init() { m_dir.reset(new QTemporaryDir); QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8()); }
    void cleanup() { qputenv("KYLINLIST_UPDATE_INSTALL_INFO_PATH", "");
        qputenv("KYLINLIST_UPDATE_FAKE_INSTALLER", ""); m_dir.reset(); }

    void versionParsing_data();
    void versionParsing();
    void versionCompare();
    void manifestParsing();
    void artifactPathValidation();
    void fullReleaseManifestParses();
    void manifestTargetSelection();
    void identityParsing();
    void identityReadFromOverride();
    void pendingEvaluation();
    void verifyInstallerFileChecks();
    void baseUrlNormalization();
    void serviceCheckFindsUpdate();
    void serviceCheckUpToDate();
    void serviceCheckHttpError();
    void serviceCheckTimeout();
    void serviceCheckMissingTarget();
    void serviceCheckNotConfigured();
    void serviceDownloadSucceeds();
    void serviceDownloadHashMismatch();
    void serviceDownloadSizeMismatch();
    void serviceDownloadCancel();
    void serviceDownloadStall();
    void serviceRespectsBasePrefix();
    void installerHandOffFake();

private:
    QScopedPointer<QTemporaryDir> m_dir;
};

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

QTEST_MAIN(UpdatesTest)
#include "tst_updates.moc"
