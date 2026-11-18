// 开发阶段的当前 schema / 备份契约。全部数据库均为临时夹具，不触碰真实用户库。
#include "db/Database.h"
#include "db/TaskRepository.h"
#include "domain/BackupCodec.h"
#include "domain/RecurrenceRule.h"
#include "domain/ReminderRule.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest>

using namespace kl;

namespace {

int connectionCounter = 0;

bool seed(const QString &path, const QStringList &statements)
{
    const QString name = QStringLiteral("database_seed_%1").arg(++connectionCounter);
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        ok = db.open();
        for (const QString &statement : statements) {
            if (!ok) break;
            QSqlQuery query(db);
            ok = query.exec(statement);
            if (!ok) qWarning() << query.lastError().text() << statement;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

QVariant inspect(const QString &path, const QString &sql)
{
    const QString name = QStringLiteral("database_inspect_%1").arg(++connectionCounter);
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) value = query.value(0);
            else qWarning() << query.lastError().text() << sql;
        } else qWarning() << db.lastError().text();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList directoryEntries(const QString &path)
{
    return QDir(QFileInfo(path).absolutePath()).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

struct SnapshotFixture {
    EntitySnapshot snapshot;
    QVector<EntityEvent> events;
};

SnapshotFixture makeSnapshotFixture()
{
    SnapshotFixture fixture;
    const QDateTime now(QDate(2026, 1, 1), QTime(10, 0), Qt::UTC);
    Folder folder;
    folder.id = QStringLiteral("70000000-0000-4000-8000-000000000005");
    folder.name = QStringLiteral("文件夹");
    folder.icon = QStringLiteral("夹");
    folder.createdAt = now;
    folder.updatedAt = now;
    fixture.snapshot.folders.append(folder);
    TaskList inbox;
    inbox.id = QStringLiteral("70000000-0000-4000-8000-000000000001");
    inbox.name = QStringLiteral("收集箱");
    inbox.inbox = true;
    inbox.icon = QStringLiteral("箱");
    inbox.color = QStringLiteral("#123456");
    inbox.createdAt = now;
    inbox.updatedAt = now;
    fixture.snapshot.lists.append(inbox);
    StoredEntity stored;
    stored.entity.id = QStringLiteral("70000000-0000-4000-8000-000000000002");
    stored.entity.kind = EntityKind::Task;
    stored.entity.title = QStringLiteral("任务");
    stored.entity.body = QStringLiteral("正文");
    stored.entity.listId = inbox.id;
    stored.entity.startAt = now;
    stored.entity.dueAt = now.addDays(2).addSecs(3600);
    stored.entity.hasScheduleRange = true;
    stored.entity.timeZone = QStringLiteral("UTC");
    stored.entity.createdAt = now;
    stored.entity.updatedAt = now;
    stored.revision = 1;

    RecurrenceSpec recurrence;
    recurrence.frequency = RecurrenceSpec::Frequency::Weekly;
    recurrence.interval = 2;
    recurrence.weekdays = QVector<int>{Qt::Monday, Qt::Wednesday, Qt::Friday};
    RecurrenceSeries series;
    series.id = QStringLiteral("70000000-0000-4000-8000-000000000006");
    series.rrule = recurrenceFrequencyName(recurrence);
    series.ruleJson = recurrenceRuleText(recurrence);
    series.occurrenceOrdinal = 4;
    series.completionCount = 2;
    series.anchorDay = now.date().day();
    series.timeZone = stored.entity.timeZone;
    series.dtstartAt = stored.entity.startAt;
    series.createdAt = now;
    series.updatedAt = now;
    fixture.snapshot.recurrenceSeries.append(series);
    stored.entity.recurrenceSeriesId = series.id;
    stored.entity.occurrenceKey = QString::number(utcMs(stored.entity.startAt));
    stored.entity.rrule = series.rrule;
    stored.entity.recurrence = recurrenceSummary(recurrence);

    ReminderSpec reminderSpec;
    reminderSpec.anchor = ReminderSpec::Anchor::End;
    reminderSpec.offset = 15;
    TaskReminder reminder;
    reminder.id = QStringLiteral("70000000-0000-4000-8000-000000000007");
    reminder.entityId = stored.entity.id;
    reminder.triggerAt = stored.entity.dueAt.addSecs(-15 * 60);
    reminder.ruleJson = reminderRuleText(reminderSpec);
    reminder.createdAt = now;
    reminder.updatedAt = now;
    stored.entity.reminders.append(reminder);
    fixture.snapshot.reminders.append(reminder);
    fixture.snapshot.entities.append(stored);
    EntityEvent event;
    event.id = QStringLiteral("70000000-0000-4000-8000-000000000003");
    event.entityId = stored.entity.id;
    event.entityKind = EntityKind::Task;
    event.entityTitle = stored.entity.title;
    event.batchId = QStringLiteral("70000000-0000-4000-8000-000000000004");
    event.type = QStringLiteral("changed");
    event.field = QStringLiteral("title");
    event.oldValue = QVariant(QStringLiteral("a"));
    event.newValue = QVariant(QStringLiteral("b"));
    event.occurredAt = now;
    event.context.insert(QStringLiteral("listName"), QStringLiteral("事件发生时的清单"));
    fixture.events.append(event);
    return fixture;
}

QByteArray snapshotJson(const EntitySnapshot &snapshot, const QVector<EntityEvent> &events)
{
    return QJsonDocument(backupcodec::toJson(snapshot, events)).toJson(QJsonDocument::Compact);
}

} // namespace

class tst_Database : public QObject
{
    Q_OBJECT

private slots:
    void init() { Database::instance().close(); QVERIFY(m_dir.isValid()); }
    void cleanup() { Database::instance().close(); }

    void freshSchema_data();
    void freshSchema();
    void currentDatabaseReopensWithContentAndContext();
    void rejectsIncompatibleDatabasesWithoutWrites_data();
    void rejectsIncompatibleDatabasesWithoutWrites();
    void backupV6RoundTripWithContext();
    void backupRejectsOtherVersions_data();
    void backupRejectsOtherVersions();
    void backupRequiresAppearanceAndContext_data();
    void backupRequiresAppearanceAndContext();
    void backupRejectsRemovedFields_data();
    void backupRejectsRemovedFields();
    void repositoryBypassEmitsTrashedEventsWithContext();

private:
    QTemporaryDir m_dir;
};

void tst_Database::freshSchema_data()
{
    QTest::addColumn<bool>("existingEmptyFile");
    QTest::newRow("new-file") << false;
    QTest::newRow("empty-file") << true;
}

void tst_Database::freshSchema()
{
    QFETCH(bool, existingEmptyFile);
    const QString path = m_dir.filePath(QStringLiteral("fresh-%1.db").arg(QLatin1String(QTest::currentDataTag())));
    if (existingEmptyFile) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
    }
    Database &database = Database::instance();
    QString error = QStringLiteral("previous error");
    QVERIFY2(database.openAt(path, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(database.lastError().isEmpty());
    QVERIFY(database.isOpen());
    QCOMPARE(database.path(), path);
    QCOMPARE(Database::schemaVersion(), 10);
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA user_version")).toInt(), 10);
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA application_id")).toInt(), Database::applicationId());
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM entities")).toInt(), 0);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM lists WHERE is_inbox=1")).toInt(), 1);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name IN ('checklist_items','idx_checklist_entity')")).toInt(), 0);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entity_events') WHERE name='context_json' AND \"notnull\"=1")).toInt(), 1);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('all_day_end_date','has_schedule_range')")).toInt(), 2);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('recurrence_series') WHERE name IN ('rule_json','occurrence_ordinal','completion_count')")).toInt(), 3);
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('reminders') WHERE name='rule_json'")).toInt(), 1);
    QCOMPARE(inspect(path, QStringLiteral("SELECT dflt_value FROM pragma_table_info('entity_events') WHERE name='context_json'")).toString(),
             QStringLiteral("'{}'"));
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_check")).toInt(), 0);
}

