#include "app/DemoData.h"
#include "app/SingleInstanceGuard.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/AttachmentCodec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

using namespace kl;

// 覆盖：生成、校验、备份、manifest、安全/锁行为。全部只依赖 kylinlist_core。
class DemoDataTests : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();

    void generateCreatesVerifiedDemoData();
    void generateRejectsNonEmptyDirectory();
    void generateRejectsRelativeAndRootPaths();
    void generateFailsWhileDirectoryLocked();

    void populateWorksAgainstCustomTaskStore();
    void manifestRoundTrips();
    void manifestRejectsForeignOwnership();

    void verifyWritesNothingIntoNonDemoDirectory();
    void verifyDetectsRemovedAttachment();
    void verifyFailsWhileDirectoryLocked();

private:
    std::unique_ptr<QTemporaryDir> m_directory;
};

namespace {
QStringList dirEntries(const QString &path)
{
    return QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}
} // namespace

void DemoDataTests::init()
{
    Database::instance().close();
    qunsetenv("KYLINLIST_DATA_DIR");
    m_directory.reset(new QTemporaryDir);
    QVERIFY(m_directory->isValid());
}

void DemoDataTests::cleanup()
{
    Database::instance().close();
    qunsetenv("KYLINLIST_DATA_DIR");
}

void DemoDataTests::generateCreatesVerifiedDemoData()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("demo"));
    QString error;
    bool locked = true;
    QVERIFY2(demo::generate(dataDir, &error, &locked), qPrintable(error));
    QVERIFY(!locked);

    // manifest：所有权 + 数量 + 关键 id。
    demo::Manifest manifest;
    QVERIFY2(demo::readManifest(dataDir, &manifest, &error), qPrintable(error));
    QCOMPARE(manifest.format, demo::manifestFormat());
    QCOMPARE(manifest.marker, demo::ownershipMarker());
    QVERIFY(manifest.owned);
    QVERIFY(manifest.canReset);
    QVERIFY(manifest.generatedAtUtc.isValid());
    QVERIFY(manifest.referenceDate == toAppTime(appNow()).date());
    QVERIFY(manifest.entities > 20);
    QVERIFY(manifest.tasks > 20);
    QVERIFY(manifest.notes >= 4);
    QVERIFY(manifest.lists >= 8);
    QVERIFY(manifest.sections == 3);
    QVERIFY(manifest.attachments == 5);
    QVERIFY(manifest.desktopEntityIds.size() == 6);
    QVERIFY(manifest.completedTaskIds.size() == 4);
    QVERIFY(manifest.abandonedTaskIds.size() == 2);
    QVERIFY(manifest.trashedTaskIds.size() == 2);
    QVERIFY(manifest.recurrenceSeries == 7);
    QVERIFY(!manifest.recurringSourceId.isEmpty());
    QVERIFY(!manifest.recurringNextId.isEmpty());
    QVERIFY(!manifest.backupSha256.isEmpty());

    // 备份：KLB1 容器且与 manifest 哈希一致。
    QFile backup(demo::backupPath(dataDir));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    const QByteArray bytes = backup.readAll();
    backup.close();
    QVERIFY(bytes.size() > 16);
    QVERIFY(attachmentcodec::isBackupContainer(bytes));
    QCOMPARE(bytes.size(), static_cast<int>(manifest.backupBytes));

    // 校验：全新 TaskStore 重开 + 完整报告。
    QString report;
    QVERIFY2(demo::verify(dataDir, &report, &error), qPrintable(error));
    QVERIFY(report.contains(QStringLiteral("RESULT: PASS")));
    QFile reportFile(demo::reportPath(dataDir));
    QVERIFY(reportFile.exists());

    // 环境变量必须被恢复（不泄漏 KYLINLIST_DATA_DIR）。
    QVERIFY(qEnvironmentVariable("KYLINLIST_DATA_DIR").isEmpty());
}

void DemoDataTests::generateRejectsNonEmptyDirectory()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("occupied"));
    QVERIFY(QDir().mkpath(dataDir));
    QFile stray(dataDir + QStringLiteral("/keep.txt"));
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.write("user data");
    stray.close();
    QString error;
    bool locked = false;
    QVERIFY(!demo::generate(dataDir, &error, &locked));
    QVERIFY(!locked);
    QVERIFY(error.contains(QStringLiteral("必须为空")));
    QVERIFY(!QFile::exists(demo::manifestPath(dataDir)));
    QVERIFY(!QFile::exists(dataDir + QStringLiteral("/kylinlist.db")));
    QCOMPARE(dirEntries(dataDir), QStringList() << QStringLiteral("keep.txt"));
}