void tst_Database::currentDatabaseReopensWithContentAndContext()
{
    const QString path = m_dir.filePath(QStringLiteral("reopen.db"));
    Database &database = Database::instance();
    QString error;
    QVERIFY2(database.openAt(path, &error), qPrintable(error));
    const SnapshotFixture fixture = makeSnapshotFixture();
    QString inbox;
    const QString installation = inspect(path, QStringLiteral("SELECT value FROM local_meta WHERE key='installation_id'")).toString();
    QVERIFY(isUuid(installation));
    {
        TaskRepository repository(database.connection());
        EntitySnapshot initial;
        QVERIFY2(repository.load(&initial, &error), qPrintable(error));
        for (const TaskList &list : initial.lists) if (list.inbox) inbox = list.id;
        QVERIFY(isUuid(inbox));
        EntityChanges changes;
        StoredEntity stored = fixture.snapshot.entities.first();
        stored.revision = 0;
        stored.entity.listId = inbox;
        changes.saves.append(stored);
        changes.seriesSaves = fixture.snapshot.recurrenceSeries;
        changes.reminderSaves = fixture.snapshot.reminders;
        changes.eventSaves = fixture.events;
        EntitySnapshot after;
        QVERIFY2(repository.apply(&changes, &after, &error), qPrintable(error));
        QCOMPARE(after.entities.size(), 1);
    }
    database.close();
    QVERIFY2(database.openAt(path, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA user_version")).toInt(), Database::schemaVersion());
    QCOMPARE(inspect(path, QStringLiteral("SELECT value FROM local_meta WHERE key='installation_id'")).toString(), installation);
    QCOMPARE(inspect(path, QStringLiteral("SELECT value FROM local_meta WHERE key='inbox_list_id'")).toString(), inbox);
    TaskRepository repository(database.connection());
    EntitySnapshot reopened;
    QVERIFY2(repository.load(&reopened, &error), qPrintable(error));
    QCOMPARE(reopened.entities.size(), 1);
    QCOMPARE(reopened.entities.first().entity.id, fixture.snapshot.entities.first().entity.id);
    QCOMPARE(reopened.entities.first().entity.body, QStringLiteral("正文"));
    QCOMPARE(reopened.entities.first().entity.listId, inbox);
    QCOMPARE(reopened.entities.first().entity.startAt, fixture.snapshot.entities.first().entity.startAt);
    QCOMPARE(reopened.entities.first().entity.dueAt, fixture.snapshot.entities.first().entity.dueAt);
    QVERIFY(reopened.entities.first().entity.hasScheduleRange);
    QCOMPARE(reopened.recurrenceSeries.size(), 1);
    QCOMPARE(reopened.recurrenceSeries.first().ruleJson, fixture.snapshot.recurrenceSeries.first().ruleJson);
    QCOMPARE(reopened.recurrenceSeries.first().occurrenceOrdinal, 4);
    QCOMPARE(reopened.recurrenceSeries.first().completionCount, 2);
    QCOMPARE(reopened.reminders.size(), 1);
    QCOMPARE(reopened.reminders.first().id, fixture.snapshot.reminders.first().id);
    QCOMPARE(reopened.reminders.first().ruleJson, fixture.snapshot.reminders.first().ruleJson);
    QCOMPARE(reopened.entities.first().entity.reminders.size(), 1);
    QCOMPARE(reopened.entities.first().entity.reminders.first().triggerAt,
             fixture.snapshot.reminders.first().triggerAt);
    const QVector<EntityEvent> events = repository.allEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().id, fixture.events.first().id);
    QCOMPARE(events.first().context, fixture.events.first().context);
    QCOMPARE(events.first().oldValue.toString(), QStringLiteral("a"));
    QCOMPARE(events.first().newValue.toString(), QStringLiteral("b"));
}

void tst_Database::rejectsIncompatibleDatabasesWithoutWrites_data()
{
    QTest::addColumn<int>("version");
    QTest::addColumn<int>("application");
    QTest::addColumn<bool>("populated");
    QTest::newRow("unmarked-content") << 0 << 0 << true;
    QTest::newRow("unversioned-kylin") << 0 << Database::applicationId() << true;
    QTest::newRow("v4") << 4 << Database::applicationId() << true;
    QTest::newRow("v5") << 5 << Database::applicationId() << true;
    QTest::newRow("v6") << 6 << Database::applicationId() << true;
    QTest::newRow("v7") << 7 << Database::applicationId() << true;
    QTest::newRow("v8") << 8 << Database::applicationId() << true;
    QTest::newRow("v9") << 9 << Database::applicationId() << true;
    QTest::newRow("v9-no-tables") << 9 << Database::applicationId() << false;
    QTest::newRow("future") << Database::schemaVersion() + 1 << Database::applicationId() << true;
    QTest::newRow("foreign-current") << Database::schemaVersion() << 0x4F544852 << true;
    QTest::newRow("missing-application") << Database::schemaVersion() << 0 << true;
    QTest::newRow("foreign-empty") << 0 << 0x4F544852 << false;
}

void tst_Database::rejectsIncompatibleDatabasesWithoutWrites()
{
    QFETCH(int, version);
    QFETCH(int, application);
    QFETCH(bool, populated);
    const QString path = m_dir.filePath(QStringLiteral("rejected-%1.db").arg(QLatin1String(QTest::currentDataTag())));
    // 此夹具只测试标识门禁；门禁不能依赖任何业务表存在，更不能执行迁移/重建。
    QStringList statements{QStringLiteral("PRAGMA journal_mode=DELETE"),
        QStringLiteral("PRAGMA application_id=%1").arg(application),
        QStringLiteral("PRAGMA user_version=%1").arg(version)};
    if (populated) {
        statements.append(QStringLiteral("CREATE TABLE retained_data(id INTEGER PRIMARY KEY,payload TEXT NOT NULL,revision INTEGER NOT NULL)"));
        statements.append(QStringLiteral("INSERT INTO retained_data VALUES(7,'保留原始正文与内容',11)"));
    }
    QVERIFY(seed(path, statements));
    const QByteArray before = readBytes(path);
    QVERIFY(!before.isEmpty());
    const QStringList beforeEntries = directoryEntries(path);
    Database &database = Database::instance();
    for (int attempt = 0; attempt < 2; ++attempt) {
        QString error;
        QVERIFY(!database.openAt(path, &error));
        QVERIFY(!database.isOpen());
        QVERIFY(database.path().isEmpty());
        QCOMPARE(database.lastError(), error);
        QVERIFY2(error.contains(path), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("检测到版本 v%1").arg(version)), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("要求 v%1").arg(Database::schemaVersion())), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("application_id=%1").arg(application)), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("开发阶段不支持数据库迁移")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("--data-dir")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("新的空数据目录")), qPrintable(error));
        QCOMPARE(readBytes(path), before);
        QCOMPARE(directoryEntries(path), beforeEntries); // 不生成迁移备份、WAL 或其他旁路文件。
    }
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA user_version")).toInt(), version);
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA application_id")).toInt(), application);
    QCOMPARE(inspect(path, QStringLiteral("PRAGMA journal_mode")).toString(), QStringLiteral("delete"));
    QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")).toInt(), populated ? 1 : 0);
    if (populated) {
        QCOMPARE(inspect(path, QStringLiteral("SELECT COUNT(*) FROM retained_data")).toInt(), 1);
        QCOMPARE(inspect(path, QStringLiteral("SELECT payload FROM retained_data WHERE id=7")).toString(), QStringLiteral("保留原始正文与内容"));
        QCOMPARE(inspect(path, QStringLiteral("SELECT revision FROM retained_data WHERE id=7")).toInt(), 11);
    }
    QCOMPARE(readBytes(path), before);
    QCOMPARE(directoryEntries(path), beforeEntries);
}

void tst_Database::backupV6RoundTripWithContext()
{
    const SnapshotFixture fixture = makeSnapshotFixture();
    const QJsonObject root = backupcodec::toJson(fixture.snapshot, fixture.events);
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6);
    QVERIFY(!root.contains(QStringLiteral("checklistItems")));
    QVERIFY(!root.value(QStringLiteral("entities")).toArray().first().toObject().contains(QStringLiteral("checklist")));
    QVERIFY(root.value(QStringLiteral("events")).toArray().first().toObject().value(QStringLiteral("context")).isObject());
    const QJsonObject exportedSeries = root.value(QStringLiteral("recurrenceSeries")).toArray().first().toObject();
    QCOMPARE(exportedSeries.value(QStringLiteral("ruleJson")).toString(),
             fixture.snapshot.recurrenceSeries.first().ruleJson);
    QCOMPARE(exportedSeries.value(QStringLiteral("occurrenceOrdinal")).toInt(), 4);
    QCOMPARE(exportedSeries.value(QStringLiteral("completionCount")).toInt(), 2);
    const QJsonObject exportedEntity = root.value(QStringLiteral("entities")).toArray().first().toObject();
    QVERIFY(exportedEntity.value(QStringLiteral("hasScheduleRange")).toBool());
    QCOMPARE(exportedEntity.value(QStringLiteral("startAtUtcMs")).toDouble(),
             static_cast<double>(utcMs(fixture.snapshot.entities.first().entity.startAt)));
    QCOMPARE(root.value(QStringLiteral("reminders")).toArray().first().toObject().value(QStringLiteral("ruleJson")).toString(),
             fixture.snapshot.reminders.first().ruleJson);
    EntitySnapshot parsed;
    QVector<EntityEvent> events;
    QString timezone;
    QString error;
    QVERIFY2(backupcodec::fromJson(root, &parsed, &events, &timezone, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(timezone, QStringLiteral("Asia/Shanghai"));
    QCOMPARE(parsed.entities.size(), 1);
    QCOMPARE(parsed.entities.first().entity.body, QStringLiteral("正文"));
    QVERIFY(parsed.entities.first().entity.hasScheduleRange);
    QCOMPARE(parsed.entities.first().entity.startAt, fixture.snapshot.entities.first().entity.startAt);
    QCOMPARE(parsed.entities.first().entity.dueAt, fixture.snapshot.entities.first().entity.dueAt);
    QCOMPARE(parsed.recurrenceSeries.first().ruleJson, fixture.snapshot.recurrenceSeries.first().ruleJson);
    QCOMPARE(parsed.recurrenceSeries.first().occurrenceOrdinal, 4);
    QCOMPARE(parsed.recurrenceSeries.first().completionCount, 2);
    RecurrenceSpec restoredRule;
    QVERIFY(recurrenceFromRuleText(parsed.recurrenceSeries.first().ruleJson, &restoredRule, &error));
    QVERIFY(restoredRule.weekdays == QVector<int>({Qt::Monday, Qt::Wednesday, Qt::Friday}));
    QCOMPARE(parsed.reminders.size(), 1);
    QCOMPARE(parsed.reminders.first().id, fixture.snapshot.reminders.first().id);
    QCOMPARE(parsed.reminders.first().ruleJson, fixture.snapshot.reminders.first().ruleJson);
    QCOMPARE(parsed.folders.first().icon, fixture.snapshot.folders.first().icon);
    QCOMPARE(parsed.lists.first().icon, fixture.snapshot.lists.first().icon);
    QCOMPARE(parsed.lists.first().color, fixture.snapshot.lists.first().color);
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().oldValue.toString(), QStringLiteral("a"));
    QCOMPARE(events.first().context, fixture.events.first().context);
    QCOMPARE(snapshotJson(parsed, events), QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void tst_Database::backupRejectsOtherVersions_data()
{
    QTest::addColumn<int>("version");
    QTest::newRow("negative") << -1;
    QTest::newRow("v0") << 0;
    QTest::newRow("v1") << 1;
    QTest::newRow("v2") << 2;
    QTest::newRow("v3") << 3;
    QTest::newRow("v4") << 4;
    QTest::newRow("v5") << 5;
    QTest::newRow("future") << backupcodec::kVersion + 1;
}

void tst_Database::backupRejectsOtherVersions()
{
    QFETCH(int, version);
    const SnapshotFixture fixture = makeSnapshotFixture();
    QJsonObject root = backupcodec::toJson(fixture.snapshot, fixture.events);
    root.insert(QStringLiteral("version"), version);
    EntitySnapshot parsed = fixture.snapshot;
    QVector<EntityEvent> events = fixture.events;
    const QByteArray before = snapshotJson(parsed, events);
    QString timezone = QStringLiteral("unchanged");
    QString error;
    QVERIFY(!backupcodec::fromJson(root, &parsed, &events, &timezone, &error));
    QVERIFY2(error.contains(QStringLiteral("v%1").arg(backupcodec::kVersion)), qPrintable(error));
    QCOMPARE(snapshotJson(parsed, events), before);
    QCOMPARE(timezone, QStringLiteral("unchanged"));
}

void tst_Database::backupRequiresAppearanceAndContext_data()
{
    QTest::addColumn<QString>("arrayKey");
    QTest::addColumn<QString>("field");
    QTest::addColumn<int>("invalidKind");
    const struct { const char *arrayKey; const char *field; } fields[] = {
        {"folders", "icon"}, {"lists", "icon"}, {"lists", "color"},
        {"lists", "archivedAtUtcMs"}, {"events", "context"}
    };
    const char *const kinds[] = {"missing", "null", "wrong-type"};
    for (const auto &entry : fields) for (int kind = 0; kind < 3; ++kind) {
        const QByteArray tag = QByteArray(entry.arrayKey) + '-' + entry.field + '-' + kinds[kind];
        QTest::newRow(tag.constData()) << QString::fromLatin1(entry.arrayKey) << QString::fromLatin1(entry.field) << kind;
    }
}

void tst_Database::backupRequiresAppearanceAndContext()
{
    QFETCH(QString, arrayKey);
    QFETCH(QString, field);
    QFETCH(int, invalidKind);
    const SnapshotFixture fixture = makeSnapshotFixture();
    QJsonObject root = backupcodec::toJson(fixture.snapshot, fixture.events);
    QJsonArray rows = root.value(arrayKey).toArray();
    QJsonObject row = rows.first().toObject();
    if (invalidKind == 0) row.remove(field);
    else if (invalidKind == 1) row.insert(field, QJsonValue(QJsonValue::Null));
    else row.insert(field, field == QStringLiteral("context") ? QJsonValue(QStringLiteral("not an object")) : QJsonValue(QJsonArray()));
    rows.replace(0, row);
    root.insert(arrayKey, rows);
    EntitySnapshot parsed = fixture.snapshot;
    QVector<EntityEvent> events = fixture.events;
    const QByteArray before = snapshotJson(parsed, events);
    QString timezone = QStringLiteral("unchanged");
    QString error;
    QVERIFY(!backupcodec::fromJson(root, &parsed, &events, &timezone, &error));
    const QString expectedField = field == QStringLiteral("archivedAtUtcMs") ? QStringLiteral("归档时间") : field;
    QVERIFY2(error.contains(expectedField), qPrintable(error));
    QCOMPARE(snapshotJson(parsed, events), before);
    QCOMPARE(timezone, QStringLiteral("unchanged"));
}

void tst_Database::backupRejectsRemovedFields_data()
{
    QTest::addColumn<bool>("nested");
    QTest::addColumn<bool>("nullValue");
    QTest::newRow("root-array") << false << false;
    QTest::newRow("root-null") << false << true;
    QTest::newRow("nested-array") << true << false;
    QTest::newRow("nested-null") << true << true;
}

void tst_Database::backupRejectsRemovedFields()
{
    QFETCH(bool, nested);
    QFETCH(bool, nullValue);
    const SnapshotFixture fixture = makeSnapshotFixture();
    QJsonObject root = backupcodec::toJson(fixture.snapshot, fixture.events);
    const QJsonValue value = nullValue ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonArray());
    const QString key = nested ? QStringLiteral("checklist") : QStringLiteral("checklistItems");
    if (nested) {
        QJsonArray entities = root.value(QStringLiteral("entities")).toArray();
        QJsonObject entity = entities.first().toObject();
        entity.insert(key, value);
        entities.replace(0, entity);
        root.insert(QStringLiteral("entities"), entities);
    } else root.insert(key, value);
    EntitySnapshot parsed;
    QVector<EntityEvent> events;
    QString timezone;
    QString error;
    QVERIFY(!backupcodec::fromJson(root, &parsed, &events, &timezone, &error));
    QVERIFY2(error.contains(key), qPrintable(error));
}

void tst_Database::repositoryBypassEmitsTrashedEventsWithContext()
{
    const QString path = m_dir.filePath(QStringLiteral("bypass.db"));
    QString error;
    Database &database = Database::instance();
    QVERIFY2(database.openAt(path, &error), qPrintable(error));
    TaskRepository repository(database.connection());
    EntitySnapshot initial;
    QVERIFY2(repository.load(&initial, &error), qPrintable(error));
    QString inbox;
    for (const TaskList &list : initial.lists) if (list.inbox) inbox = list.id;
    QVERIFY(isUuid(inbox));
    const QDateTime now = utcNow();
    TaskList list;
    list.id = newUuid();
    list.name = QStringLiteral("被删清单");
    list.createdAt = now;
    list.updatedAt = now;
    const QString kept = newUuid(), kept2 = newUuid(), alreadyTrashed = newUuid();
    EntityChanges setup;
    setup.groupSaves.append(list);
    for (const QString &id : {kept, kept2, alreadyTrashed}) {
        StoredEntity stored;
        stored.entity.id = id;
        stored.entity.kind = EntityKind::Task;
        stored.entity.title = QStringLiteral("任务");
        stored.entity.listId = list.id;
        stored.entity.createdAt = now;
        stored.entity.updatedAt = now;
        if (id == alreadyTrashed) {
            stored.entity.deleted = true;
            stored.entity.deletedAt = now;
        }
        setup.saves.append(stored);
    }
    EntitySnapshot afterSetup;
    QVERIFY2(repository.apply(&setup, &afterSetup, &error), qPrintable(error));
    // 删除清单旁路：同事务生成 trashed 事件并回填 changes->eventSaves。
    EntityChanges removal;
    removal.groupRemovals.append(list.id);
    EntitySnapshot afterRemoval;
    QVERIFY2(repository.apply(&removal, &afterRemoval, &error), qPrintable(error));
    QCOMPARE(removal.eventSaves.size(), 2);
    QString batch;
    for (const EntityEvent &event : removal.eventSaves) {
        QCOMPARE(event.type, QStringLiteral("trashed"));
        QCOMPARE(event.field, QStringLiteral("deleted"));
        QCOMPARE(event.oldValue.toString(), QStringLiteral("0"));
        QCOMPARE(event.newValue.toString(), QStringLiteral("1"));
        QCOMPARE(event.context.value(QStringLiteral("listId")).toString(), list.id);
        QCOMPARE(event.context.value(QStringLiteral("listName")).toString(), QStringLiteral("被删清单"));
        QVERIFY(event.entityId == kept || event.entityId == kept2);
        if (batch.isEmpty()) batch = event.batchId;
        QCOMPARE(event.batchId, batch);
    }
    const QVector<EntityEvent> events = repository.eventsFor(kept, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().context.value(QStringLiteral("listName")).toString(), QStringLiteral("被删清单"));
    QCOMPARE(repository.eventsFor(alreadyTrashed, &error).size(), 0);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // 实体移入收集箱；已在回收站的实体不重复生成 trashed 事件。
    for (const StoredEntity &row : afterRemoval.entities) {
        if (row.entity.id == kept || row.entity.id == kept2) {
            QVERIFY(row.entity.deleted);
            QCOMPARE(row.entity.listId, inbox);
            QCOMPARE(row.revision, qint64(2));
        } else if (row.entity.id == alreadyTrashed) {
            QVERIFY(row.entity.deleted);
            QCOMPARE(row.entity.listId, inbox);
        }
    }
}

QTEST_MAIN(tst_Database)
#include "tst_database.moc"