void DemoDataTests::generateRejectsRelativeAndRootPaths()
{
    QString error;
    bool locked = false;
    QVERIFY(!demo::generate(QStringLiteral("relative-demo-dir"), &error, &locked));
    QVERIFY(error.contains(QStringLiteral("绝对路径")));
    QVERIFY(!demo::generate(QDir::rootPath(), &error, &locked));
    QVERIFY(error.contains(QStringLiteral("根目录")));
    QVERIFY(!demo::generate(QString(), &error, &locked));
}

void DemoDataTests::generateFailsWhileDirectoryLocked()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(dataDir));
    qputenv("KYLINLIST_DATA_DIR", dataDir.toUtf8());
    SingleInstanceGuard guard(lockFilePath());
    QCOMPARE(guard.acquire(), SingleInstanceGuard::Acquired);
    QString error;
    bool locked = false;
    QVERIFY(!demo::generate(dataDir, &error, &locked));
    QVERIFY(locked);
    QVERIFY(error.contains(QStringLiteral("其他实例")));
    guard.release();
    // 锁释放后可正常生成。
    QVERIFY2(demo::generate(dataDir, &error, &locked), qPrintable(error));
}

void DemoDataTests::populateWorksAgainstCustomTaskStore()
{
    // DemoData 直接可用调用方自己的 TaskStore 驱动（同一份写入路径，无直接 SQL）。
    const QString dataDir = m_directory->filePath(QStringLiteral("manual"));
    QVERIFY(QDir().mkpath(dataDir));
    qputenv("KYLINLIST_DATA_DIR", dataDir.toUtf8());
    QString error;
    QVERIFY2(Database::instance().open(&error), qPrintable(error));
    {
        TaskStore store(Database::instance().connection());
        QVERIFY2(store.load(&error), qPrintable(error));
        demo::Manifest manifest;
        QVERIFY2(demo::populate(store, appNow(), &manifest, &error), qPrintable(error));
        QCOMPARE(static_cast<int>(store.lists().size()), manifest.lists);
        QCOMPARE(static_cast<int>(store.entities().size()), manifest.entities);
        QCOMPARE(manifest.lists, 8);
        QCOMPARE(manifest.folders, 3);
        QCOMPARE(manifest.attachments, 5);
        // 提醒：Start/End/Schedule 三种锚点 + 绝对 + 全天 + 笔记提醒（当前共 6 条），
        // 数量以实时汇总为准，避免与数据集演进脱节。
        int remindersSeen = 0;
        for (const Entity &value : store.entities()) remindersSeen += value.reminders.size();
        QCOMPARE(remindersSeen, manifest.reminders);
        QVERIFY(manifest.reminders >= 6);
        QCOMPARE(manifest.recurrenceSeries, 7);
        // 后继实例：同 series、active、锚定明日。
        const Entity source = store.entity(manifest.recurringSourceId);
        const Entity next = store.entity(manifest.recurringNextId);
        QCOMPARE(source.status, TaskStatus::Completed);
        QCOMPARE(next.status, TaskStatus::Active);
        QCOMPARE(source.recurrenceSeriesId, next.recurrenceSeriesId);
        QCOMPARE(next.allDayDate, manifest.referenceDate.addDays(1));
        QVERIFY(!store.inboxListId().isEmpty());
        QVERIFY(manifest.inboxListId == store.inboxListId());
        for (const QString &id : manifest.archivedListIds)
            QVERIFY(store.isListArchived(id));
    }
    Database::instance().close();
}

void DemoDataTests::manifestRoundTrips()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("roundtrip"));
    QString error;
    QVERIFY2(demo::generate(dataDir, &error), qPrintable(error));
    demo::Manifest first;
    QVERIFY2(demo::readManifest(dataDir, &first, &error), qPrintable(error));
    const QByteArray json = demo::manifestToJson(first).toUtf8();
    demo::Manifest second;
    QVERIFY2(demo::manifestFromJson(json, &second, &error), qPrintable(error));
    QCOMPARE(second.format, first.format);
    QCOMPARE(second.version, first.version);
    QCOMPARE(second.marker, first.marker);
    QCOMPARE(second.owned, first.owned);
    QCOMPARE(second.canReset, first.canReset);
    QCOMPARE(second.generatedAtUtc, first.generatedAtUtc);
    QCOMPARE(second.referenceDate, first.referenceDate);
    QCOMPARE(second.entities, first.entities);
    QCOMPARE(second.attachments, first.attachments);
    QCOMPARE(second.events, first.events);
    QCOMPARE(second.taskIds, first.taskIds);
    QCOMPARE(second.noteIds, first.noteIds);
    QCOMPARE(second.desktopEntityIds, first.desktopEntityIds);
    QCOMPARE(second.recurringNextId, first.recurringNextId);
    QCOMPARE(second.backupSha256, first.backupSha256);
    QCOMPARE(second.scheduleMidnightId, first.scheduleMidnightId);
    QCOMPARE(second.scheduleSameDayAllDayId, first.scheduleSameDayAllDayId);
    QCOMPARE(second.reminderScheduleAnchorId, first.reminderScheduleAnchorId);
}

void DemoDataTests::manifestRejectsForeignOwnership()
{
    const QByteArray foreign = QByteArrayLiteral(
        "{\"format\":\"kylinlist.demo-manifest\",\"version\":1,"
        "\"ownership\":{\"marker\":\"something-else\",\"owned\":true,\"canReset\":true}}");
    demo::Manifest manifest;
    QString error;
    QVERIFY(!demo::manifestFromJson(foreign, &manifest, &error));
    QVERIFY(error.contains(QStringLiteral("所有权")));
    const QByteArray disowned = QByteArrayLiteral(
        "{\"format\":\"kylinlist.demo-manifest\",\"version\":1,"
        "\"ownership\":{\"marker\":\"kylinlist-synthetic-demo\",\"owned\":false,\"canReset\":true}}");
    QVERIFY(!demo::manifestFromJson(disowned, &manifest, &error));
}

void DemoDataTests::verifyWritesNothingIntoNonDemoDirectory()
{
    // 阶段一（usage/manifest）失败绝不写任何文件：不证明所有权前不动目录。
    const QString dataDir = m_directory->filePath(QStringLiteral("plain"));
    QVERIFY(QDir().mkpath(dataDir));
    QFile user(dataDir + QStringLiteral("/user-file.txt"));
    QVERIFY(user.open(QIODevice::WriteOnly));
    user.write("user");
    user.close();
    const QStringList before = dirEntries(dataDir);

    QString report, error;
    QVERIFY(!demo::verify(dataDir, &report, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(report.contains(QStringLiteral("RESULT: FAIL")));
    QCOMPARE(dirEntries(dataDir), before);            // 无报告文件写入
    QVERIFY(!QFile::exists(demo::reportPath(dataDir)));
    QVERIFY(!QFile::exists(demo::manifestPath(dataDir)));

    // 伪 manifest（他方所有权）同样不写。
    QFile fake(dataDir + QStringLiteral("/demo-manifest.json"));
    QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write("{\"format\":\"other\",\"version\":9}");
    fake.close();
    const QStringList beforeFake = dirEntries(dataDir);
    QVERIFY(!demo::verify(dataDir, &report, &error));
    QCOMPARE(dirEntries(dataDir), beforeFake);
    QVERIFY(!QFile::exists(demo::reportPath(dataDir)));
}

void DemoDataTests::verifyDetectsRemovedAttachment()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("tampered"));
    QString error;
    QVERIFY2(demo::generate(dataDir, &error), qPrintable(error));
    // 删除一个附件文件：校验必须失败并给出定位。
    const QString attachments = dataDir + QStringLiteral("/attachments");
    const QStringList files = QDir(attachments).entryList(QDir::Files);
    QVERIFY(files.size() >= 5);
    QVERIFY(QFile::remove(attachments + QLatin1Char('/') + files.first()));
    QString report;
    QVERIFY(!demo::verify(dataDir, &report, &error));
    QVERIFY(error.contains(QStringLiteral("verify:")));
    QVERIFY(report.contains(QStringLiteral("RESULT: FAIL")));
    QVERIFY(QFile::exists(demo::reportPath(dataDir)));
}

void DemoDataTests::verifyFailsWhileDirectoryLocked()
{
    const QString dataDir = m_directory->filePath(QStringLiteral("verify-locked"));
    QString error;
    QVERIFY2(demo::generate(dataDir, &error), qPrintable(error));
    qputenv("KYLINLIST_DATA_DIR", dataDir.toUtf8());
    SingleInstanceGuard guard(lockFilePath());
    QCOMPARE(guard.acquire(), SingleInstanceGuard::Acquired);
    QString report;
    bool locked = false;
    QVERIFY(!demo::verify(dataDir, &report, &error, &locked));
    QVERIFY(locked);                                   // 与 CLI 退出码 3 对应
    QVERIFY(error.contains(QStringLiteral("其他实例")));
    guard.release();
    locked = true;
    QVERIFY2(demo::verify(dataDir, &report, &error, &locked), qPrintable(error));
    QVERIFY(!locked);
}

QTEST_GUILESS_MAIN(DemoDataTests)
#include "tst_demo.moc"
