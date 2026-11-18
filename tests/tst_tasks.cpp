#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "db/TaskRepository.h"
#include "domain/AttachmentCodec.h"
#include "domain/BackupCodec.h"
#include "domain/InternalLink.h"
#include "domain/TaskDrop.h"
#include "domain/ViewSpec.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <memory>

using namespace kl;

namespace {
int connectionCounter = 0;
QDateTime utc(int year, int month, int day, int hour = 10, int minute = 0)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute), Qt::UTC);
}
bool seed(const QString &path, const QStringList &statements)
{
    const QString name = QStringLiteral("seed_%1").arg(++connectionCounter);
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path); ok = db.open();
        for (const QString &statement : statements) {
            if (!ok) break;
            QSqlQuery query(db); ok = query.exec(statement);
            if (!ok) qWarning() << query.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name); return ok;
}
QVariant inspect(const QString &path, const QString &sql)
{
    const QString name = QStringLiteral("inspect_%1").arg(++connectionCounter);
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name); db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) value = query.value(0);
            else qWarning() << query.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name); return value;
}
bool execute(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql)) return true;
    qWarning() << sql << query.lastError().text(); return false;
}
QVariant scalar(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql) && query.next()) return query.value(0);
    qWarning() << sql << query.lastError().text(); return QVariant();
}
QList<QVariantList> queryRows(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    QList<QVariantList> rows;
    if (!query.exec(sql)) {
        qWarning() << sql << query.lastError().text();
        return rows;
    }
    while (query.next()) {
        QVariantList row;
        const QSqlRecord record = query.record();
        for (int i = 0; i < record.count(); ++i) row.append(query.value(i));
        rows.append(row);
    }
    return rows;
}
QByteArray readBytes(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray pngBytes()
{
    QImage image(6, 4, QImage::Format_ARGB32); image.fill(Qt::blue);
    QByteArray bytes; QBuffer buffer(&bytes);
    return buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG") ? bytes : QByteArray();
}
QString activeTask(const TaskStore &store, const QString &except = QString())
{
    for (const Entity &task : store.entities()) if (task.id != except && task.status == TaskStatus::Active) return task.id;
    return QString();
}
QJsonObject readMetadata(const QString &path, QVector<attachmentcodec::BackupFrame> *frames = nullptr)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
    QByteArray metadata; QVector<attachmentcodec::BackupFrameInfo> infos; QString error;
    if (!attachmentcodec::readBackupContainer(&file, &metadata, &infos, &error)) {
        qWarning() << error; return QJsonObject();
    }
    if (frames) {
        QVector<attachmentcodec::BackupFrame> parsed;
        for (const auto &info : infos) {
            if (!file.seek(info.offset)) return QJsonObject();
            parsed.append(attachmentcodec::BackupFrame{info.id, file.read(info.size)});
        }
        *frames = parsed;
    }
    return QJsonDocument::fromJson(metadata).object();
}
bool writeContainer(const QString &path, const QJsonObject &root, const QVector<attachmentcodec::BackupFrame> &frames)
{
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    QString error;
    return attachmentcodec::writeBackupContainer(&file, QJsonDocument(root).toJson(QJsonDocument::Compact), frames, &error);
}
}

class TaskTests : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void freshSchemaIgnoresUnrelatedDatabase();
    void rejectsWrongIdentityAndFutureVersion();
    void completeModelAndUuidCrud();
    void inboxAndOrganizationIntegrity();
    void entityKindAndParentConstraints();
    void eventRepositoryTransactionsAndLazyReads();
    void convertKindPreservesFieldsAndAssociations();
    void convertKindRejectsRelationshipsAndRollsBack();
    void applicationFacadeKindAndPinnedAt();
    void journalEditSessionsCommitFullText();
    void textDraftThreeWayMerge();
    void textDraftUnavailableAndFailureAtomicity();
    void textDraftSessionCancelSemantics();
    void journalScalarBusinessAndLocalOnlyEvents();
    void convertedNotesReceiveRemindersWithoutCompleting();
    void bodyMarkdownCheckboxesRoundTrip();
    void taskAutosaveValidationAndTransactions();
    void scheduleRangesPersistAndBackupRoundTrip_data();
    void scheduleRangesPersistAndBackupRoundTrip();
    void scheduleRejectsInvalidEndpoints_data();
    void scheduleRejectsInvalidEndpoints();
    void fullRecurrenceRulesPersistAndAdvance_data();
    void fullRecurrenceRulesPersistAndAdvance();
    void recurrenceFinalCompletionAndSafeUndoGuard();
    void recurrenceProgressCasAndDeletedMiddleUndo();
    void relativeReminderRulesRecalculateAndSnoozeOnce();
    void recurrenceMonthEndAndOccurrenceUnique();
    void recurrenceDailyWeeklyAndSafeUndo();
    void recurrenceUndoPreservesAssociatedData();
    void staleStoreCannotUndoAssociatedOccurrence();
    void cloneCompletedRecurringTask();
    void multipleRemindersDeliveryRestartAndSnooze();
    void attachmentsShaStateAndTransactions();
    void desktopNotesAreDeviceIndependent();
    void allDayRecurrenceAndFlatBackup();
    void dateOnlyAndMidnightBackup_data();
    void dateOnlyAndMidnightBackup();
    void entityCodecAndBackupRoundTrip();
    void listAppearanceArchiveAndAtomicUpdate();
    void listUpdatesRejectStaleRevisions();
    void folderIconEditAndStaleRevisionGuard();
    void listAndFolderTailSortOrder();
    void listOrganizationMoveCommands();
    void folderFromListsAtomicCommand();
    void allZeroTopLevelOrderIsDeterministic();
    void mixedTopLevelOrderAndCrossFolderMoves();
    void organizationNameLengthLimits();
    void deleteListMovesContentToTrashAtomically();
    void viewSpecScopesAndArchivedFiltering();
    void archivedListsSuppressReminders();
    void postponeOverdueTasksShiftsDates();
    void postponePreservesDateSpansAndReminders();
    void createSubtaskInheritsDatesOnly();
    void backupRoundTripAndLegacyRejection();
    void backupValidationAndSqlRollbackAreAtomic();
    void backupContainerLimitsAndStreaming();
    void backupRestoredReminderDeliveryPolicy();
    void backupDeliveryFailureRestoresEverything();
    void backupRecurrenceUndoProtection();
    void outboxDoesNotAccumulateWhileLoggedOut();
    void appTimeZoneHelpers();

private:
    bool open();
    QString dbPath() const { return m_directory->filePath(QStringLiteral("kylinlist.db")); }
    QString backupPath() const { return m_directory->filePath(QStringLiteral("备份.klb")); }
    QString attachmentPath(const QString &relative) const
    {
        return QDir(m_directory->filePath(QStringLiteral("attachments"))).filePath(relative);
    }
    int attachmentFileCount() const
    {
        return QDir(m_directory->filePath(QStringLiteral("attachments"))).entryList(QDir::Files).size();
    }
    std::unique_ptr<QTemporaryDir> m_directory;
};

void TaskTests::init()
{
    Database::instance().close(); m_directory.reset(new QTemporaryDir); QVERIFY(m_directory->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_directory->path().toUtf8());
}
void TaskTests::cleanup()
{
    Database::instance().close(); qunsetenv("KYLINLIST_DATA_DIR"); m_directory.reset();
}
bool TaskTests::open()
{
    QString error; const bool ok = Database::instance().openAt(dbPath(), &error);
    if (!ok) qWarning() << error; return ok;
}

void TaskTests::freshSchemaIgnoresUnrelatedDatabase()
{
    const QString legacy = m_directory->filePath(QStringLiteral("notes.db"));
    QVERIFY(seed(legacy, {QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY,title TEXT,content TEXT)"),
                          QStringLiteral("PRAGMA user_version=3"),
                          QStringLiteral("INSERT INTO notes VALUES(7,'旧数据','<b>不迁移</b>')")}));
    QString error;
    QVERIFY2(Database::instance().open(&error), qPrintable(error));
    QCOMPARE(Database::instance().path(), dbPath());
    QCOMPARE(Database::schemaVersion(), 10);
    QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), Database::schemaVersion());
    QCOMPARE(scalar(QStringLiteral("PRAGMA application_id")).toInt(), Database::applicationId());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities")).toInt(), 0);
    QCOMPARE(inspect(legacy, QStringLiteral("SELECT title FROM notes")).toString(), QStringLiteral("旧数据"));
    QCOMPARE(inspect(legacy, QStringLiteral("PRAGMA user_version")).toInt(), 3);
    const char *tables[] = {"folders","lists","sections","entities","tags","entity_tags",
        "recurrence_series","recurrence_links","reminders","reminder_deliveries","attachments","desktop_notes",
        "sync_bindings","view_defs","local_meta","tombstones","outbox","entity_events"};
    for (const char *table : tables)
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%1'").arg(QLatin1String(table))).toInt(), 1);
    const char *removedTables[] = {"workspaces","tasks","task_lists","task_tags","task_recurrence_links","saved_filters","checklist_items"};
    for (const char *table : removedTables)
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%1'").arg(QLatin1String(table))).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE sql LIKE '%workspace_id%' OR sql LIKE '%task_id%'")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('favorite','content_format','description','workspace_id')")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('kind','body_markdown','sort_order','pinned_at_utc_ms')")).toInt(), 4);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('all_day_end_date','has_schedule_range')")).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('recurrence_series') WHERE name IN ('rule_json','occurrence_ordinal','completion_count')")).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('reminders') WHERE name='rule_json'")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE name='parent_id'")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name='kind'")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name IN ('icon','color','archived_at_utc_ms')")).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name IN ('icon','color','archived_at_utc_ms') AND \"notnull\"=1")).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE name='icon'")).toInt(), 1);       // v7：文件夹图标
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE name='icon' AND \"notnull\"=1")).toInt(), 1);
    QSqlQuery invalidFolderIcon(Database::instance().connection());
    invalidFolderIcon.prepare(QStringLiteral("INSERT INTO folders(id,name,icon,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'超长文件夹图标','%1',1,1)")
                                .arg(QString(17, QChar(0x4E2D))));
    invalidFolderIcon.addBindValue(newUuid()); QVERIFY(!invalidFolderIcon.exec());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name='prevent_inbox_archive'")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name LIKE 'prevent_inbox%'")).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('reminders') WHERE name='kind'")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entity_events') WHERE name IN ('revision','server_revision')")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entity_events') WHERE name='context_json' AND \"notnull\"=1")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_list('entity_events')")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_list('reminders')")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_list('reminders') WHERE \"from\"='entity_id' AND \"table\"='entities'")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(DISTINCT \"from\") FROM pragma_foreign_key_list('entities') WHERE \"from\" IN ('parent_id','kind')")).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT value FROM local_meta WHERE key='default_timezone'")).toString(), QStringLiteral("Asia/Shanghai"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM local_meta WHERE key='default_workspace_id'")).toInt(), 0);
    QVERIFY(isUuid(scalar(QStringLiteral("SELECT value FROM local_meta WHERE key='installation_id'")).toString()));
    QVERIFY(isUuid(scalar(QStringLiteral("SELECT value FROM local_meta WHERE key='device_id'")).toString()));
    const QString inbox = scalar(QStringLiteral("SELECT value FROM local_meta WHERE key='inbox_list_id'")).toString();
    QVERIFY(isUuid(inbox));
    QCOMPARE(scalar(QStringLiteral("SELECT is_inbox FROM lists WHERE id='%1'").arg(inbox)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT kind FROM lists WHERE id='%1'").arg(inbox)).toString(), QStringLiteral("task"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM lists WHERE is_inbox=1")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT icon FROM lists WHERE id='%1'").arg(inbox)).toString(), QString());
    QCOMPARE(scalar(QStringLiteral("SELECT color FROM lists WHERE id='%1'").arg(inbox)).toString(), QString());
    QCOMPARE(scalar(QStringLiteral("SELECT archived_at_utc_ms FROM lists WHERE id='%1'").arg(inbox)).toLongLong(), qint64(0));
    QSqlQuery archiveInbox(Database::instance().connection());
    archiveInbox.prepare(QStringLiteral("UPDATE lists SET archived_at_utc_ms=123 WHERE id=?"));
    archiveInbox.addBindValue(inbox); QVERIFY(!archiveInbox.exec());   // 收集箱永不可归档
    QCOMPARE(scalar(QStringLiteral("SELECT archived_at_utc_ms FROM lists WHERE id='%1'").arg(inbox)).toLongLong(), qint64(0));
    QSqlQuery invalidColor(Database::instance().connection());
    invalidColor.prepare(QStringLiteral("INSERT INTO lists(id,name,color,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'非法颜色','red',1,1)"));
    invalidColor.addBindValue(newUuid()); QVERIFY(!invalidColor.exec());
    invalidColor.prepare(QStringLiteral("INSERT INTO lists(id,name,color,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'非法颜色','#12345',1,1)"));
    invalidColor.addBindValue(newUuid()); QVERIFY(!invalidColor.exec());
    invalidColor.prepare(QStringLiteral("INSERT INTO lists(id,name,color,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'非法颜色','#12345G',1,1)"));
    invalidColor.addBindValue(newUuid()); QVERIFY(!invalidColor.exec());
    invalidColor.prepare(QStringLiteral("INSERT INTO lists(id,name,icon,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'超长图标','%1',1,1)")
                            .arg(QString(17, QChar(0x4E2D))));
    invalidColor.addBindValue(newUuid()); QVERIFY(!invalidColor.exec());
    invalidColor.prepare(QStringLiteral("INSERT INTO lists(id,name,color,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'合法外观','#a1B2c3',1,1)"));
    invalidColor.addBindValue(newUuid()); QVERIFY(invalidColor.exec());
    QSqlQuery secondInbox(Database::instance().connection());
    secondInbox.prepare(QStringLiteral("INSERT INTO lists(id,name,is_inbox,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'第二收集箱',1,1,1)"));
    secondInbox.addBindValue(newUuid()); QVERIFY(!secondInbox.exec());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name='pinned'")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('outbox') WHERE name IN ('batch_id','base_server_revision')")).toInt(), 2);
    const char *versioned[] = {"folders","lists","sections","recurrence_series","entities","recurrence_links",
        "tags","entity_tags","reminders","attachments","sync_bindings","view_defs"};
    for (const char *table : versioned)
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('%1') WHERE name IN ('revision','server_revision')").arg(QLatin1String(table))).toInt(), 2);
    const QString filter = newUuid();
    QVERIFY(execute(QStringLiteral("INSERT INTO view_defs(id,name,spec_json,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','全部','{\"view\":\"all\"}',1,1)").arg(filter)));
    const QString orphanEvent = newUuid();
    QVERIFY(execute(QStringLiteral("INSERT INTO entity_events(id,entity_id,entity_kind,entity_title,batch_id,type,occurred_at_utc_ms) VALUES('%1','%2','task','已删除实体','%3','created',1)")
                        .arg(newUuid(), orphanEvent, newUuid())));
    TaskRepository repository(Database::instance().connection()); EntitySnapshot snapshot; QVERIFY(repository.load(&snapshot));
    QCOMPARE(snapshot.viewDefs.size(), 1); QCOMPARE(snapshot.viewDefs.first().id, filter);
    QCOMPARE(snapshot.viewDefs.first().specJson, QStringLiteral("{\"view\":\"all\"}"));
    QCOMPARE(snapshot.viewDefs.first().revision, 1); QCOMPARE(snapshot.viewDefs.first().serverRevision, 0);
    QCOMPARE(scalar(QStringLiteral("PRAGMA foreign_keys")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("PRAGMA busy_timeout")).toInt(), 5000);
    QCOMPARE(scalar(QStringLiteral("PRAGMA journal_mode")).toString().toLower(), QStringLiteral("wal"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_foreign_key_check")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")).toString(), QStringLiteral("ok"));
}

void TaskTests::rejectsWrongIdentityAndFutureVersion()
{
    const QString old = m_directory->filePath(QStringLiteral("old.db"));
    QVERIFY(seed(old, {QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY,title TEXT)"),
                       QStringLiteral("PRAGMA user_version=3"), QStringLiteral("INSERT INTO notes VALUES(1,'保留')")}));
    QString error;
    QVERIFY(!Database::instance().openAt(old, &error)); QVERIFY(!error.isEmpty()); QVERIFY(!Database::instance().isOpen());
    QCOMPARE(inspect(old, QStringLiteral("PRAGMA user_version")).toInt(), 3);
    QCOMPARE(inspect(old, QStringLiteral("SELECT title FROM notes")).toString(), QStringLiteral("保留"));
    const QString v4 = m_directory->filePath(QStringLiteral("v4.db"));
    QVERIFY(seed(v4, {QStringLiteral("CREATE TABLE tasks(id TEXT PRIMARY KEY)"), QStringLiteral("PRAGMA user_version=4"),
                      QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId()),
                      QStringLiteral("INSERT INTO tasks VALUES('legacy')")}));
    QVERIFY(!Database::instance().openAt(v4, &error)); QVERIFY(error.contains(QStringLiteral("要求 v%1").arg(Database::schemaVersion())));
    QCOMPARE(inspect(v4, QStringLiteral("PRAGMA user_version")).toInt(), 4);
    QCOMPARE(inspect(v4, QStringLiteral("SELECT id FROM tasks")).toString(), QStringLiteral("legacy"));
    const QString future = m_directory->filePath(QStringLiteral("future.db"));
    const int futureVersion = Database::schemaVersion() + 1;
    QVERIFY(seed(future, {QStringLiteral("CREATE TABLE future_data(id TEXT)"), QStringLiteral("PRAGMA user_version=%1").arg(futureVersion),
                          QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId())}));
    QVERIFY(!Database::instance().openAt(future, &error));
    QVERIFY(error.contains(QStringLiteral("要求 v%1").arg(Database::schemaVersion())));
    QVERIFY(error.contains(QStringLiteral("检测到版本 v%1").arg(futureVersion)));
    QVERIFY(error.contains(future)); QVERIFY(error.contains(QStringLiteral("--data-dir")));
    QCOMPARE(inspect(future, QStringLiteral("PRAGMA user_version")).toInt(), futureVersion);
    QVERIFY(!Database::instance().openAt(QString(), &error));
    QVERIFY(open());
}

void TaskTests::completeModelAndUuidCrud()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral(" 项目 ")); QVERIFY(isUuid(folder));
    const QString list = store.createList(QStringLiteral(" 本周 "), folder, EntityKind::Note); QVERIFY(isUuid(list));
    QCOMPARE(scalar(QStringLiteral("SELECT kind FROM lists WHERE id='%1'").arg(list)).toString(), QStringLiteral("note"));
    TaskList persistedList; for (const TaskList &candidate : store.lists()) if (candidate.id == list) persistedList = candidate;
    QCOMPARE(persistedList.kind, EntityKind::Note); QCOMPARE(persistedList.folderId, folder);
    const QString section = store.createSection(list, QStringLiteral("重要")); QVERIFY(isUuid(section));
    const QString parent = store.createTask(QStringLiteral("父任务"), list, section);
    QVERIFY2(isUuid(parent), qPrintable(store.lastError()));
    const QString child = store.createTask(QStringLiteral("子任务"), list, section, parent);
    QVERIFY2(isUuid(child) && parent != child, qPrintable(store.lastError()));
    QCOMPARE(store.entity(child).parentId, parent);
    QCOMPARE(store.entity(parent).kind, EntityKind::Task); // 清单 kind 不限制实际实体类型
    Entity value = store.entity(parent); value.priority = 3; value.body = QStringLiteral("<b>只按 Markdown 原文保存</b>");
    value.folderId = folder; value.sortOrder = 42; value.pinnedAt = utc(2026, 4, 1, 0, 30);
    value.startAt = utc(2026, 4, 1, 1, 2).addMSecs(345); value.dueAt = utc(2026, 4, 2, 3, 4).addMSecs(567);
    value.hasScheduleRange = true; value.allDayDate = QDate(); value.timeZone = QStringLiteral("Asia/Shanghai");
    value.tags = QStringList{QStringLiteral("工作"), QStringLiteral("Qt")};
    QVERIFY2(store.saveTask(value), qPrintable(store.lastError()));
    QCOMPARE(store.entity(parent).priority, 3); QCOMPARE(store.entity(parent).body, value.body);
    QCOMPARE(store.entity(parent).sortOrder, 42); QCOMPARE(store.entity(parent).pinnedAt, value.pinnedAt);
    QCOMPARE(store.entity(parent).startAt, value.startAt); QVERIFY(!store.entity(parent).allDayDate.isValid());
    QCOMPARE(scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(parent)).toString(), value.body);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM entities WHERE id='%1'").arg(parent)).toLongLong(), qint64(42));
    QCOMPARE(scalar(QStringLiteral("SELECT pinned_at_utc_ms FROM entities WHERE id='%1'").arg(parent)).toLongLong(), utcMs(value.pinnedAt));
    QVERIFY(scalar(QStringLiteral("SELECT start_at_utc_ms FROM entities WHERE id='%1'").arg(parent)).toString().toLongLong() == utcMs(value.startAt));
    QCOMPARE(scalar(QStringLiteral("SELECT all_day_date FROM entities WHERE id='%1'").arg(parent)).isNull(), true);
    QCOMPARE(scalar(QStringLiteral("SELECT timezone FROM entities WHERE id='%1'").arg(parent)).toString(), QStringLiteral("Asia/Shanghai"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tags")).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entity_tags WHERE entity_id='%1'").arg(parent)).toInt(), 2);
    QVERIFY(store.abandonTask(parent)); QCOMPARE(store.entity(parent).status, TaskStatus::Abandoned);
    QVERIFY(!store.entity(parent).completed); QVERIFY(store.entity(parent).abandonedAt.isValid());
    QVERIFY(store.setStatus(parent, TaskStatus::Active)); QVERIFY(!store.entity(parent).abandonedAt.isValid());
    QVERIFY(store.trashTask(parent)); QVERIFY(store.entity(parent).deleted); QCOMPARE(store.entity(parent).status, TaskStatus::Active);
    QVERIFY(store.restoreTask(parent));
    QVERIFY(store.deleteSection(section)); QVERIFY(store.entity(parent).sectionId.isEmpty());
    // 删除清单 = 原子回收站：内容移入收集箱且进入回收站（不再是 active）。
    QVERIFY(store.deleteList(list)); QCOMPARE(store.entity(parent).listId, store.inboxListId()); QCOMPARE(store.entity(child).listId, store.inboxListId());
    QVERIFY(store.entity(parent).deleted); QVERIFY(store.entity(child).deleted);
    QVERIFY(store.entity(parent).deletedAt.isValid());
    QCOMPARE(store.entity(child).parentId, parent);
    QVERIFY(store.restoreTask(parent)); QVERIFY(!store.entity(parent).deleted);
    QCOMPARE(store.entity(parent).listId, store.inboxListId());
    QVERIFY(store.deleteFolder(folder)); QVERIFY(store.folders().isEmpty()); QVERIFY(store.entity(parent).folderId.isEmpty());
    QVERIFY(store.trashTask(parent)); QVERIFY(store.deletePermanently(parent));
    QVERIFY(store.entity(parent).id.isEmpty()); QVERIFY(store.entity(child).id.isEmpty()); // 真正子任务级联
}

void TaskTests::inboxAndOrganizationIntegrity()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString inbox = store.inboxListId();
    QVERIFY(store.groups().isEmpty()); QCOMPARE(store.lists().size(), 1);
    const QString localTask = store.createTask(QStringLiteral("默认进入 inbox")); QVERIFY(!localTask.isEmpty());
    QCOMPARE(store.entity(localTask).listId, inbox); QCOMPARE(store.entity(localTask).kind, EntityKind::Task);
    QVERIFY(!store.deleteList(inbox));
    QSqlQuery deleteInbox(Database::instance().connection());
    deleteInbox.prepare(QStringLiteral("DELETE FROM lists WHERE id=?")); deleteInbox.addBindValue(inbox);
    QVERIFY(!deleteInbox.exec()); QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM lists WHERE is_inbox=1")).toInt(), 1);
    QSqlQuery unsetInbox(Database::instance().connection());
    unsetInbox.prepare(QStringLiteral("UPDATE lists SET is_inbox=0 WHERE id=?")); unsetInbox.addBindValue(inbox);
    QVERIFY(!unsetInbox.exec()); QCOMPARE(scalar(QStringLiteral("SELECT is_inbox FROM lists WHERE id='%1'").arg(inbox)).toInt(), 1);

    const QString folder = store.createFolder(QStringLiteral("单层文件夹")); QVERIFY(isUuid(folder));
    const QString userList = store.createList(QStringLiteral("用户清单"), folder, EntityKind::Note);
    const QString flatList = store.createList(QStringLiteral("平铺清单"), QString(), EntityKind::Note); QVERIFY(isUuid(userList) && isUuid(flatList));
    QVERIFY(store.setListFolder(flatList, folder));
    for (const TaskList &candidate : store.lists()) if (candidate.id == flatList) QCOMPARE(candidate.folderId, folder);
    QVERIFY(store.setListFolder(flatList, QString()));
    QVERIFY(!store.setListFolder(flatList, newUuid())); QVERIFY(!store.setListFolder(inbox, folder));
    const QString userTask = store.createTask(QStringLiteral("用户任务"), userList); QVERIFY(isUuid(userTask));
    Entity moved = store.entity(userTask); moved.folderId = folder; QVERIFY(store.saveTask(moved));
    moved = store.entity(userTask); moved.listId.clear(); QVERIFY(store.saveTask(moved)); QCOMPARE(store.entity(userTask).listId, inbox);
    moved = store.entity(userTask); moved.listId = userList; QVERIFY(store.saveTask(moved)); QCOMPARE(store.entity(userTask).listId, userList);
    moved = store.entity(userTask); moved.tags = QStringList{QStringLiteral("同名")}; QVERIFY(store.saveTask(moved));
    QVERIFY(store.deleteFolder(folder)); QVERIFY(store.folders().isEmpty());
    TaskList unfiled; for (const TaskList &candidate : store.lists()) if (candidate.id == userList) unfiled = candidate;
    QCOMPARE(unfiled.name, QStringLiteral("用户清单")); QVERIFY(unfiled.folderId.isEmpty()); QCOMPARE(unfiled.kind, EntityKind::Note);
    QVERIFY(store.entity(userTask).folderId.isEmpty());
    QVERIFY(store.deleteList(userList)); QCOMPARE(store.entity(userTask).listId, inbox);
    QVERIFY(store.entity(userTask).deleted);   // 删除清单的实体进入回收站而非保持 active

    QVERIFY(store.exportBackup(backupPath())); QVERIFY(store.importBackup(backupPath()));
    QCOMPARE(store.entity(localTask).id, localTask); QCOMPARE(store.entity(localTask).listId, inbox);
    bool importedNoteList = false;
    for (const TaskList &candidate : store.lists())
        if (candidate.id == flatList) importedNoteList = candidate.kind == EntityKind::Note;
    QVERIFY(importedNoteList); // 覆盖恢复保留 inbox 与清单 UUID/kind，不创建副本
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM lists WHERE is_inbox=1")).toInt(), 1);
    QCOMPARE(store.lists().size(), 2); QCOMPARE(store.groups().size(), 1);

    QVERIFY(store.createSection(inbox, QStringLiteral("非法系统分区")).isEmpty());
    QVERIFY(store.createList(QStringLiteral("非法清单"), newUuid()).isEmpty());
    QVERIFY(store.createTask(QStringLiteral("非法任务"), newUuid()).isEmpty());
    QVERIFY(!store.renameFolder(newUuid(), QStringLiteral("不存在")));
    QVERIFY(!store.renameList(newUuid(), QStringLiteral("不存在")));
    QVERIFY(!store.deleteFolder(newUuid())); QVERIFY(!store.deleteList(newUuid())); QVERIFY(!store.deleteSection(newUuid()));
    QSqlQuery invalid(Database::instance().connection());
    invalid.prepare(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'task','非法外键','',?,1,1)"));
    invalid.addBindValue(newUuid()); invalid.addBindValue(newUuid()); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("INSERT INTO entity_tags(id,entity_id,tag_id) VALUES(?,?,?)"));
    invalid.addBindValue(newUuid()); invalid.addBindValue(userTask); invalid.addBindValue(newUuid()); QVERIFY(!invalid.exec());
    QVERIFY(store.load()); QCOMPARE(store.entity(userTask).listId, inbox);
}

void TaskTests::entityKindAndParentConstraints()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("父任务"));
    const QString child = store.createTask(QStringLiteral("子任务"), store.inboxListId(), QString(), parent);
    QVERIFY(isUuid(parent) && isUuid(child));
    const QString note = newUuid();
    QVERIFY(execute(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','note','独立笔记','正文','%2',1,1)")
                        .arg(note, store.inboxListId())));
    QSqlQuery invalid(Database::instance().connection());
    invalid.prepare(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,parent_id,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'note','笔记父引用','',?,?,1,1)"));
    invalid.addBindValue(newUuid()); invalid.addBindValue(store.inboxListId()); invalid.addBindValue(parent); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,parent_id,created_at_utc_ms,updated_at_utc_ms) VALUES(?,'task','跨kind父引用','',?,?,1,1)"));
    invalid.addBindValue(newUuid()); invalid.addBindValue(store.inboxListId()); invalid.addBindValue(note); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("UPDATE entities SET kind='note' WHERE id=?")); invalid.addBindValue(child); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("UPDATE entities SET kind='note' WHERE id=?")); invalid.addBindValue(parent); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("UPDATE entities SET kind='invalid' WHERE id=?")); invalid.addBindValue(note); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("UPDATE entities SET sort_order=-1 WHERE id=?")); invalid.addBindValue(note); QVERIFY(!invalid.exec());
    invalid.prepare(QStringLiteral("UPDATE entities SET parent_id=id WHERE id=?")); invalid.addBindValue(note); QVERIFY(!invalid.exec());
    QVERIFY(store.load()); QCOMPARE(store.entity(note).kind, EntityKind::Note);
    QCOMPARE(store.entity(note).body, QStringLiteral("正文")); QVERIFY(store.entity(note).parentId.isEmpty());
    QCOMPARE(store.entity(parent).kind, EntityKind::Task); QCOMPARE(store.entity(child).parentId, parent);
}

void TaskTests::eventRepositoryTransactionsAndLazyReads()
{
    QVERIFY(open());
    TaskRepository repository(Database::instance().connection());
    const QString inbox = scalar(QStringLiteral("SELECT value FROM local_meta WHERE key='inbox_list_id'")).toString();
    const QDateTime created = utc(2026, 7, 1, 8).addMSecs(123);
    StoredEntity stored;
    stored.entity.id = newUuid(); stored.entity.title = QStringLiteral("事件实体"); stored.entity.body = QStringLiteral("正文");
    stored.entity.listId = inbox; stored.entity.createdAt = created; stored.entity.updatedAt = created;
    EntityChanges changes; changes.saves.append(stored);
    const auto makeEvent = [&](const QString &type, const QString &field, const QDateTime &occurred,
                               const QVariant &oldValue, const QVariant &newValue) {
        EntityEvent event; event.entityId = stored.entity.id; event.entityKind = EntityKind::Task;
        event.entityTitle = stored.entity.title; event.type = type; event.field = field;
        event.oldValue = oldValue; event.newValue = newValue; event.occurredAt = occurred;
        return event;
    };
    changes.eventSaves.append(makeEvent(QStringLiteral("created"), QString(), created.addSecs(1),
                                        QVariant(QVariant::String), QVariant()));
    changes.eventSaves.append(makeEvent(QStringLiteral("changed"), QStringLiteral("body"), created.addSecs(2),
                                        QVariant(QStringLiteral("")), QVariant(QStringLiteral("正文"))));
    changes.eventSaves.append(makeEvent(QStringLiteral("changed"), QStringLiteral("title"), created.addSecs(2),
                                        QVariant(QStringLiteral("旧标题")), QVariant(QStringLiteral(""))));
    EntitySnapshot after; QString error;
    QVERIFY2(repository.apply(&changes, &after, &error), qPrintable(error));
    QCOMPARE(after.entities.size(), 1); QCOMPARE(after.entities.first().revision, qint64(1));
    QCOMPARE(changes.eventSaves.size(), 3);
    const QString generatedBatch = changes.eventSaves.first().batchId;
    QVERIFY(isUuid(generatedBatch));
    for (const EntityEvent &event : changes.eventSaves) {
        QVERIFY(isUuid(event.id)); QCOMPARE(event.batchId, generatedBatch);
    }

    QVector<EntityEvent> events = repository.eventsFor(stored.entity.id, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(0).type, QStringLiteral("created"));
    QVERIFY(!events.at(0).oldValue.isValid()); QVERIFY(!events.at(0).newValue.isValid());
    QCOMPARE(events.at(1).occurredAt, events.at(2).occurredAt);
    QVERIFY(events.at(1).id < events.at(2).id); // occurred_at 相同时以 id 稳定排序
    int bodyIndex = -1, titleIndex = -1;
    for (int i = 0; i < events.size(); ++i) {
        QCOMPARE(events.at(i).batchId, generatedBatch);
        if (events.at(i).field == QStringLiteral("body")) bodyIndex = i;
        if (events.at(i).field == QStringLiteral("title")) titleIndex = i;
    }
    QVERIFY(bodyIndex > 0 && titleIndex > 0);
    QVERIFY(events.at(bodyIndex).oldValue.isValid()); QCOMPARE(events.at(bodyIndex).oldValue.toString(), QString());
    QCOMPARE(events.at(bodyIndex).newValue.toString(), QStringLiteral("正文"));
    QCOMPARE(events.at(titleIndex).oldValue.toString(), QStringLiteral("旧标题"));
    QVERIFY(events.at(titleIndex).newValue.isValid()); QCOMPARE(events.at(titleIndex).newValue.toString(), QString());

    EntitySnapshot lazy; QVERIFY(repository.load(&lazy, &error));
    QCOMPARE(lazy.entities.size(), 1); // 快照只含实体及关联，不加载事件；事件只能由 eventsFor 惰性读取
    EntityEvent mixedFirst = events.first(); mixedFirst.id = newUuid(); mixedFirst.batchId = newUuid();
    EntityEvent mixedSecond = mixedFirst; mixedSecond.id = newUuid(); mixedSecond.batchId = newUuid();
    QVERIFY(!repository.appendEvents(QVector<EntityEvent>{mixedFirst, mixedSecond}, &error));
    QVERIFY(error.contains(QStringLiteral("batch_id")));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entity_events")).toInt(), 3);
    EntityEvent duplicateFirst = events.first(); duplicateFirst.id = newUuid();
    EntityEvent duplicateSecond = duplicateFirst; duplicateSecond.field = QStringLiteral("body");
    QVERIFY(!repository.appendEvents(QVector<EntityEvent>{duplicateFirst, duplicateSecond}, &error));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entity_events")).toInt(), 3);

    StoredEntity rejectedSave = after.entities.first(); rejectedSave.entity.title = QStringLiteral("不应提交");
    rejectedSave.entity.updatedAt = created.addDays(1);
    EntityChanges rejected; rejected.saves.append(rejectedSave);
    EntityEvent duplicate = changes.eventSaves.first(); duplicate.entityTitle = rejectedSave.entity.title;
    duplicate.type = QStringLiteral("changed"); duplicate.field = QStringLiteral("title"); duplicate.occurredAt = created.addSecs(3);
    rejected.eventSaves.append(duplicate);
    EntitySnapshot rejectedAfter = after;
    QVERIFY(!repository.apply(&rejected, &rejectedAfter, &error));
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(stored.entity.id)).toString(), QStringLiteral("事件实体"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(stored.entity.id)).toLongLong(), qint64(1));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entity_events")).toInt(), 3);
    QCOMPARE(rejectedAfter.entities.first().entity.title, QStringLiteral("事件实体"));

    EntityChanges remove; remove.removals.append(after.entities.first()); EntitySnapshot removed;
    QVERIFY2(repository.apply(&remove, &removed, &error), qPrintable(error));
    QVERIFY(removed.entities.isEmpty()); QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities")).toInt(), 0);
    const QVector<EntityEvent> retained = repository.eventsFor(stored.entity.id, &error);
    QCOMPARE(retained.size(), 3); QCOMPARE(retained.first().id, events.first().id);
    EntityEvent orphan = makeEvent(QStringLiteral("deleted"), QString(), created.addDays(2), QVariant(), QVariant());
    QVERIFY2(repository.appendEvents(QVector<EntityEvent>{orphan}, &error), qPrintable(error));
    events = repository.eventsFor(stored.entity.id, &error); QCOMPARE(events.size(), 4);
    QCOMPARE(events.last().type, QStringLiteral("deleted")); QVERIFY(isUuid(events.last().id)); QVERIFY(isUuid(events.last().batchId));
    QVERIFY(repository.eventsFor(QString(), &error).isEmpty()); QVERIFY(!error.isEmpty());
}

void TaskTests::convertKindPreservesFieldsAndAssociations()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("转换文件夹"));
    const QString list = store.createList(QStringLiteral("转换清单"), folder);
    const QString section = store.createSection(list, QStringLiteral("转换分区"));
    const QString id = store.createTask(QStringLiteral("完整转换"), list, section);
    QVERIFY(isUuid(id));
    Entity task = store.entity(id); task.body = QStringLiteral("**保留正文**"); task.folderId = folder;
    task.sortOrder = 73; task.priority = 3; task.pinnedAt = utc(2026, 8, 1, 7, 30);
    task.startAt = utc(2026, 8, 2, 9).addMSecs(111); task.dueAt = utc(2026, 8, 31, 18).addMSecs(222);
    task.hasScheduleRange = true; task.allDayDate = QDate(); task.timeZone = QStringLiteral("Asia/Shanghai"); task.recurrence = QStringLiteral("monthly");
    task.tags = QStringList{QStringLiteral("工作"), QStringLiteral("转换")};
    task.reminderAt = utc(2026, 8, 30, 9); QVERIFY2(store.saveTask(task), qPrintable(store.lastError()));
    QVERIFY(store.saveDesktopState(id, store.entity(id).color, store.entity(id).geometry, store.entity(id).zOrder, store.entity(id).layer, true));
    const QString source = m_directory->filePath(QStringLiteral("转换附件.txt"));
    QVERIFY(writeBytes(source, QByteArray("convert-attachment"))); QVERIFY(!store.importAttachment(id, source).id.isEmpty());
    QVERIFY2(store.setCompleted(id, true), qPrintable(store.lastError()));

    TaskRepository repository(Database::instance().connection()); EntitySnapshot original; QString error;
    QVERIFY2(repository.load(&original, &error), qPrintable(error));
    StoredEntity originalStored; RecurrenceLink link;
    for (const StoredEntity &candidate : original.entities) if (candidate.entity.id == id) originalStored = candidate;
    for (const RecurrenceLink &candidate : original.recurrenceLinks) if (candidate.sourceId == id) link = candidate;
    QCOMPARE(originalStored.entity.id, id); QVERIFY(!link.nextId.isEmpty());
    QCOMPARE(originalStored.entity.reminders.size(), 1);
    const auto stableEntitySql = [](const QString &entityId) {
        return QStringLiteral("SELECT id,title,body_markdown,folder_id,list_id,section_id,parent_id,sort_order,status,"
            "deleted_at_utc_ms,start_at_utc_ms,due_at_utc_ms,all_day_date,all_day_end_date,has_schedule_range,timezone,recurrence_series_id,occurrence_key,"
            "priority,pinned_at_utc_ms,created_at_utc_ms,completed_at_utc_ms,abandoned_at_utc_ms,server_revision "
            "FROM entities WHERE id='%1'").arg(entityId);
    };
    const QList<QVariantList> originalEntity = queryRows(stableEntitySql(id));
    const QList<QVariantList> originalNext = queryRows(stableEntitySql(link.nextId));
    QVERIFY(originalEntity.size() == 1 && originalNext.size() == 1);
    const QString associatedQueries[] = {
        QStringLiteral("SELECT * FROM entity_tags WHERE entity_id='%1' ORDER BY id").arg(id),
        QStringLiteral("SELECT * FROM reminders WHERE entity_id='%1' ORDER BY id").arg(id),
        QStringLiteral("SELECT * FROM attachments WHERE entity_id='%1' ORDER BY id").arg(id),
        QStringLiteral("SELECT * FROM desktop_notes WHERE entity_id='%1' ORDER BY id").arg(id),
        QStringLiteral("SELECT * FROM recurrence_series WHERE id='%1'").arg(originalStored.entity.recurrenceSeriesId),
        QStringLiteral("SELECT * FROM recurrence_links WHERE source_id='%1'").arg(id)
    };
    QList<QList<QVariantList>> originalAssociations;
    for (const QString &sql : associatedQueries) {
        const QList<QVariantList> rows = queryRows(sql); QVERIFY(!rows.isEmpty()); originalAssociations.append(rows);
    }
    const qint64 originalRevision = originalStored.revision;
    const int originalEventCount = repository.eventsFor(id, &error).size();

    EntitySnapshot converted;
    QVERIFY2(repository.convertKind(id, EntityKind::Note, &converted, &error), qPrintable(error));
    QVERIFY(queryRows(stableEntitySql(id)) == originalEntity); QVERIFY(queryRows(stableEntitySql(link.nextId)) == originalNext);
    for (int i = 0; i < originalAssociations.size(); ++i) QVERIFY(queryRows(associatedQueries[i]) == originalAssociations.at(i));
    QCOMPARE(scalar(QStringLiteral("SELECT kind FROM entities WHERE id='%1'").arg(id)).toString(), QStringLiteral("note"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong(), originalRevision + 1);
    StoredEntity convertedStored;
    for (const StoredEntity &candidate : converted.entities) if (candidate.entity.id == id) convertedStored = candidate;
    QCOMPARE(convertedStored.entity.kind, EntityKind::Note); QCOMPARE(convertedStored.revision, originalRevision + 1);
    QCOMPARE(convertedStored.entity.status, TaskStatus::Completed); QCOMPARE(convertedStored.entity.body, task.body);
    QCOMPARE(convertedStored.entity.sortOrder, qint64(73)); QCOMPARE(convertedStored.entity.pinnedAt, task.pinnedAt);
    QCOMPARE(convertedStored.entity.reminders.size(), 1);
    QVERIFY(convertedStored.entity.updatedAt >= originalStored.entity.updatedAt);
    QVector<EntityEvent> events = repository.eventsFor(id, &error); QCOMPARE(events.size(), originalEventCount + 1);
    QVector<EntityEvent> kindEvents;
    for (const EntityEvent &event : events) if (event.type == QStringLiteral("kind_changed")) kindEvents.append(event);
    QCOMPARE(kindEvents.size(), 1); const EntityEvent &conversion = kindEvents.first();
    QCOMPARE(conversion.field, QStringLiteral("kind"));
    QCOMPARE(conversion.oldValue.toString(), QStringLiteral("task")); QCOMPARE(conversion.newValue.toString(), QStringLiteral("note"));
    QCOMPARE(conversion.entityKind, EntityKind::Note); QCOMPARE(conversion.entityTitle, originalStored.entity.title);
    QVERIFY(isUuid(conversion.id)); QVERIFY(isUuid(conversion.batchId));
    QCOMPARE(conversion.occurredAt, convertedStored.entity.updatedAt);

    EntitySnapshot restored;
    QVERIFY2(repository.convertKind(id, EntityKind::Task, &restored, &error), qPrintable(error));
    QVERIFY(queryRows(stableEntitySql(id)) == originalEntity); QVERIFY(queryRows(stableEntitySql(link.nextId)) == originalNext);
    for (int i = 0; i < originalAssociations.size(); ++i) QVERIFY(queryRows(associatedQueries[i]) == originalAssociations.at(i));
    QCOMPARE(scalar(QStringLiteral("SELECT kind FROM entities WHERE id='%1'").arg(id)).toString(), QStringLiteral("task"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong(), originalRevision + 2);
    events = repository.eventsFor(id, &error); QCOMPARE(events.size(), originalEventCount + 2);
    kindEvents.clear(); for (const EntityEvent &event : events) if (event.type == QStringLiteral("kind_changed")) kindEvents.append(event);
    QCOMPARE(kindEvents.size(), 2);
    bool sawNote = false, sawTask = false;
    for (const EntityEvent &event : kindEvents) {
        if (event.newValue.toString() == QStringLiteral("note")) {
            sawNote = true; QCOMPARE(event.oldValue.toString(), QStringLiteral("task")); QCOMPARE(event.entityKind, EntityKind::Note);
        }
        if (event.newValue.toString() == QStringLiteral("task")) {
            sawTask = true; QCOMPARE(event.oldValue.toString(), QStringLiteral("note")); QCOMPARE(event.entityKind, EntityKind::Task);
        }
    }
    QVERIFY(sawNote && sawTask); QVERIFY(kindEvents.at(0).id != kindEvents.at(1).id); QVERIFY(kindEvents.at(0).batchId != kindEvents.at(1).batchId);
}

void TaskTests::convertKindRejectsRelationshipsAndRollsBack()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("有子任务"));
    const QString child = store.createTask(QStringLiteral("自身是子任务"), store.inboxListId(), QString(), parent);
    const QString standalone = store.createTask(QStringLiteral("独立实体"));
    QVERIFY(isUuid(parent) && isUuid(child) && isUuid(standalone));
    TaskRepository repository(Database::instance().connection()); EntitySnapshot after; QString error;
    const auto rows = []() { return queryRows(QStringLiteral("SELECT id,kind,updated_at_utc_ms,revision FROM entities ORDER BY id")); };
    const QList<QVariantList> before = rows(); QCOMPARE(before.size(), 3);
    const int parentEvents = repository.eventsFor(parent, &error).size();
    const int childEvents = repository.eventsFor(child, &error).size();
    const int allEvents = scalar(QStringLiteral("SELECT COUNT(*) FROM entity_events")).toInt();
    QVERIFY(!repository.convertKind(parent, EntityKind::Note, &after, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(rows() == before); QCOMPARE(repository.eventsFor(parent, &error).size(), parentEvents);
    QVERIFY(!repository.convertKind(child, EntityKind::Note, &after, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(rows() == before); QCOMPARE(repository.eventsFor(child, &error).size(), childEvents);
    QVERIFY(!repository.convertKind(standalone, EntityKind::Task, &after, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(!repository.convertKind(newUuid(), EntityKind::Note, &after, &error)); QVERIFY(error.contains(QStringLiteral("不存在")));
    QVERIFY(!repository.convertKind(standalone, static_cast<EntityKind>(99), &after, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(rows() == before); QVERIFY(after.entities.isEmpty());

    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_kind_event BEFORE INSERT ON entity_events WHEN NEW.type='kind_changed' "
        "BEGIN SELECT RAISE(ABORT,'kind event rejected'); END")));
    QVERIFY(!repository.convertKind(standalone, EntityKind::Note, &after, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(rows() == before); QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entity_events")).toInt(), allEvents);
    QVERIFY(after.entities.isEmpty()); QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_kind_event")));
}

void TaskTests::applicationFacadeKindAndPinnedAt()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString noteList = store.createList(QStringLiteral("笔记清单"), QString(), EntityKind::Note);
    const QString section = store.createSection(noteList, QStringLiteral("摘录"));
    const QString note = store.createNote(QStringLiteral("门面笔记"), noteList, section);
    const QString task = store.createTask(QStringLiteral("显式任务"), noteList, section);
    QVERIFY(isUuid(note) && isUuid(task));
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QCOMPARE(store.entity(note).sectionId, section);
    QCOMPARE(store.entity(task).kind, EntityKind::Task); // createTask 不受清单默认类型影响
    Entity inserted; inserted.kind = EntityKind::Note; inserted.title = QStringLiteral("saveTask 新建笔记");
    QVERIFY2(store.saveTask(inserted), qPrintable(store.lastError()));
    bool insertedNote = false;
    for (const Entity &candidate : store.entities())
        if (candidate.title == inserted.title) insertedNote = candidate.kind == EntityKind::Note;
    QVERIFY(insertedNote); // 普通保存不得把调用方给出的 note 偷改为 task
    Entity edited = store.entity(note); edited.body = QStringLiteral("笔记正文"); QVERIFY(store.saveTask(edited));
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QCOMPARE(store.entity(note).body, edited.body);
    edited.kind = EntityKind::Task; QVERIFY(!store.saveTask(edited));
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QCOMPARE(store.entity(note).body, edited.body);

    QVERIFY(store.saveDesktopState(task, NoteColor::Blue, QRect(10, 20, 300, 200), 2, NoteLayer::OnTop, true));
    const QString desktopSql = QStringLiteral("SELECT * FROM desktop_notes WHERE entity_id='%1'").arg(task);
    const QList<QVariantList> desktopBeforePin = queryRows(desktopSql);
    const QDateTime pinned = utc(2026, 9, 1, 8).addMSecs(321);
    QVERIFY2(store.setPinnedAt(task, pinned), qPrintable(store.lastError()));
    QCOMPARE(store.entity(task).pinnedAt, pinned); QVERIFY(store.desktopNote(task).open);
    QVERIFY(queryRows(desktopSql) == desktopBeforePin); // 置顶不触碰桌面行
    QVERIFY(store.saveDesktopState(task, NoteColor::Blue, QRect(10, 20, 300, 200), 2, NoteLayer::OnTop, false));
    QCOMPARE(store.entity(task).pinnedAt, pinned); QVERIFY(!store.desktopNote(task).open);
    const QList<QVariantList> desktopBeforeUnpin = queryRows(desktopSql);
    QVERIFY(store.setPinnedAt(task, QDateTime()));
    QVERIFY(!store.entity(task).pinnedAt.isValid()); QVERIFY(!store.desktopNote(task).open);
    QVERIFY(queryRows(desktopSql) == desktopBeforeUnpin);
    QVERIFY(!store.setPinnedAt(newUuid(), pinned));

    QSignalSpy changed(&store, &TaskStore::taskChanged); QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY2(store.convertKind(task, EntityKind::Note), qPrintable(store.lastError()));
    QCOMPARE(store.entity(task).kind, EntityKind::Note); QCOMPARE(changed.count(), 1); QCOMPARE(reset.count(), 1);
    const qint64 revision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(task)).toLongLong();
    QVERIFY(!store.convertKind(task, EntityKind::Note));
    QCOMPARE(store.entity(task).kind, EntityKind::Note); QCOMPARE(changed.count(), 1); QCOMPARE(reset.count(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(task)).toLongLong(), revision);
}

void TaskTests::journalEditSessionsCommitFullText()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto idsOf = [](const QVector<EntityEvent> &events) {
        QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
    };
    const auto addedSince = [&](const QSet<QString> &before, const QString &entityId) {
        QVector<EntityEvent> added; for (const EntityEvent &event : store.events(entityId)) if (!before.contains(event.id)) added.append(event); return added;
    };
    const QString id = store.createTask(QStringLiteral("原始标题"));
    Entity initial = store.entity(id); initial.body = QStringLiteral("原始正文");
    QVERIFY(store.saveTask(initial));

    // 会话只记自己的写入：saveTask 直存在有活动会话时也必须立即记账（否则差异会凭空丢失）。
    const QString first = store.beginEditSession(id), second = store.beginEditSession(id);
    QVERIFY(isUuid(first) && isUuid(second)); QVERIFY(first != second);
    QVERIFY(store.beginEditSession(newUuid()).isEmpty());
    const QSet<QString> beforeDirect = idsOf(store.events(id));
    Entity edited = store.entity(id); edited.title = QStringLiteral("中间标题"); QVERIFY(store.saveTask(edited));
    edited = store.entity(id); edited.title = QStringLiteral("最终标题"); edited.body = QStringLiteral("**完整新正文**\n第二行");
    QVERIFY(store.saveTask(edited));
    const QVector<EntityEvent> directEvents = addedSince(beforeDirect, id);
    QCOMPARE(directEvents.size(), 3);   // 每次保存的 title/body 差异在保存时即刻直存记录
    bool sawMiddleTitle = false, sawFinalTitle = false, sawFinalBody = false;
    for (const EntityEvent &event : directEvents) {
        QCOMPARE(event.entityId, id); QCOMPARE(event.type, QStringLiteral("changed"));
        QVERIFY(isUuid(event.id)); QVERIFY(!event.oldValue.isValid()); QVERIFY(event.newValue.isValid());
        if (event.field == QStringLiteral("title") && event.newValue.toString() == QStringLiteral("中间标题"))
            sawMiddleTitle = true;
        else if (event.field == QStringLiteral("title") && event.newValue.toString() == QStringLiteral("最终标题"))
            sawFinalTitle = true;
        else if (event.field == QStringLiteral("body") && event.newValue.toString() == edited.body)
            sawFinalBody = true;
        else QFAIL("直存不应记录 title/body 以外的字段");
    }
    QVERIFY(sawMiddleTitle && sawFinalTitle && sawFinalBody);
    edited = store.entity(id); edited.body.clear(); QVERIFY(store.saveTask(edited));
    const QVector<EntityEvent> clearedEvents = addedSince(beforeDirect, id);
    QCOMPARE(clearedEvents.size(), 4);                  // 清空正文同样立即直存记账
    bool sawEmptyBody = false;
    for (const EntityEvent &event : clearedEvents) {
        if (event.field == QStringLiteral("body") && event.newValue.isValid()
            && event.newValue.toString().isEmpty())
            sawEmptyBody = true;
    }
    QVERIFY(sawEmptyBody);
    edited = store.entity(id); edited.body = QStringLiteral("**完整新正文**\n第二行"); QVERIFY(store.saveTask(edited));
    QVERIFY(addedSince(beforeDirect, id).size() == 5);

    // 无自有写入的会话：结束零事件（别人的写入已由直存记账，不再按“基线 vs 当前”兜底）。
    const QSet<QString> beforeFirstEnd = idsOf(store.events(id));
    QVERIFY2(store.endEditSession(first), qPrintable(store.lastError()));
    QVERIFY(addedSince(beforeFirstEnd, id).isEmpty());
    const QString unchanged = store.beginEditSession(id); QVERIFY(isUuid(unchanged));
    const QSet<QString> beforeUnchanged = idsOf(store.events(id));
    QVERIFY(store.endEditSession(unchanged)); QVERIFY(addedSince(beforeUnchanged, id).isEmpty());
    const QSet<QString> beforeSecond = idsOf(store.events(id));
    QVERIFY(store.endAllSessions()); QVERIFY(addedSince(beforeSecond, id).isEmpty());

    // 会话归因提交：保存时不产生直存事件，结束时只补记自己真正写入的字段终值（多次保存合并为终值一条）。
    const Entity writerBase = store.entity(id);   // 最终标题 / **完整新正文**\n第二行
    const QString writer = store.beginEditSession(id);
    const auto writerDraft = [&](const QString &baseline, const QString &title) {
        TextDraft draft; draft.entityId = id; draft.sessionId = writer;
        draft.baselineTitle = baseline; draft.baselineBody = writerBase.body;
        draft.proposedTitle = title; draft.proposedBody = writerBase.body;
        return draft;
    };
    const QSet<QString> beforeWriter = idsOf(store.events(id));
    QCOMPARE(store.commitTextDraft(writerDraft(writerBase.title, QStringLiteral("过程标题"))).status, TextCommitStatus::Saved);
    QCOMPARE(store.commitTextDraft(writerDraft(QStringLiteral("过程标题"), QStringLiteral("过程标题 2"))).status, TextCommitStatus::Saved);
    QCOMPARE(store.commitTextDraft(writerDraft(QStringLiteral("过程标题 2"), QStringLiteral("终版标题"))).status, TextCommitStatus::Saved);
    QVERIFY(addedSince(beforeWriter, id).isEmpty());   // 归因保存的差异推迟到会话结束补记
    QVERIFY2(store.endEditSession(writer), qPrintable(store.lastError()));
    const QVector<EntityEvent> writerEvents = addedSince(beforeWriter, id);
    QCOMPARE(writerEvents.size(), 1);
    QCOMPARE(writerEvents.first().type, QStringLiteral("changed"));
    QCOMPARE(writerEvents.first().field, QStringLiteral("title"));
    QVERIFY(!writerEvents.first().oldValue.isValid());
    QCOMPARE(writerEvents.first().newValue.toString(), QStringLiteral("终版标题"));
    QCOMPARE(writerEvents.first().entityTitle, QStringLiteral("终版标题"));
    QVERIFY(isUuid(writerEvents.first().batchId));

    // 空闲会话不吞无会话写入的事件：不带 sessionId 的文本提交立即直存记账。
    const Entity foreignBase = store.entity(id);   // 终版标题
    const QString idle = store.beginEditSession(id);
    TextDraft foreign; foreign.entityId = id;
    foreign.baselineTitle = foreignBase.title; foreign.baselineBody = foreignBase.body;
    foreign.proposedTitle = foreignBase.title; foreign.proposedBody = QStringLiteral("外来正文");
    const QSet<QString> beforeForeign = idsOf(store.events(id));
    QCOMPARE(store.commitTextDraft(foreign).status, TextCommitStatus::Saved);
    const QVector<EntityEvent> foreignEvents = addedSince(beforeForeign, id);
    QCOMPARE(foreignEvents.size(), 1);
    QCOMPARE(foreignEvents.first().field, QStringLiteral("body"));
    QCOMPARE(foreignEvents.first().newValue.toString(), QStringLiteral("外来正文"));
    const QSet<QString> beforeIdleEnd = idsOf(store.events(id));
    QVERIFY(store.endEditSession(idle));              // 空闲会话结束不重复记账
    QVERIFY(addedSince(beforeIdleEnd, id).isEmpty());

    // 会话结束补账失败：保留会话供重试，重试成功后只记自己的字段终值。
    const Entity retryBase = store.entity(id);   // 终版标题 / 外来正文
    const QString retry = store.beginEditSession(id);
    TextDraft retryDraft; retryDraft.entityId = id; retryDraft.sessionId = retry;
    retryDraft.baselineTitle = retryBase.title; retryDraft.baselineBody = retryBase.body;
    retryDraft.proposedTitle = QStringLiteral("重试标题"); retryDraft.proposedBody = retryBase.body;
    QCOMPARE(store.commitTextDraft(retryDraft).status, TextCommitStatus::Saved);
    const QSet<QString> beforeRetry = idsOf(store.events(id));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_session_event BEFORE INSERT ON entity_events WHEN NEW.type='changed' "
        "BEGIN SELECT RAISE(ABORT,'session event rejected'); END")));
    QVERIFY(!store.endEditSession(retry)); QVERIFY(addedSince(beforeRetry, id).isEmpty());
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_session_event")));
    QVERIFY2(store.endEditSession(retry), qPrintable(store.lastError()));
    const QVector<EntityEvent> retryEvents = addedSince(beforeRetry, id);
    QCOMPARE(retryEvents.size(), 1);
    QCOMPARE(retryEvents.first().field, QStringLiteral("title"));
    QCOMPARE(retryEvents.first().newValue.toString(), QStringLiteral("重试标题"));
    QVERIFY(store.endAllSessions());
    const QSet<QString> beforeDelete = idsOf(store.events(id));
    QVERIFY(store.trashTask(id)); QVERIFY(store.deletePermanently(id));
    const QVector<EntityEvent> deletionEvents = addedSince(beforeDelete, id); QCOMPARE(deletionEvents.size(), 2);
    bool sawTrash = false, sawDelete = false;
    for (const EntityEvent &event : deletionEvents) {
        if (event.type == QStringLiteral("trashed")) sawTrash = true;
        if (event.type == QStringLiteral("deleted")) sawDelete = true;
    }
    QVERIFY(sawTrash && sawDelete);
    QVERIFY(!store.events(id).isEmpty()); // 永久删除后仍可由同一门面查询
}

void TaskTests::textDraftThreeWayMerge()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto idsOf = [](const QVector<EntityEvent> &events) {
        QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
    };
    const auto addedSince = [&](const QSet<QString> &before, const QString &entityId) {
        QVector<EntityEvent> added; for (const EntityEvent &event : store.events(entityId)) if (!before.contains(event.id)) added.append(event); return added;
    };
    const auto revisionOf = [](const QString &id) {
        return scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
    };
    const auto updatedAtMs = [](const QString &id) {
        return scalar(QStringLiteral("SELECT updated_at_utc_ms FROM entities WHERE id='%1'").arg(id)).toLongLong();
    };
    const QString id = store.createTask(QStringLiteral("基线标题")); QVERIFY(isUuid(id));
    Entity seed = store.entity(id); seed.body = QStringLiteral("基线正文"); QVERIFY(store.saveTask(seed));
    const Entity base = store.entity(id);

    // 两窗口共用基线：A 改标题、B 改正文 → 均成功且互不覆盖（不相关字段合并）。
    TextDraft titleOnly; titleOnly.entityId = id;
    titleOnly.baselineTitle = base.title; titleOnly.baselineBody = base.body;
    titleOnly.proposedTitle = QStringLiteral("A 窗口标题"); titleOnly.proposedBody = base.body;
    QVERIFY(titleOnly.titleDirty() && !titleOnly.bodyDirty());
    const TextCommitResult savedTitle = store.commitTextDraft(titleOnly);
    QCOMPARE(savedTitle.status, TextCommitStatus::Saved); QVERIFY(savedTitle.ok());
    QVERIFY(savedTitle.error.isEmpty()); QCOMPARE(savedTitle.entity.id, id);
    QCOMPARE(savedTitle.entity.title, QStringLiteral("A 窗口标题"));
    QCOMPARE(savedTitle.entity.body, base.body);
    TextDraft bodyOnly; bodyOnly.entityId = id;
    bodyOnly.baselineTitle = base.title; bodyOnly.baselineBody = base.body;
    bodyOnly.proposedTitle = base.title; bodyOnly.proposedBody = QStringLiteral("B 窗口正文");
    QCOMPARE(store.commitTextDraft(bodyOnly).status, TextCommitStatus::Saved);
    QCOMPARE(store.entity(id).title, QStringLiteral("A 窗口标题"));
    QCOMPARE(store.entity(id).body, QStringLiteral("B 窗口正文"));

    // 同字段双改且不同 → Conflict：零写入，不碰 revision/updated_at，不产生事件。
    TextDraft staleTitle; staleTitle.entityId = id;
    staleTitle.baselineTitle = base.title; staleTitle.baselineBody = QStringLiteral("B 窗口正文");
    staleTitle.proposedTitle = QStringLiteral("B 窗口标题"); staleTitle.proposedBody = QStringLiteral("B 窗口正文");
    const QSet<QString> beforeConflict = idsOf(store.events(id));
    const qint64 conflictRevision = revisionOf(id), conflictUpdatedAt = updatedAtMs(id);
    const TextCommitResult conflict = store.commitTextDraft(staleTitle);
    QCOMPARE(conflict.status, TextCommitStatus::Conflict); QVERIFY(!conflict.ok());
    QVERIFY(conflict.error.contains(QStringLiteral("标题")));
    QCOMPARE(conflict.entity.title, QStringLiteral("A 窗口标题"));   // 最新值供 UI 刷新与推进基线
    QCOMPARE(conflict.entity.body, QStringLiteral("B 窗口正文"));
    QCOMPARE(revisionOf(id), conflictRevision);
    QCOMPARE(updatedAtMs(id), conflictUpdatedAt);
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(id)).toString(),
             QStringLiteral("A 窗口标题"));
    QVERIFY(addedSince(beforeConflict, id).isEmpty());

    // 双字段同时冲突同样整体拒绝，消息列出全部冲突字段。
    TextDraft bothConflict; bothConflict.entityId = id;
    bothConflict.baselineTitle = base.title; bothConflict.baselineBody = base.body;
    bothConflict.proposedTitle = QStringLiteral("双改标题"); bothConflict.proposedBody = QStringLiteral("双改正文");
    const TextCommitResult both = store.commitTextDraft(bothConflict);
    QCOMPARE(both.status, TextCommitStatus::Conflict);
    QVERIFY(both.error.contains(QStringLiteral("标题")) && both.error.contains(QStringLiteral("正文")));
    QCOMPARE(revisionOf(id), conflictRevision);

    // 远端已等于本地（旧基线提交远端当前值）→ Unchanged，零写入。
    TextDraft alreadyThere; alreadyThere.entityId = id;
    alreadyThere.baselineTitle = base.title; alreadyThere.baselineBody = base.body;
    alreadyThere.proposedTitle = QStringLiteral("A 窗口标题"); alreadyThere.proposedBody = QStringLiteral("B 窗口正文");
    const QSet<QString> beforeUnchanged = idsOf(store.events(id));
    const TextCommitResult unchanged = store.commitTextDraft(alreadyThere);
    QCOMPARE(unchanged.status, TextCommitStatus::Unchanged); QVERIFY(unchanged.ok());
    QCOMPARE(unchanged.entity.title, QStringLiteral("A 窗口标题"));
    QCOMPARE(revisionOf(id), conflictRevision);
    QVERIFY(addedSince(beforeUnchanged, id).isEmpty());

    // 本地恢复基线（proposed==baseline）→ 保留远端值，不覆盖。
    TextDraft reverted; reverted.entityId = id;
    reverted.baselineTitle = base.title; reverted.baselineBody = QStringLiteral("B 窗口正文");
    reverted.proposedTitle = base.title; reverted.proposedBody = QStringLiteral("B 窗口正文");
    QCOMPARE(store.commitTextDraft(reverted).status, TextCommitStatus::Unchanged);
    QCOMPARE(store.entity(id).title, QStringLiteral("A 窗口标题"));

    // 有效改变推进 revision 并沿既有事件链（无活动会话 → 直存 title/body 事件）。
    const QSet<QString> beforeSave = idsOf(store.events(id));
    const qint64 beforeRevision = revisionOf(id);
    TextDraft effective; effective.entityId = id;
    effective.baselineTitle = QStringLiteral("A 窗口标题"); effective.baselineBody = QStringLiteral("B 窗口正文");
    effective.proposedTitle = QStringLiteral("终版标题"); effective.proposedBody = QStringLiteral("终版正文");
    const TextCommitResult finalSave = store.commitTextDraft(effective);
    QCOMPARE(finalSave.status, TextCommitStatus::Saved);
    QCOMPARE(revisionOf(id), beforeRevision + 1);
    QCOMPARE(store.entity(id).title, QStringLiteral("终版标题"));
    QCOMPARE(store.entity(id).body, QStringLiteral("终版正文"));
    const QVector<EntityEvent> savedEvents = addedSince(beforeSave, id);
    QCOMPARE(savedEvents.size(), 2);
    bool sawTitle = false, sawBody = false;
    for (const EntityEvent &event : savedEvents) {
        QCOMPARE(event.type, QStringLiteral("changed"));
        if (event.field == QStringLiteral("title")) {
            sawTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("终版标题"));
        } else if (event.field == QStringLiteral("body")) {
            sawBody = true; QCOMPARE(event.newValue.toString(), QStringLiteral("终版正文"));
        } else QFAIL("正文提交不应记录 title/body 以外的字段");
    }
    QVERIFY(sawTitle && sawBody);
    QVERIFY(store.lastError().isEmpty());
}

void TaskTests::textDraftUnavailableAndFailureAtomicity()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto idsOf = [](const QVector<EntityEvent> &events) {
        QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
    };
    const auto addedSince = [&](const QSet<QString> &before, const QString &entityId) {
        QVector<EntityEvent> added; for (const EntityEvent &event : store.events(entityId)) if (!before.contains(event.id)) added.append(event); return added;
    };
    const auto revisionOf = [](const QString &id) {
        return scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
    };
    const QString id = store.createTask(QStringLiteral("保留标题")); QVERIFY(isUuid(id));
    Entity seed = store.entity(id); seed.body = QStringLiteral("保留正文"); QVERIFY(store.saveTask(seed));
    const Entity base = store.entity(id);
    const auto draftWithTitle = [&](const QString &title) {
        TextDraft draft; draft.entityId = id;
        draft.baselineTitle = base.title; draft.baselineBody = base.body;
        draft.proposedTitle = title; draft.proposedBody = base.body;
        return draft;
    };

    // 目标不存在 → Unavailable，零写入，结果实体为空。
    TextDraft ghost; ghost.entityId = newUuid();
    ghost.baselineTitle = QStringLiteral("x"); ghost.baselineBody = QStringLiteral("y");
    ghost.proposedTitle = QStringLiteral("新"); ghost.proposedBody = QStringLiteral("y");
    const TextCommitResult missing = store.commitTextDraft(ghost);
    QCOMPARE(missing.status, TextCommitStatus::Unavailable);
    QVERIFY(missing.entity.id.isEmpty());
    QVERIFY(!missing.error.isEmpty());

    // 空 id 草稿属于调用方错误 → Failed。
    TextDraft noId; noId.proposedTitle = QStringLiteral("新标题");
    QCOMPARE(store.commitTextDraft(noId).status, TextCommitStatus::Failed);

    // 空白标题 → Failed（沿用既有校验消息），零写入。
    const QSet<QString> beforeEmpty = idsOf(store.events(id));
    const qint64 emptyRevision = revisionOf(id);
    const TextCommitResult emptyTitle = store.commitTextDraft(draftWithTitle(QStringLiteral("   ")));
    QCOMPARE(emptyTitle.status, TextCommitStatus::Failed);
    QVERIFY(emptyTitle.error.contains(QStringLiteral("标题不能为空")));
    QVERIFY(emptyTitle.entity.id.isEmpty());
    QCOMPARE(revisionOf(id), emptyRevision);
    QCOMPARE(store.entity(id).title, QStringLiteral("保留标题"));
    QVERIFY(addedSince(beforeEmpty, id).isEmpty());

    // SQL 失败原子性：拒绝 entities UPDATE → Failed 且事务回滚，无半成品。
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_text_update BEFORE UPDATE ON entities "
                                   "BEGIN SELECT RAISE(ABORT,'text update rejected'); END")));
    const QSet<QString> beforeReject = idsOf(store.events(id));
    const TextCommitResult rejected = store.commitTextDraft(draftWithTitle(QStringLiteral("触发器标题")));
    QCOMPARE(rejected.status, TextCommitStatus::Failed);
    QVERIFY(rejected.error.contains(QStringLiteral("text update rejected")));
    QCOMPARE(revisionOf(id), emptyRevision);
    QCOMPARE(store.entity(id).title, QStringLiteral("保留标题"));
    QCOMPARE(store.entity(id).body, QStringLiteral("保留正文"));
    QVERIFY(addedSince(beforeReject, id).isEmpty());
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_text_update")));
    QCOMPARE(store.commitTextDraft(draftWithTitle(QStringLiteral("重试标题"))).status, TextCommitStatus::Saved);
    QCOMPARE(store.entity(id).title, QStringLiteral("重试标题"));

    // 回收站：Unavailable，实体不可复活；结果带出 deleted 实体供 UI 区分。
    QVERIFY(store.trashTask(id));
    const TextCommitResult trashed = store.commitTextDraft(draftWithTitle(QStringLiteral("回收站标题")));
    QCOMPARE(trashed.status, TextCommitStatus::Unavailable);
    QVERIFY(trashed.entity.deleted);
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(id)).toString(),
             QStringLiteral("重试标题"));
    QVERIFY(store.restoreTask(id));

    // 永久删除后：Unavailable 且不可复活（不重新插入行）。
    QVERIFY(store.trashTask(id)); QVERIFY(store.deletePermanently(id));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(id)).toInt(), 0);
    const TextCommitResult deleted = store.commitTextDraft(draftWithTitle(QStringLiteral("复活标题")));
    QCOMPARE(deleted.status, TextCommitStatus::Unavailable);
    QVERIFY(deleted.entity.id.isEmpty());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(id)).toInt(), 0);

    // CAS：共享同一连接的两个 store；过期快照写整实体被 revision CAS 拒绝。
    const QString casId = store.createTask(QStringLiteral("CAS 标题")); QVERIFY(isUuid(casId));
    Entity casSeed = store.entity(casId); casSeed.body = QStringLiteral("CAS 正文"); QVERIFY(store.saveTask(casSeed));
    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    const Entity casBase = store.entity(casId);
    TextDraft mainDraft; mainDraft.entityId = casId;
    mainDraft.baselineTitle = casBase.title; mainDraft.baselineBody = casBase.body;
    mainDraft.proposedTitle = QStringLiteral("主窗口标题"); mainDraft.proposedBody = casBase.body;
    QCOMPARE(store.commitTextDraft(mainDraft).status, TextCommitStatus::Saved);
    TextDraft staleDraft; staleDraft.entityId = casId;
    staleDraft.baselineTitle = casBase.title; staleDraft.baselineBody = casBase.body;
    staleDraft.proposedTitle = casBase.title; staleDraft.proposedBody = QStringLiteral("过期正文");
    const TextCommitResult casRejected = stale.commitTextDraft(staleDraft);
    QCOMPARE(casRejected.status, TextCommitStatus::Failed);
    QVERIFY(casRejected.error.contains(QStringLiteral("任务已被其他操作修改")));
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(casId)).toString(),
             QStringLiteral("主窗口标题"));
    QCOMPARE(scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(casId)).toString(),
             QStringLiteral("CAS 正文"));
    // 过期 store 重载后重试同一草稿：正文远端未变 → Saved，主窗口标题保留（不相关字段合并）。
    QVERIFY(stale.load());
    QCOMPARE(stale.commitTextDraft(staleDraft).status, TextCommitStatus::Saved);
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(casId)).toString(),
             QStringLiteral("主窗口标题"));
    QCOMPARE(scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(casId)).toString(),
             QStringLiteral("过期正文"));
    // 重载后同字段双改（旧基线对新值）→ Conflict 而非覆盖。
    TextDraft staleConflict; staleConflict.entityId = casId;
    staleConflict.baselineTitle = casBase.title; staleConflict.baselineBody = casBase.body;
    staleConflict.proposedTitle = QStringLiteral("过期标题"); staleConflict.proposedBody = QStringLiteral("过期正文");
    QCOMPARE(stale.commitTextDraft(staleConflict).status, TextCommitStatus::Conflict);
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(casId)).toString(),
             QStringLiteral("主窗口标题"));
}

void TaskTests::textDraftSessionCancelSemantics()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto idsOf = [](const QVector<EntityEvent> &events) {
        QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
    };
    const auto addedSince = [&](const QSet<QString> &before, const QString &entityId) {
        QVector<EntityEvent> added; for (const EntityEvent &event : store.events(entityId)) if (!before.contains(event.id)) added.append(event); return added;
    };
    const auto revisionOf = [](const QString &id) {
        return scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
    };
    const QString id = store.createTask(QStringLiteral("会话标题")); QVERIFY(isUuid(id));
    Entity seed = store.entity(id); seed.body = QStringLiteral("会话正文"); QVERIFY(store.saveTask(seed));

    // 未保存草稿取消：零事件（其他窗口的修改由它自己的会话记账），不误伤其他编辑器会话。
    const QString stayer = store.beginEditSession(id), quitter = store.beginEditSession(id);
    QVERIFY(isUuid(stayer) && isUuid(quitter));
    TextDraft otherDraft; otherDraft.entityId = id; otherDraft.sessionId = stayer;
    otherDraft.baselineTitle = QStringLiteral("会话标题"); otherDraft.baselineBody = QStringLiteral("会话正文");
    otherDraft.proposedTitle = QStringLiteral("会话标题"); otherDraft.proposedBody = QStringLiteral("其他窗口正文");
    QCOMPARE(store.commitTextDraft(otherDraft).status, TextCommitStatus::Saved);
    const QSet<QString> beforeCancel = idsOf(store.events(id));
    QVERIFY(store.cancelEditSession(quitter));
    QVERIFY(addedSince(beforeCancel, id).isEmpty());
    QVERIFY(store.cancelEditSession(quitter));   // 幂等
    // 存活会话不受影响：结束时仍按其基线 vs 当前记一条正文全文差异。
    QVERIFY(store.endEditSession(stayer));
    const QVector<EntityEvent> stayerEvents = addedSince(beforeCancel, id);
    QCOMPARE(stayerEvents.size(), 1);
    QCOMPARE(stayerEvents.first().field, QStringLiteral("body"));
    QCOMPARE(stayerEvents.first().newValue.toString(), QStringLiteral("其他窗口正文"));

    // Unchanged 提交不登记会话归因：会话内只有 Unchanged 提交时，取消零事件。
    const Entity base2 = store.entity(id);   // 会话标题 / 其他窗口正文
    const QString idle = store.beginEditSession(id);
    TextDraft sameDraft; sameDraft.entityId = id; sameDraft.sessionId = idle;
    sameDraft.baselineTitle = base2.title; sameDraft.baselineBody = base2.body;
    sameDraft.proposedTitle = base2.title; sameDraft.proposedBody = base2.body;
    QCOMPARE(store.commitTextDraft(sameDraft).status, TextCommitStatus::Unchanged);
    const QSet<QString> beforeIdle = idsOf(store.events(id));
    QVERIFY(store.cancelEditSession(idle));
    QVERIFY(addedSince(beforeIdle, id).isEmpty());   // 本会话从未写库：取消不重复记账

    // 有真实写入后取消：只补记本会话自己写入的字段终值，不含其他窗口后续修改。
    const Entity base3 = store.entity(id);   // 草稿标题 / 其他窗口正文
    const QString writer = store.beginEditSession(id), later = store.beginEditSession(id);
    TextDraft writerDraft; writerDraft.entityId = id; writerDraft.sessionId = writer;
    writerDraft.baselineTitle = base3.title; writerDraft.baselineBody = base3.body;
    writerDraft.proposedTitle = QStringLiteral("写入标题"); writerDraft.proposedBody = base3.body;
    QCOMPARE(store.commitTextDraft(writerDraft).status, TextCommitStatus::Saved);
    const Entity afterWriter = store.entity(id);
    TextDraft laterDraft; laterDraft.entityId = id; laterDraft.sessionId = later;
    laterDraft.baselineTitle = afterWriter.title; laterDraft.baselineBody = afterWriter.body;
    laterDraft.proposedTitle = afterWriter.title; laterDraft.proposedBody = QStringLiteral("之后正文");
    QCOMPARE(store.commitTextDraft(laterDraft).status, TextCommitStatus::Saved);
    const QSet<QString> beforeWriterCancel = idsOf(store.events(id));
    QVERIFY(store.cancelEditSession(writer));
    const QVector<EntityEvent> writerEvents = addedSince(beforeWriterCancel, id);
    QCOMPARE(writerEvents.size(), 1);
    QCOMPARE(writerEvents.first().type, QStringLiteral("changed"));
    QCOMPARE(writerEvents.first().field, QStringLiteral("title"));
    QVERIFY(!writerEvents.first().oldValue.isValid());
    QCOMPARE(writerEvents.first().newValue.toString(), QStringLiteral("写入标题"));
    QCOMPARE(writerEvents.first().entityTitle, QStringLiteral("写入标题"));
    QVERIFY(store.cancelEditSession(later));
    const QVector<EntityEvent> laterEvents = addedSince(beforeWriterCancel, id);
    QCOMPARE(laterEvents.size(), 2);   // writer 的 title + later 自己的 body，各自会话只记自己的部分
    bool sawWriterTitle = false, sawLaterBody = false;
    for (const EntityEvent &event : laterEvents) {
        if (event.field == QStringLiteral("title")) {
            sawWriterTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("写入标题"));
        } else if (event.field == QStringLiteral("body")) {
            sawLaterBody = true; QCOMPARE(event.newValue.toString(), QStringLiteral("之后正文"));
        }
    }
    QVERIFY(sawWriterTitle && sawLaterBody);

    // sessionId 归属其他实体时不登记（防误归因），也不影响该会话本身。
    const QString otherId = store.createTask(QStringLiteral("另一个实体")); QVERIFY(isUuid(otherId));
    const QString otherSession = store.beginEditSession(otherId);
    const Entity base4 = store.entity(id);   // 写入标题 / 之后正文
    TextDraft mismatchDraft; mismatchDraft.entityId = id; mismatchDraft.sessionId = otherSession;
    mismatchDraft.baselineTitle = base4.title; mismatchDraft.baselineBody = base4.body;
    mismatchDraft.proposedTitle = QStringLiteral("跨会话标题"); mismatchDraft.proposedBody = base4.body;
    QCOMPARE(store.commitTextDraft(mismatchDraft).status, TextCommitStatus::Saved);
    const QSet<QString> beforeOther = idsOf(store.events(otherId));
    QVERIFY(store.cancelEditSession(otherSession));
    QVERIFY(addedSince(beforeOther, otherId).isEmpty());

    // 目标永久删除后：取消仍可用，本会话已保存部分照常补记（事件日志无外键）。
    const QString doomed = store.beginEditSession(id);
    const Entity base5 = store.entity(id);   // 跨会话标题 / 之后正文
    TextDraft doomedDraft; doomedDraft.entityId = id; doomedDraft.sessionId = doomed;
    doomedDraft.baselineTitle = base5.title; doomedDraft.baselineBody = base5.body;
    doomedDraft.proposedTitle = QStringLiteral("删除前标题"); doomedDraft.proposedBody = base5.body;
    QCOMPARE(store.commitTextDraft(doomedDraft).status, TextCommitStatus::Saved);
    QVERIFY(store.trashTask(id)); QVERIFY(store.deletePermanently(id));
    const QSet<QString> beforeDoomed = idsOf(store.events(id));
    QVERIFY(store.cancelEditSession(doomed));
    const QVector<EntityEvent> doomedEvents = addedSince(beforeDoomed, id);
    QCOMPARE(doomedEvents.size(), 1);
    QCOMPARE(doomedEvents.first().field, QStringLiteral("title"));
    QCOMPARE(doomedEvents.first().newValue.toString(), QStringLiteral("删除前标题"));
    QVERIFY(store.beginEditSession(id).isEmpty());                 // 已删除实体无法开新会话
    QVERIFY(store.cancelEditSession(QString()));                   // 未知/空会话幂等成功
    QVERIFY(store.cancelEditSession(newUuid()));

    // 有已保存内容的取消在日志提交失败时必须失败并保留会话供重试（不假成功）。
    const QString failId = store.createTask(QStringLiteral("失败会话标题")); QVERIFY(isUuid(failId));
    const QString failSession = store.beginEditSession(failId);
    TextDraft failDraft; failDraft.entityId = failId; failDraft.sessionId = failSession;
    failDraft.baselineTitle = QStringLiteral("失败会话标题"); failDraft.baselineBody = QString();
    failDraft.proposedTitle = QStringLiteral("已写入标题"); failDraft.proposedBody = QString();
    QCOMPARE(store.commitTextDraft(failDraft).status, TextCommitStatus::Saved);
    const QSet<QString> beforeFail = idsOf(store.events(failId));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_session_event BEFORE INSERT ON entity_events WHEN NEW.type='changed' "
                                   "BEGIN SELECT RAISE(ABORT,'session event rejected'); END")));
    QVERIFY(!store.cancelEditSession(failSession));
    QVERIFY(store.lastError().contains(QStringLiteral("session event rejected")));
    QVERIFY(addedSince(beforeFail, failId).isEmpty());
    QVERIFY(store.cancelEditSession(newUuid()));                   // 失败重试期间不影响其他会话
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_session_event")));
    QVERIFY(store.cancelEditSession(failSession));                 // 重试成功：会话仍在，历史补记
    const QVector<EntityEvent> failEvents = addedSince(beforeFail, failId);
    QCOMPARE(failEvents.size(), 1);
    QCOMPARE(failEvents.first().field, QStringLiteral("title"));
    QCOMPARE(failEvents.first().newValue.toString(), QStringLiteral("已写入标题"));
    QVERIFY(store.cancelEditSession(failSession));                 // 已终结会话幂等

    // 可重入回归：taskChanged 回调里立刻取消会话，归因已在信号发出前登记，不丢不重。
    const QString reId = store.createTask(QStringLiteral("重入标题")); QVERIFY(isUuid(reId));
    const QString reSession = store.beginEditSession(reId);
    bool canceledInSignal = false;
    const auto reentrant = [&]() {
        if (canceledInSignal) return;
        canceledInSignal = true;
        QVERIFY(store.cancelEditSession(reSession));
    };
    const QMetaObject::Connection reConnection =
        QObject::connect(&store, &TaskStore::taskChanged, &store, reentrant, Qt::DirectConnection);
    const QSet<QString> beforeRe = idsOf(store.events(reId));
    TextDraft reDraft; reDraft.entityId = reId; reDraft.sessionId = reSession;
    reDraft.baselineTitle = QStringLiteral("重入标题"); reDraft.baselineBody = QString();
    reDraft.proposedTitle = QStringLiteral("重入新标题"); reDraft.proposedBody = QString();
    QCOMPARE(store.commitTextDraft(reDraft).status, TextCommitStatus::Saved);
    QObject::disconnect(reConnection);
    QVERIFY(canceledInSignal);
    const QVector<EntityEvent> reEvents = addedSince(beforeRe, reId);
    QCOMPARE(reEvents.size(), 1);
    QCOMPARE(reEvents.first().field, QStringLiteral("title"));
    QCOMPARE(reEvents.first().newValue.toString(), QStringLiteral("重入新标题"));

    // 可重入回归：信号回调里的后续改动不会被算成本会话写入。
    const QString re2Id = store.createTask(QStringLiteral("重入二标题")); QVERIFY(isUuid(re2Id));
    const QString re2Session = store.beginEditSession(re2Id);
    Entity re2Base = store.entity(re2Id); re2Base.body = QStringLiteral("重入二正文");
    QVERIFY(store.saveTask(re2Base));   // 会话活跃时的直存也立即记账（抑制只作用于带归因的文本提交）
    re2Base = store.entity(re2Id);
    bool touchedInSignal = false;
    const auto reentrantTouch = [&]() {
        if (touchedInSignal) return;
        touchedInSignal = true;
        Entity touched = store.entity(re2Id);
        touched.body = QStringLiteral("回调正文");
        QVERIFY(store.saveTask(touched));
    };
    const QMetaObject::Connection re2Connection =
        QObject::connect(&store, &TaskStore::taskChanged, &store, reentrantTouch, Qt::DirectConnection);
    TextDraft re2Draft; re2Draft.entityId = re2Id; re2Draft.sessionId = re2Session;
    re2Draft.baselineTitle = re2Base.title; re2Draft.baselineBody = re2Base.body;
    re2Draft.proposedTitle = QStringLiteral("重入二新标题"); re2Draft.proposedBody = re2Base.body;
    QCOMPARE(store.commitTextDraft(re2Draft).status, TextCommitStatus::Saved);
    QObject::disconnect(re2Connection);
    QVERIFY(touchedInSignal);
    QCOMPARE(store.entity(re2Id).body, QStringLiteral("回调正文"));
    const QSet<QString> beforeRe2Cancel = idsOf(store.events(re2Id));
    QVERIFY(store.cancelEditSession(re2Session));
    const QVector<EntityEvent> re2CancelEvents = addedSince(beforeRe2Cancel, re2Id);
    QCOMPARE(re2CancelEvents.size(), 1);   // 只记自己的 title；回调里的 body 改动不属于本会话
    QCOMPARE(re2CancelEvents.first().field, QStringLiteral("title"));
    QCOMPARE(re2CancelEvents.first().newValue.toString(), QStringLiteral("重入二新标题"));

    // endEditSession 语义：会话内多次 commitTextDraft，结束时只按自己写入的字段补记终值（每字段一条）。
    const QString endId = store.createTask(QStringLiteral("结束标题")); QVERIFY(isUuid(endId));
    const QString endSession = store.beginEditSession(endId);
    TextDraft firstFlush; firstFlush.entityId = endId; firstFlush.sessionId = endSession;
    firstFlush.baselineTitle = QStringLiteral("结束标题"); firstFlush.baselineBody = QString();
    firstFlush.proposedTitle = QStringLiteral("中间标题"); firstFlush.proposedBody = QString();
    QCOMPARE(store.commitTextDraft(firstFlush).status, TextCommitStatus::Saved);
    const Entity afterFirst = store.entity(endId);
    TextDraft secondFlush; secondFlush.entityId = endId; secondFlush.sessionId = endSession;
    secondFlush.baselineTitle = afterFirst.title; secondFlush.baselineBody = afterFirst.body;
    secondFlush.proposedTitle = QStringLiteral("最终标题"); secondFlush.proposedBody = QStringLiteral("最终正文");
    QCOMPARE(store.commitTextDraft(secondFlush).status, TextCommitStatus::Saved);
    const QSet<QString> beforeEnd = idsOf(store.events(endId));
    QVERIFY(store.endEditSession(endSession));
    const QVector<EntityEvent> endEvents = addedSince(beforeEnd, endId);
    QCOMPARE(endEvents.size(), 2);
    QCOMPARE(endEvents.first().batchId, endEvents.last().batchId);
    bool endTitle = false, endBody = false;
    for (const EntityEvent &event : endEvents) {
        if (event.field == QStringLiteral("title")) {
            endTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("最终标题"));
        } else if (event.field == QStringLiteral("body")) {
            endBody = true; QCOMPARE(event.newValue.toString(), QStringLiteral("最终正文"));
        }
    }
    QVERIFY(endTitle && endBody);

    // 会话终结时序回归：eventsChanged 回调重入 cancel 同一会话——会话已在通知前终结，
    // 只补记一条自有文本事件，不重复记账，也不推进实体 revision。
    const QString reCancelId = store.createTask(QStringLiteral("重入取消标题")); QVERIFY(isUuid(reCancelId));
    const QString reCancelSession = store.beginEditSession(reCancelId);
    const QString reCancelNeighbor = store.beginEditSession(reCancelId);   // 其他编辑器会话，必须不受影响
    TextDraft reCancelDraft; reCancelDraft.entityId = reCancelId; reCancelDraft.sessionId = reCancelSession;
    reCancelDraft.baselineTitle = QStringLiteral("重入取消标题"); reCancelDraft.baselineBody = QString();
    reCancelDraft.proposedTitle = QStringLiteral("重入取消新标题"); reCancelDraft.proposedBody = QString();
    QCOMPARE(store.commitTextDraft(reCancelDraft).status, TextCommitStatus::Saved);
    const qint64 reCancelRevision = revisionOf(reCancelId);
    bool reEnteredCancel = false;
    const auto reentrantCancel = [&]() {
        if (reEnteredCancel) return;
        reEnteredCancel = true;
        QVERIFY(store.cancelEditSession(reCancelSession));   // 幂等成功：会话已在事件通知前终结
    };
    const QMetaObject::Connection reCancelConnection =
        QObject::connect(&store, &TaskStore::eventsChanged, &store, reentrantCancel, Qt::DirectConnection);
    const QSet<QString> beforeReCancel = idsOf(store.events(reCancelId));
    QVERIFY(store.cancelEditSession(reCancelSession));
    QObject::disconnect(reCancelConnection);
    QVERIFY(reEnteredCancel);   // 回调确实触发，排除“没重入也通过”的假绿
    const QVector<EntityEvent> reCancelEvents = addedSince(beforeReCancel, reCancelId);
    QCOMPARE(reCancelEvents.size(), 1);   // 只有自己的 title 补记，重入不再重复记账
    QCOMPARE(reCancelEvents.first().field, QStringLiteral("title"));
    QCOMPARE(reCancelEvents.first().newValue.toString(), QStringLiteral("重入取消新标题"));
    for (const EntityEvent &event : reCancelEvents)
        QVERIFY(event.field != QStringLiteral("body"));   // 本会话从未写正文：绝不归因到自身
    QCOMPARE(revisionOf(reCancelId), reCancelRevision);   // 取消/重入只写事件日志，不推进实体 revision
    QVERIFY(store.cancelEditSession(reCancelSession));    // 已终结会话再次幂等
    // 其他编辑器会话不受影响：空闲邻居（无自有写入）结束零事件——
    // 他人写入已由其自己的会话在取消时补记过，绝不重复归因。
    const QSet<QString> beforeNeighborEnd = idsOf(store.events(reCancelId));
    QVERIFY(store.endEditSession(reCancelNeighbor));
    QCOMPARE(addedSince(beforeNeighborEnd, reCancelId).size(), 0);

    // 会话终结时序回归：eventsChanged 回调重入 end 同一会话——同样只补记自己真正写入的
    // 字段终值，重入幂等成功，实体 revision 不推进，其他会话不受影响。
    const QString reEndId = store.createTask(QStringLiteral("重入结束标题")); QVERIFY(isUuid(reEndId));
    const QString reEndSession = store.beginEditSession(reEndId);
    const QString reEndNeighbor = store.beginEditSession(reEndId);
    TextDraft reEndDraft; reEndDraft.entityId = reEndId; reEndDraft.sessionId = reEndSession;
    reEndDraft.baselineTitle = QStringLiteral("重入结束标题"); reEndDraft.baselineBody = QString();
    reEndDraft.proposedTitle = QStringLiteral("重入结束新标题"); reEndDraft.proposedBody = QStringLiteral("重入结束新正文");
    QCOMPARE(store.commitTextDraft(reEndDraft).status, TextCommitStatus::Saved);
    const qint64 reEndRevision = revisionOf(reEndId);
    bool reEnteredEnd = false;
    const auto reentrantEnd = [&]() {
        if (reEnteredEnd) return;
        reEnteredEnd = true;
        QVERIFY(store.endEditSession(reEndSession));   // 幂等成功：会话已在事件通知前终结
    };
    const QMetaObject::Connection reEndConnection =
        QObject::connect(&store, &TaskStore::eventsChanged, &store, reentrantEnd, Qt::DirectConnection);
    const QSet<QString> beforeReEnd = idsOf(store.events(reEndId));
    QVERIFY(store.endEditSession(reEndSession));
    QObject::disconnect(reEndConnection);
    QVERIFY(reEnteredEnd);
    const QVector<EntityEvent> reEndEvents = addedSince(beforeReEnd, reEndId);
    QCOMPARE(reEndEvents.size(), 2);   // 一条 title + 一条 body 全文差异，重入不产生第二批
    bool reEndSawTitle = false, reEndSawBody = false;
    for (const EntityEvent &event : reEndEvents) {
        if (event.field == QStringLiteral("title")) {
            reEndSawTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("重入结束新标题"));
        } else if (event.field == QStringLiteral("body")) {
            reEndSawBody = true; QCOMPARE(event.newValue.toString(), QStringLiteral("重入结束新正文"));
        }
    }
    QVERIFY(reEndSawTitle && reEndSawBody);
    QCOMPARE(revisionOf(reEndId), reEndRevision);   // 结束/重入只写事件日志，不推进实体 revision
    QVERIFY(store.endEditSession(reEndSession));    // 已终结会话再次幂等
    // 其他编辑器会话不受影响：空闲邻居结束零事件（自己的 title/body 已在其会话结束时补记）。
    const QSet<QString> beforeReEndNeighbor = idsOf(store.events(reEndId));
    QVERIFY(store.endEditSession(reEndNeighbor));
    QCOMPARE(addedSince(beforeReEndNeighbor, reEndId).size(), 0);
    QVERIFY(store.endAllSessions());
}

void TaskTests::journalScalarBusinessAndLocalOnlyEvents()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto idsOf = [](const QVector<EntityEvent> &events) {
        QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
    };
    const auto addedSince = [&](const QSet<QString> &before, const QString &entityId) {
        QVector<EntityEvent> added; for (const EntityEvent &event : store.events(entityId)) if (!before.contains(event.id)) added.append(event); return added;
    };
    const QString id = store.createTask(QStringLiteral("完整活动")); QVERIFY(isUuid(id));
    QVector<EntityEvent> events = store.events(id); QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().type, QStringLiteral("created"));
    const QJsonObject created = QJsonDocument::fromJson(events.first().newValue.toString().toUtf8()).object();
    QCOMPARE(created.value(QStringLiteral("title")).toString(), QStringLiteral("完整活动"));
    QCOMPARE(created.value(QStringLiteral("body")).toString(), QString()); QCOMPARE(created.size(), 2);

    QSet<QString> before = idsOf(store.events(id));
    QVERIFY(store.saveTask(store.entity(id))); QVERIFY(addedSince(before, id).isEmpty()); // updatedAt 等内部字段不构成实质变化
    for (int i = 1; i <= 3; ++i) {
        before = idsOf(store.events(id)); Entity value = store.entity(id); value.title = QStringLiteral("直接文本 %1").arg(i);
        QVERIFY(store.saveTask(value)); const QVector<EntityEvent> added = addedSince(before, id); QCOMPARE(added.size(), 1);
        QCOMPARE(added.first().field, QStringLiteral("title")); QVERIFY(!added.first().oldValue.isValid());
        QCOMPARE(added.first().newValue.toString(), value.title);
    }

    Entity rollback = store.entity(id); rollback.title = QStringLiteral("回滚标题");
    const qint64 rollbackRevision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_logged_entity_update BEFORE UPDATE ON entities WHEN NEW.title='回滚标题' "
        "BEGIN SELECT RAISE(ABORT,'logged update rejected'); END")));
    before = idsOf(store.events(id)); QVERIFY(!store.saveTask(rollback)); QVERIFY(addedSince(before, id).isEmpty());
    QCOMPARE(store.entity(id).title, QStringLiteral("直接文本 3"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong(), rollbackRevision);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_logged_entity_update"))); // 事件先写也随同事务回滚，不留半条日志

    before = idsOf(store.events(id)); QVERIFY(store.setCompleted(id, true));
    const QVector<EntityEvent> statusEvents = addedSince(before, id); QCOMPARE(statusEvents.size(), 1);
    QCOMPARE(statusEvents.first().field, QStringLiteral("status")); QCOMPARE(statusEvents.first().oldValue.toString(), QStringLiteral("active"));
    QCOMPARE(statusEvents.first().newValue.toString(), QStringLiteral("completed"));
    before = idsOf(store.events(id)); QVERIFY(store.setCompleted(id, true)); QVERIFY(addedSince(before, id).isEmpty());
    QVERIFY(store.setCompleted(id, false));

    const QString list = store.createList(QStringLiteral("活动目标清单"));
    const QString section = store.createSection(list, QStringLiteral("活动分组"));
    const QString folder = store.createFolder(QStringLiteral("活动文件夹"));
    Entity changed = store.entity(id); changed.dueAt = utc(2026, 10, 2, 8).addMSecs(123);
    changed.startAt = utc(2026, 10, 1, 9).addMSecs(456); changed.allDayDate = QDate(); changed.hasScheduleRange = true;
    changed.priority = 3; changed.listId = list; changed.sectionId = section; changed.folderId = folder;
    changed.rrule = QStringLiteral("FREQ=DAILY"); changed.timeZone = QStringLiteral("UTC"); changed.sortOrder = 42;
    changed.tags = QStringList{QStringLiteral("甲"), QStringLiteral("乙")};
    before = idsOf(store.events(id)); QVERIFY2(store.saveTask(changed), qPrintable(store.lastError()));
    const QVector<EntityEvent> scalarEvents = addedSince(before, id);
    QSet<QString> fields; QSet<QString> batches;
    for (const EntityEvent &event : scalarEvents) {
        QCOMPARE(event.type, QStringLiteral("changed")); fields.insert(event.field); batches.insert(event.batchId);
        if (event.field == QStringLiteral("due")) QCOMPARE(event.newValue.toString(), QString::number(utcMs(changed.dueAt)));
        if (event.field == QStringLiteral("start")) QCOMPARE(event.newValue.toString(), QString::number(utcMs(changed.startAt)));
    }
    const QSet<QString> expectedFields = {QStringLiteral("due"), QStringLiteral("start"), QStringLiteral("hasScheduleRange"),
        QStringLiteral("priority"), QStringLiteral("list"), QStringLiteral("recurrence"), QStringLiteral("recurrenceRule"),
        QStringLiteral("timeZone"), QStringLiteral("tags")};
    QVERIFY(fields == expectedFields); QCOMPARE(batches.size(), 1); QVERIFY(isUuid(*batches.constBegin()));
    before = idsOf(store.events(id)); QVERIFY(store.saveTask(store.entity(id))); QVERIFY(addedSince(before, id).isEmpty());

    const QDateTime pinned = utc(2026, 10, 3, 10).addMSecs(789);
    before = idsOf(store.events(id)); QVERIFY(store.setPinnedAt(id, pinned));
    QVector<EntityEvent> added = addedSince(before, id); QCOMPARE(added.size(), 0);
    QCOMPARE(store.entity(id).pinnedAt, pinned); // 置顶持久化但不生成活动日志
    before = idsOf(store.events(id)); QVERIFY(store.setPinnedAt(id, pinned)); QVERIFY(addedSince(before, id).isEmpty());
    before = idsOf(store.events(id)); QVERIFY(store.convertKind(id, EntityKind::Note));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("kind_changed"));
    QVERIFY(store.convertKind(id, EntityKind::Task));

    QTemporaryDir files; QVERIFY(files.isValid()); const QString path = files.filePath(QStringLiteral("event.txt"));
    QVERIFY(writeBytes(path, QByteArray("attachment")));
    before = idsOf(store.events(id)); const Attachment attachment = store.importAttachment(id, path); QVERIFY(!attachment.id.isEmpty());
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("attachment_added"));
    QCOMPARE(added.first().newValue.toString(), attachment.id);
    before = idsOf(store.events(id)); QVERIFY(store.refreshAttachmentState(attachment.id)); QVERIFY(addedSince(before, id).isEmpty());
    before = idsOf(store.events(id));
    QVERIFY(store.saveDesktopState(id, NoteColor::Yellow, QRect(20, 30, 400, 300), 2, NoteLayer::Normal, true));
    QVERIFY(addedSince(before, id).isEmpty()); // 桌面几何、颜色和 open 不进入活动日志

    before = idsOf(store.events(id)); const TaskReminder reminder = store.addReminder(id, QDateTime::currentDateTimeUtc().addSecs(-60)); QVERIFY(isUuid(reminder.id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("reminder_added"));
    QCOMPARE(added.first().newValue.toString(), reminder.id);
    const QDateTime deliveredAt = QDateTime::currentDateTimeUtc(); before = idsOf(store.events(id));
    QVERIFY(store.markReminderDelivered(reminder.id, reminder.triggerAt, deliveredAt)); QVERIFY(addedSince(before, id).isEmpty());
    ReminderScheduler scheduler(&store); before = idsOf(store.events(id));
    QVERIFY2(scheduler.snoozeReminder(reminder.id, 10), qPrintable(store.lastError()));
    added = addedSince(before, id); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().type, QStringLiteral("changed")); QCOMPARE(added.first().field, QStringLiteral("reminder.%1").arg(reminder.id));
    before = idsOf(store.events(id)); QVERIFY(store.removeReminder(reminder.id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("reminder_removed"));
    QCOMPARE(added.first().oldValue.toString(), reminder.id);

    changed = store.entity(id); changed.body = QStringLiteral("- [ ] 这只是正文\n- [x] 不是清单事件");
    before = idsOf(store.events(id)); QVERIFY(store.saveTask(changed));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().field, QStringLiteral("body"));

    before = idsOf(store.events(id)); QVERIFY(store.trashTask(id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("trashed"));
    before = idsOf(store.events(id)); QVERIFY(store.restoreTask(id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("restored"));
    before = idsOf(store.events(id)); QVERIFY(store.removeAttachment(id, attachment.id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("attachment_removed"));
    QCOMPARE(added.first().oldValue.toString(), attachment.id);
    QVERIFY(store.trashTask(id)); before = idsOf(store.events(id)); QVERIFY(store.deletePermanently(id));
    added = addedSince(before, id); QCOMPARE(added.size(), 1); QCOMPARE(added.first().type, QStringLiteral("deleted"));
    QCOMPARE(added.first().oldValue.toString(), id); QVERIFY(store.entity(id).id.isEmpty());
    events = store.events(id); QVERIFY(events.size() > 20); // 无外键事件在永久删除后仍可读
}

void TaskTests::convertedNotesReceiveRemindersWithoutCompleting()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString id = store.createTask(QStringLiteral("转换提醒")); Entity value = store.entity(id);
    value.body = QStringLiteral("转换后保留"); value.dueAt = now.addSecs(3600); value.reminderAt = now.addSecs(-60);
    QVERIFY2(store.saveTask(value), qPrintable(store.lastError()));
    const TaskReminder reminder = store.entity(id).reminders.first();
    ReminderScheduler scheduler(&store); QSignalSpy due(&scheduler, &ReminderScheduler::reminderDue);
    QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    QVERIFY(store.convertKind(id, EntityKind::Note));
    scheduler.checkDue(now);
    QCOMPARE(due.count(), 1); QCOMPARE(triggered.count(), 1);
    QCOMPARE(triggered.first().at(0).toString(), id);
    QCOMPARE(triggered.first().at(1).toString(), reminder.id);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries WHERE reminder_id='%1'").arg(reminder.id)).toInt(), 1);
    QCOMPARE(store.entity(id).kind, EntityKind::Note);
    QCOMPARE(store.entity(id).status, TaskStatus::Active); // 提醒认领/展示不替笔记完成任务
    QVERIFY(!store.entity(id).completed);
    QCOMPARE(store.entity(id).body, value.body); QCOMPARE(store.entity(id).dueAt, value.dueAt);
    scheduler.checkDue(now.addSecs(1)); QCOMPARE(triggered.count(), 1); // 同一安装不重复投递

    QVERIFY2(scheduler.snoozeReminder(reminder.id, 10), qPrintable(store.lastError()));
    QCOMPARE(store.entity(id).reminders.size(), 1);
    QCOMPARE(store.entity(id).reminders.first().id, reminder.id);
    QVERIFY(store.entity(id).reminderAt > now);
    QVERIFY(store.entity(id).reminders.first().ruleJson.isEmpty());
    QCOMPARE(store.entity(id).status, TaskStatus::Active);
    QVERIFY(store.convertKind(id, EntityKind::Task));
    scheduler.checkDue(store.entity(id).reminderAt.addSecs(1));
    QCOMPARE(due.count(), 2); QCOMPARE(triggered.count(), 2);
    QCOMPARE(store.entity(id).body, value.body); QCOMPARE(store.entity(id).dueAt, value.dueAt);
}

void TaskTests::bodyMarkdownCheckboxesRoundTrip()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("正文清单不是进度")); QVERIFY(isUuid(id));
    Entity value = store.entity(id); value.body = QStringLiteral("- [ ] 未完成\n- [x] 完成");
    QVERIFY(store.saveTask(value)); QVERIFY(store.load());
    QCOMPARE(scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(id)).toString(), value.body);
}

void TaskTests::taskAutosaveValidationAndTransactions()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("自动保存")); QVERIFY(!id.isEmpty());
    Entity task = store.entity(id); task.title = QStringLiteral("新标题"); task.body = QStringLiteral("**Markdown**");
    task.color = NoteColor::Purple; task.geometry = QRect(-100, 30, 400, 300); task.zOrder = 7; task.layer = NoteLayer::OnTop;
    QSignalSpy changed(&store, &TaskStore::taskChanged); QVERIFY(store.saveTask(task)); QCOMPARE(changed.count(), 1);
    QVERIFY(store.saveDesktopState(id, store.entity(id).color, store.entity(id).geometry, store.entity(id).zOrder, store.entity(id).layer, true)); QVERIFY(store.desktopNote(id).open);
    Entity persisted = store.entity(id); QCOMPARE(persisted.title, task.title); QCOMPARE(persisted.geometry, task.geometry);
    QCOMPARE(persisted.color, NoteColor::Purple); QCOMPARE(persisted.layer, NoteLayer::OnTop);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM desktop_notes WHERE entity_id='%1'").arg(id)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('geometry','color','z_order','layer')")).toInt(), 0);
    Entity invalid = persisted; invalid.priority = 4; QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY(!store.saveTask(invalid)); QCOMPARE(reset.count(), 0); QCOMPARE(store.entity(id).priority, 0);
    invalid = persisted; invalid.sortOrder = -1; QVERIFY(!store.saveTask(invalid));
    invalid = persisted; invalid.kind = EntityKind::Note; QVERIFY(!store.saveTask(invalid));
    invalid = persisted; invalid.title = QStringLiteral(" \n"); QVERIFY(!store.saveTask(invalid));
    invalid = persisted; invalid.parentId = id; QVERIFY(!store.saveTask(invalid));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_task_update BEFORE UPDATE ON entities WHEN NEW.title='reject' BEGIN SELECT RAISE(ABORT,'injected'); END")));
    Entity rejected = store.entity(id); rejected.title = QStringLiteral("reject"); QVERIFY(!store.saveTask(rejected));
    QCOMPARE(store.entity(id).title, QStringLiteral("新标题")); QCOMPARE(scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(id)).toString(), QStringLiteral("新标题"));
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_task_update")));
    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    Entity current = store.entity(id); current.title = QStringLiteral("已更新"); QVERIFY(store.saveTask(current));
    Entity old = stale.entity(id); old.title = QStringLiteral("陈旧覆盖"); QVERIFY(!stale.saveTask(old));
    QCOMPARE(store.entity(id).title, QStringLiteral("已更新"));
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load()); QCOMPARE(reloaded.entity(id).geometry, task.geometry);
}

void TaskTests::scheduleRangesPersistAndBackupRoundTrip_data()
{
    QTest::addColumn<QDateTime>("startAt");
    QTest::addColumn<QDateTime>("dueAt");
    QTest::addColumn<QDate>("allDayDate");
    QTest::addColumn<QDate>("allDayEndDate");
    QTest::addColumn<bool>("range");
    const QDate day(2026, 11, 15);
    QTest::newRow("all-day-single") << QDateTime() << QDateTime() << day << QDate() << false;
    QTest::newRow("all-day-same-day-range") << QDateTime() << QDateTime() << day << day << true;
    QTest::newRow("all-day-cross-day-range") << QDateTime() << QDateTime() << day << day.addDays(3) << true;
    QTest::newRow("timed-single") << QDateTime() << utc(2026, 11, 15, 18) << QDate() << QDate() << false;
    QTest::newRow("timed-same-day-range")
        << utc(2026, 11, 15, 9) << utc(2026, 11, 15, 18) << QDate() << QDate() << true;
    QTest::newRow("timed-cross-day-range")
        << utc(2026, 11, 15, 22) << utc(2026, 11, 17, 2) << QDate() << QDate() << true;
}

void TaskTests::scheduleRangesPersistAndBackupRoundTrip()
{
    QFETCH(QDateTime, startAt); QFETCH(QDateTime, dueAt);
    QFETCH(QDate, allDayDate); QFETCH(QDate, allDayEndDate); QFETCH(bool, range);
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    Entity draft; draft.title = QStringLiteral("调度范围");
    ScheduleValue schedule; schedule.startAt = startAt; schedule.dueAt = dueAt;
    schedule.allDayDate = allDayDate; schedule.allDayEndDate = allDayEndDate;
    schedule.hasScheduleRange = range; schedule.timeZone = QStringLiteral("UTC");
    QString id = QStringLiteral("not-cleared");
    QVERIFY2(store.saveSchedule(draft, schedule, &id), qPrintable(store.lastError()));
    QVERIFY(isUuid(id)); QCOMPARE(store.entities().size(), 1);
    const auto check = [&](const Entity &value) {
        QCOMPARE(value.startAt, startAt); QCOMPARE(value.dueAt, dueAt);
        QCOMPARE(value.allDayDate, allDayDate); QCOMPARE(value.allDayEndDate, allDayEndDate);
        QCOMPARE(value.hasScheduleRange, range); QCOMPARE(value.timeZone, QStringLiteral("UTC"));
    };
    check(store.entity(id));
    const ScheduleValue facade = store.schedule(id);
    QCOMPARE(facade.startAt, startAt); QCOMPARE(facade.dueAt, dueAt);
    QCOMPARE(facade.allDayDate, allDayDate); QCOMPARE(facade.allDayEndDate, allDayEndDate);
    QCOMPARE(facade.hasScheduleRange, range); QVERIFY(!facade.repeating);
    QCOMPARE(scalar(QStringLiteral("SELECT has_schedule_range FROM entities WHERE id='%1'").arg(id)).toInt(), range ? 1 : 0);
    QCOMPARE(scalar(QStringLiteral("SELECT all_day_end_date FROM entities WHERE id='%1'").arg(id)).isNull(), !allDayEndDate.isValid());
    QVERIFY2(store.load(), qPrintable(store.lastError())); check(store.entity(id));
    const QList<QVariantList> before = queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id"));
    QVERIFY2(store.exportBackup(backupPath()), qPrintable(store.lastError()));
    const QJsonObject root = readMetadata(backupPath());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6);
    const QJsonObject exported = root.value(QStringLiteral("entities")).toArray().first().toObject();
    QCOMPARE(exported.value(QStringLiteral("hasScheduleRange")).toBool(), range);
    QCOMPARE(exported.value(QStringLiteral("allDayEndDate")).isNull(), !allDayEndDate.isValid());
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id")), before);
    check(store.entity(id));
}

void TaskTests::scheduleRejectsInvalidEndpoints_data()
{
    QTest::addColumn<QDateTime>("startAt");
    QTest::addColumn<QDateTime>("dueAt");
    QTest::addColumn<QDate>("allDayDate");
    QTest::addColumn<QDate>("allDayEndDate");
    QTest::addColumn<bool>("range");
    const QDate day(2026, 11, 15);
    QTest::newRow("timed-reversed") << utc(2026, 11, 16, 9) << utc(2026, 11, 15, 18) << QDate() << QDate() << true;
    QTest::newRow("timed-missing-start") << QDateTime() << utc(2026, 11, 15, 18) << QDate() << QDate() << true;
    QTest::newRow("timed-missing-due") << utc(2026, 11, 15, 9) << QDateTime() << QDate() << QDate() << true;
    QTest::newRow("single-start-only") << utc(2026, 11, 15, 9) << QDateTime() << QDate() << QDate() << false;
    QTest::newRow("single-with-end-date") << QDateTime() << QDateTime() << day << day << false;
    QTest::newRow("all-day-reversed") << QDateTime() << QDateTime() << day << day.addDays(-1) << true;
    QTest::newRow("all-day-missing-end") << QDateTime() << QDateTime() << day << QDate() << true;
    QTest::newRow("mixed-all-day-and-timed")
        << utc(2026, 11, 15, 9) << utc(2026, 11, 15, 18) << day << day << true;
}

void TaskTests::scheduleRejectsInvalidEndpoints()
{
    QFETCH(QDateTime, startAt); QFETCH(QDateTime, dueAt);
    QFETCH(QDate, allDayDate); QFETCH(QDate, allDayEndDate); QFETCH(bool, range);
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    Entity draft; draft.title = QStringLiteral("非法调度范围");
    ScheduleValue schedule; schedule.startAt = startAt; schedule.dueAt = dueAt;
    schedule.allDayDate = allDayDate; schedule.allDayEndDate = allDayEndDate;
    schedule.hasScheduleRange = range; schedule.timeZone = QStringLiteral("UTC");
    QString id = QStringLiteral("sentinel");
    QVERIFY(!store.saveSchedule(draft, schedule, &id));
    QVERIFY(id.isEmpty()); QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(store.entities().size(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities")).toInt(), 0);
}

void TaskTests::fullRecurrenceRulesPersistAndAdvance_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QDate>("current");
    QTest::addColumn<QDate>("expected");
    QTest::newRow("weekly-multiple-days") << QStringLiteral("weekly") << QDate(2024, 1, 3) << QDate(2024, 1, 5);
    QTest::newRow("monthly-fixed-31-skips-short-months") << QStringLiteral("monthly-31") << QDate(2024, 1, 31) << QDate(2024, 3, 31);
    QTest::newRow("monthly-last-day") << QStringLiteral("month-end") << QDate(2024, 1, 31) << QDate(2024, 2, 29);
    QTest::newRow("yearly-leap-day") << QStringLiteral("yearly") << QDate(2024, 2, 29) << QDate(2028, 2, 29);
}

void TaskTests::fullRecurrenceRulesPersistAndAdvance()
{
    QFETCH(QString, mode); QFETCH(QDate, current); QFETCH(QDate, expected);
    RecurrenceSpec spec;
    if (mode == QStringLiteral("weekly")) {
        spec.frequency = RecurrenceSpec::Frequency::Weekly; spec.interval = 2;
        spec.weekdays = QVector<int>{Qt::Monday, Qt::Wednesday, Qt::Friday};
    } else if (mode == QStringLiteral("monthly-31")) {
        spec.frequency = RecurrenceSpec::Frequency::Monthly; spec.monthDays = QVector<int>{31};
    } else if (mode == QStringLiteral("month-end")) {
        spec.frequency = RecurrenceSpec::Frequency::Monthly; spec.monthDays = QVector<int>{-1};
    } else {
        spec.frequency = RecurrenceSpec::Frequency::Yearly; spec.month = 2; spec.monthDays = QVector<int>{29};
    }
    const QString ruleJson = recurrenceRuleText(spec); QVERIFY(!ruleJson.isEmpty());
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    Entity draft; draft.title = QStringLiteral("完整重复规则");
    ScheduleValue schedule; schedule.dueAt = QDateTime(current, QTime(10, 0), Qt::UTC);
    schedule.timeZone = QStringLiteral("UTC"); schedule.repeating = true; schedule.recurrence = spec;
    QString id; QVERIFY2(store.saveSchedule(draft, schedule, &id), qPrintable(store.lastError()));
    QVERIFY(isUuid(id));
    const QString seriesId = store.entity(id).recurrenceSeriesId; QVERIFY(isUuid(seriesId));
    RecurrenceSpec restored;
    QVERIFY(store.recurrenceSpec(id, &restored));
    QCOMPARE(recurrenceRuleText(restored), ruleJson);
    QCOMPARE(scalar(QStringLiteral("SELECT rule_json FROM recurrence_series WHERE id='%1'").arg(seriesId)).toString(), ruleJson);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 0);
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    QVERIFY(store.recurrenceSpec(id, &restored)); QCOMPARE(recurrenceRuleText(restored), ruleJson);
    QVERIFY(store.schedule(id).repeating);
    QVERIFY2(store.setCompleted(id, true), qPrintable(store.lastError()));
    QString nextId;
    for (const Entity &candidate : store.entities())
        if (candidate.id != id && candidate.status == TaskStatus::Active
            && candidate.recurrenceSeriesId == seriesId) nextId = candidate.id;
    QVERIFY2(isUuid(nextId), qPrintable(store.lastError()));
    QCOMPARE(store.entity(nextId).dueAt.toUTC().date(), expected);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 1);
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    QVERIFY(store.recurrenceSpec(nextId, &restored)); QCOMPARE(recurrenceRuleText(restored), ruleJson);
    const QList<QVariantList> seriesBefore = queryRows(QStringLiteral("SELECT * FROM recurrence_series ORDER BY id"));
    QVERIFY2(store.exportBackup(backupPath()), qPrintable(store.lastError()));
    const QJsonObject root = readMetadata(backupPath());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6);
    const QJsonObject exportedSeries = root.value(QStringLiteral("recurrenceSeries")).toArray().first().toObject();
    QCOMPARE(exportedSeries.value(QStringLiteral("ruleJson")).toString(), ruleJson);
    QCOMPARE(exportedSeries.value(QStringLiteral("occurrenceOrdinal")).toInt(), 2);
    QCOMPARE(exportedSeries.value(QStringLiteral("completionCount")).toInt(), 1);
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM recurrence_series ORDER BY id")), seriesBefore);
    QVERIFY(store.recurrenceSpec(nextId, &restored)); QCOMPARE(recurrenceRuleText(restored), ruleJson);
    QCOMPARE(store.entity(nextId).dueAt.toUTC().date(), expected);
}

void TaskTests::recurrenceFinalCompletionAndSafeUndoGuard()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    RecurrenceSpec spec; spec.frequency = RecurrenceSpec::Frequency::Daily;
    spec.endMode = RecurrenceSpec::EndMode::Count; spec.endCount = 2;
    Entity draft; draft.title = QStringLiteral("最后完成");
    ScheduleValue schedule; schedule.dueAt = utc(2026, 1, 1, 10); schedule.timeZone = QStringLiteral("UTC");
    schedule.repeating = true; schedule.recurrence = spec;
    QString first; QVERIFY2(store.saveSchedule(draft, schedule, &first), qPrintable(store.lastError()));
    const QString seriesId = store.entity(first).recurrenceSeriesId;
    QVERIFY(store.setCompleted(first, true));
    const QString second = activeTask(store, first); QVERIFY(isUuid(second));
    QCOMPARE(store.entity(second).dueAt, utc(2026, 1, 2, 10));
    QCOMPARE(store.entities().size(), 2);
    QVERIFY2(store.setCompleted(second, true), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2); // 达到 endCount 后只完成当前实例，不生成第三个实例
    QVERIFY(activeTask(store).isEmpty());
    QVERIFY(store.entity(first).completed); QVERIFY(store.entity(second).completed);
    QCOMPARE(scalar(QStringLiteral("SELECT next_id IS NULL FROM recurrence_links WHERE source_id='%1'").arg(second)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QVERIFY(!store.setCompleted(first, false)); // 下一实例已完成，不能通过撤销首项静默删除它
    QVERIFY(store.entity(first).completed); QVERIFY(store.entity(second).completed);
    QCOMPARE(store.entities().size(), 2);
    QVERIFY(store.setCompleted(second, true)); QCOMPARE(store.entities().size(), 2);
    QVERIFY2(store.exportBackup(backupPath()), qPrintable(store.lastError()));
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2);
    QVERIFY(!store.setCompleted(first, false));
    QVERIFY(store.entity(first).completed); QVERIFY(store.entity(second).completed);
    QVERIFY2(store.setCompleted(second, false), qPrintable(store.lastError())); // 最终空凭证可安全撤销，且不生成第三项
    QVERIFY(!store.entity(second).completed); QVERIFY(store.entity(first).completed);
    QCOMPARE(store.entities().size(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links WHERE source_id='%1'").arg(second)).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 1);
    QVERIFY2(store.setCompleted(second, true), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2); QVERIFY(activeTask(store).isEmpty());
    QCOMPARE(scalar(QStringLiteral("SELECT next_id IS NULL FROM recurrence_links WHERE source_id='%1'").arg(second)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
}

void TaskTests::recurrenceProgressCasAndDeletedMiddleUndo()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    RecurrenceSpec daily; daily.frequency = RecurrenceSpec::Frequency::Daily;
    ScheduleValue schedule; schedule.dueAt = utc(2026, 1, 1, 9); schedule.timeZone = QStringLiteral("UTC");
    schedule.repeating = true; schedule.recurrence = daily;
    Entity draft; draft.title = QStringLiteral("并发进度");
    QString first; QVERIFY2(store.saveSchedule(draft, schedule, &first), qPrintable(store.lastError()));
    const QString seriesId = store.entity(first).recurrenceSeriesId;
    QVERIFY(store.setCompleted(first, true));
    const QString second = activeTask(store, first); QVERIFY(isUuid(second));

    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    QVERIFY(store.setCompleted(second, true));
    const QString third = activeTask(store, second); QVERIFY(isUuid(third));
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    const qint64 seriesRevision = scalar(QStringLiteral("SELECT revision FROM recurrence_series WHERE id='%1'").arg(seriesId)).toLongLong();

    // 陈旧快照修改的是未被第二次完成触碰的首个实体；实体 CAS 可通过，但序列 CAS 必须拒绝整批保存。
    ScheduleValue moved = stale.schedule(first); moved.dueAt = moved.dueAt.addDays(10);
    QVERIFY(!stale.saveSchedule(stale.entity(first), moved));
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM recurrence_series WHERE id='%1'").arg(seriesId)).toLongLong(), seriesRevision);
    QCOMPARE(store.entity(first).dueAt, utc(2026, 1, 1, 9));
    QVERIFY(stale.load());
    moved = stale.schedule(first); moved.dueAt = moved.dueAt.addDays(10);
    QVERIFY2(stale.saveSchedule(stale.entity(first), moved), qPrintable(stale.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);

    QVERIFY(store.load());
    schedule = ScheduleValue(); schedule.dueAt = utc(2026, 2, 1, 9); schedule.timeZone = QStringLiteral("UTC");
    schedule.repeating = true; schedule.recurrence = daily;
    draft = Entity(); draft.title = QStringLiteral("中间实例删除");
    QString a; QVERIFY2(store.saveSchedule(draft, schedule, &a), qPrintable(store.lastError()));
    QVERIFY(store.setCompleted(a, true));
    const QString b = scalar(QStringLiteral("SELECT next_id FROM recurrence_links WHERE source_id='%1'").arg(a)).toString();
    QVERIFY(store.setCompleted(b, true));
    const QString c = scalar(QStringLiteral("SELECT next_id FROM recurrence_links WHERE source_id='%1'").arg(b)).toString();
    QVERIFY(isUuid(b)); QVERIFY(isUuid(c));
    const QString chainSeries = store.entity(a).recurrenceSeriesId;
    QVERIFY(store.trashTask(b)); QVERIFY(store.deletePermanently(b));
    QCOMPARE(scalar(QStringLiteral("SELECT next_id IS NULL FROM recurrence_links WHERE source_id='%1'").arg(a)).toInt(), 1);
    QVERIFY(!store.setCompleted(a, false));
    QVERIFY(store.entity(a).completed); QVERIFY(!store.entity(c).completed);
    QCOMPARE(store.entity(c).status, TaskStatus::Active);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(chainSeries)).toInt(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(chainSeries)).toInt(), 2);
    QVERIFY(store.setCompleted(a, true));
    QCOMPARE(store.entities().size(), 5); // 第一组三项 + 第二组 A/C；幂等完成不得重建 B 或与 C 冲突
    QVERIFY(third != c);
}

void TaskTests::relativeReminderRulesRecalculateAndSnoozeOnce()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("相对提醒")); QVERIFY(isUuid(id));
    ScheduleValue schedule = store.schedule(id);
    schedule.startAt = utc(2026, 1, 10, 10); schedule.dueAt = utc(2026, 1, 10, 18);
    schedule.hasScheduleRange = true; schedule.timeZone = QStringLiteral("UTC");
    ReminderSpec atStart; atStart.anchor = ReminderSpec::Anchor::Start; atStart.offset = 30;
    ReminderSpec beforeEnd; beforeEnd.anchor = ReminderSpec::Anchor::End;
    beforeEnd.unit = ReminderSpec::Unit::Hours; beforeEnd.offset = 2;
    ReminderSpec atSchedule; atSchedule.anchor = ReminderSpec::Anchor::Schedule; atSchedule.offset = 0;
    const QVector<ReminderSpec> specs{atStart, beforeEnd, atSchedule};
    for (const ReminderSpec &spec : specs) {
        TaskReminder reminder; reminder.triggerAt = schedule.dueAt; // 权威 ruleJson 会重算，不信任调用方旧触发值
        reminder.ruleJson = reminderRuleText(spec); schedule.reminders.append(reminder);
    }
    QVERIFY2(store.saveSchedule(store.entity(id), schedule), qPrintable(store.lastError()));
    QVector<TaskReminder> saved = store.reminders(id); QCOMPARE(saved.size(), 3);
    QCOMPARE(saved.at(0).triggerAt, schedule.startAt.addSecs(-30 * 60));
    QCOMPARE(saved.at(1).triggerAt, schedule.dueAt.addSecs(-2 * 3600));
    QCOMPARE(saved.at(2).triggerAt, schedule.startAt);
    const QStringList ids{saved.at(0).id, saved.at(1).id, saved.at(2).id};
    const QStringList rules{saved.at(0).ruleJson, saved.at(1).ruleJson, saved.at(2).ruleJson};
    for (int i = 0; i < 3; ++i) {
        QVERIFY(isUuid(ids.at(i))); QCOMPARE(rules.at(i), reminderRuleText(specs.at(i)));
        QCOMPARE(scalar(QStringLiteral("SELECT rule_json FROM reminders WHERE id='%1'").arg(ids.at(i))).toString(), rules.at(i));
    }

    schedule = store.schedule(id); schedule.startAt = schedule.startAt.addDays(1); schedule.dueAt = schedule.dueAt.addDays(2);
    QVERIFY2(store.saveSchedule(store.entity(id), schedule), qPrintable(store.lastError()));
    saved = store.reminders(id); QCOMPARE(saved.size(), 3);
    QCOMPARE(saved.at(0).triggerAt, schedule.startAt.addSecs(-30 * 60));
    QCOMPARE(saved.at(1).triggerAt, schedule.dueAt.addSecs(-2 * 3600));
    QCOMPARE(saved.at(2).triggerAt, schedule.startAt);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(saved.at(i).id, ids.at(i)); QCOMPARE(saved.at(i).ruleJson, rules.at(i));
    }
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    saved = store.reminders(id);
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(saved.at(i).id, ids.at(i)); QCOMPARE(saved.at(i).ruleJson, rules.at(i));
        QVERIFY(store.markReminderDelivered(ids.at(i), saved.at(i).triggerAt, QDateTime::currentDateTimeUtc()));
    }

    ReminderScheduler scheduler(&store); QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    QVERIFY2(scheduler.snoozeReminder(ids.at(0), 10), qPrintable(store.lastError()));
    saved = store.reminders(id); QCOMPARE(saved.size(), 3);
    QCOMPARE(saved.at(0).id, ids.at(0)); QVERIFY(saved.at(0).ruleJson.isEmpty()); // 只把此项转为一次性绝对时刻
    QVERIFY(saved.at(0).triggerAt > QDateTime::currentDateTimeUtc().addSecs(9 * 60));
    QVERIFY(!store.isReminderDelivered(ids.at(0)));
    for (int i = 1; i < 3; ++i) {
        QCOMPARE(saved.at(i).id, ids.at(i)); QCOMPARE(saved.at(i).ruleJson, rules.at(i));
        QCOMPARE(saved.at(i).triggerAt, i == 1 ? schedule.dueAt.addSecs(-2 * 3600) : schedule.startAt);
        QVERIFY(store.isReminderDelivered(ids.at(i)));
    }
    scheduler.checkDue(saved.at(0).triggerAt.addSecs(1));
    QCOMPARE(triggered.count(), 1); QCOMPARE(triggered.first().at(1).toString(), ids.at(0));
    scheduler.checkDue(saved.at(0).triggerAt.addDays(1));
    QCOMPARE(triggered.count(), 1); // 没有持续提醒或间隔重复投递
    QCOMPARE(store.entity(id).reminders.size(), 3);
}

void TaskTests::recurrenceMonthEndAndOccurrenceUnique()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString january = store.createTask(QStringLiteral("月末"));
    Entity task = store.entity(january); task.startAt = utc(2025, 1, 30, 9); task.dueAt = utc(2025, 1, 31, 10);
    task.hasScheduleRange = true;
    const QDateTime firstReminder = task.dueAt.addSecs(-1800), secondReminder = task.dueAt.addSecs(-600);
    task.reminderAt = firstReminder; QVERIFY(store.saveTask(task));
    const TaskReminder second = store.addReminder(january, secondReminder); QVERIFY(!second.id.isEmpty());
    ScheduleValue repeating = store.schedule(january); repeating.repeating = true;
    repeating.recurrence.frequency = RecurrenceSpec::Frequency::Monthly;
    repeating.recurrence.monthDays = QVector<int>{-1};
    QVERIFY2(store.saveSchedule(store.entity(january), repeating), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT rule_json FROM recurrence_series WHERE id='%1'")
                        .arg(store.entity(january).recurrenceSeriesId)).toString(),
             recurrenceRuleText(repeating.recurrence));
    QVERIFY(store.setCompleted(january, true)); QCOMPARE(store.entities().size(), 2);
    const QString february = activeTask(store); QVERIFY(!february.isEmpty()); Entity next = store.entity(february);
    QCOMPARE(next.dueAt, utc(2025, 2, 28, 10)); QCOMPARE(next.startAt, next.dueAt.addSecs(-25 * 3600));
    QCOMPARE(next.reminders.size(), 2); QCOMPARE(next.reminders.at(0).triggerAt, next.dueAt.addSecs(-1800));
    QCOMPARE(next.reminders.at(1).triggerAt, next.dueAt.addSecs(-600));
    QCOMPARE(next.recurrenceSeriesId, store.entity(january).recurrenceSeriesId);
    QCOMPARE(next.rrule, QStringLiteral("FREQ=MONTHLY;INTERVAL=1")); QVERIFY(!next.occurrenceKey.isEmpty());
    QSqlQuery duplicate(Database::instance().connection());
    duplicate.prepare(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,created_at_utc_ms,updated_at_utc_ms,recurrence_series_id,occurrence_key) SELECT ?,kind,title,body_markdown,list_id,created_at_utc_ms,updated_at_utc_ms,recurrence_series_id,occurrence_key FROM entities WHERE id=?"));
    duplicate.addBindValue(newUuid()); duplicate.addBindValue(february); QVERIFY(!duplicate.exec());
    QVERIFY(store.setCompleted(february, true)); const QString march = activeTask(store);
    QCOMPARE(store.entity(march).dueAt, utc(2025, 3, 31, 10));
}

void TaskTests::recurrenceDailyWeeklyAndSafeUndo()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    for (const QString &rule : {QStringLiteral("daily"), QStringLiteral("FREQ=WEEKLY;INTERVAL=2")}) {
        const QString id = store.createTask(QStringLiteral("循环")); Entity task = store.entity(id);
        task.dueAt = utc(2020, 1, 1, 23, 15); task.reminderAt = task.dueAt.addSecs(-120); task.recurrence = rule;
        task.tags = QStringList{QStringLiteral("重复标签")}; QVERIFY(store.saveTask(task)); QVERIFY(store.setCompleted(id, true));
        QString nextId;
        for (const Entity &candidate : store.entities())
            if (candidate.id != id && candidate.recurrenceSeriesId == store.entity(id).recurrenceSeriesId) nextId = candidate.id;
        Entity next = store.entity(nextId); QVERIFY(!next.id.isEmpty());
        QCOMPARE(next.dueAt, task.dueAt.addDays(rule == QStringLiteral("daily") ? 1 : 14));
        QCOMPARE(next.reminderAt, next.dueAt.addSecs(-120)); QCOMPARE(next.tags, task.tags);
    }
    const QString id = store.createTask(QStringLiteral("安全撤销")); Entity task = store.entity(id);
    const auto nextFor = [&store](const QString &source) {
        QString result;
        for (const Entity &candidate : store.entities())
            if (candidate.id != source && candidate.recurrenceSeriesId == store.entity(source).recurrenceSeriesId) result = candidate.id;
        return result;
    };
    task.dueAt = utc(2026, 1, 31); task.recurrence = QStringLiteral("monthly"); QVERIFY(store.saveTask(task));
    QVERIFY(store.setCompleted(id, true)); const QString generated = nextFor(id);
    QVERIFY(store.setCompleted(id, true)); QVERIFY(store.load()); QCOMPARE(store.entities().size(), 6);
    QVERIFY(store.setCompleted(id, false)); QVERIFY(store.entity(generated).id.isEmpty()); QVERIFY(!store.entity(id).completed);
    QVERIFY(store.setCompleted(id, true)); const QString editedId = nextFor(id);
    Entity edited = store.entity(editedId); edited.title = QStringLiteral("后续已编辑"); QVERIFY(store.saveTask(edited));
    QSignalSpy changed(&store, &TaskStore::taskChanged); QVERIFY(!store.setCompleted(id, false));
    QVERIFY(store.entity(id).completed); QCOMPARE(changed.count(), 0);
    const QString seriesId = store.entity(id).recurrenceSeriesId;
    const int beforeTasks = store.entities().size();
    const int beforeLinks = scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt();
    const int beforeMembers = scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt();
    QVERIFY(store.trashTask(editedId)); QVERIFY(store.deletePermanently(editedId));
    QCOMPARE(store.entities().size(), beforeTasks - 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), beforeLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), beforeMembers - 1);
    QCOMPARE(scalar(QStringLiteral("SELECT next_id IS NULL FROM recurrence_links WHERE source_id='%1'").arg(id)).toInt(), 1);
    QVERIFY2(store.setCompleted(id, false), qPrintable(store.lastError())); // 空凭证只撤销完成，不隐式生成替代品
    QVERIFY(!store.entity(id).completed);
    QCOMPARE(store.entities().size(), beforeTasks - 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), beforeLinks - 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), beforeMembers - 1);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 0);
    QVERIFY(store.load()); QVERIFY(!store.entity(id).completed);
    QCOMPARE(store.entities().size(), beforeTasks - 1);
    QVERIFY2(store.setCompleted(id, true), qPrintable(store.lastError()));
    const QString regenerated = nextFor(id); QVERIFY(isUuid(regenerated)); QVERIFY(regenerated != editedId);
    QVERIFY(store.entity(id).completed);
    const int stableTasks = store.entities().size();
    const int stableLinks = scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt();
    const int stableMembers = scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt();
    QCOMPARE(stableTasks, beforeTasks); QCOMPARE(stableLinks, beforeLinks); QCOMPARE(stableMembers, beforeMembers);
    QCOMPARE(scalar(QStringLiteral("SELECT occurrence_ordinal FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT completion_count FROM recurrence_series WHERE id='%1'").arg(seriesId)).toInt(), 1);
    QVERIFY(store.setCompleted(id, true)); QVERIFY(store.setCompleted(id, true)); // 重复完成幂等，不重复生成
    QCOMPARE(store.entities().size(), stableTasks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), stableLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), stableMembers);
    QVERIFY(store.load());
    QCOMPARE(store.entities().size(), stableTasks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), stableLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), stableMembers);
    QVERIFY(store.entity(id).completed);
}

void TaskTests::recurrenceUndoPreservesAssociatedData()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto createCompleted = [&store](const QString &title) {
        const QString source = store.createTask(title); Entity value = store.entity(source);
        value.dueAt = utc(2026, 8, 1); value.recurrence = QStringLiteral("daily");
        store.saveTask(value); store.setCompleted(source, true); return source;
    };
    const auto nextFor = [&store](const QString &source) {
        QString result;
        for (const Entity &candidate : store.entities())
            if (candidate.id != source && candidate.recurrenceSeriesId == store.entity(source).recurrenceSeriesId) result = candidate.id;
        return result;
    };
    const QString attachmentSourceTask = createCompleted(QStringLiteral("附件保护撤销"));
    const QString withAttachment = nextFor(attachmentSourceTask); QVERIFY(!withAttachment.isEmpty());
    const QString file = m_directory->filePath(QStringLiteral("next.txt")); QVERIFY(writeBytes(file, QByteArray("must-survive")));
    const Attachment attachment = store.importAttachment(withAttachment, file); QVERIFY(!attachment.id.isEmpty());
    const int taskCount = store.entities().size();
    const qint64 nextRevision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(withAttachment)).toLongLong();
    QVERIFY(!store.setCompleted(attachmentSourceTask, false));
    QVERIFY(store.entity(attachmentSourceTask).completed); QCOMPARE(store.entities().size(), taskCount);
    QCOMPARE(store.attachments(withAttachment).size(), 1); QVERIFY(QFile::exists(attachmentPath(attachment.relativePath)));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(withAttachment)).toLongLong(), nextRevision);
    const QString childSourceTask = createCompleted(QStringLiteral("子任务保护撤销"));
    const QString withChild = nextFor(childSourceTask); QVERIFY(!withChild.isEmpty());
    const QString child = store.createTask(QStringLiteral("后续真正子任务"), store.entity(withChild).listId, QString(), withChild);
    QVERIFY(!child.isEmpty()); const int beforeChildUndo = store.entities().size();
    QVERIFY(!store.setCompleted(childSourceTask, false));
    QVERIFY(store.entity(childSourceTask).completed); QCOMPARE(store.entities().size(), beforeChildUndo);
    QCOMPARE(store.entity(child).parentId, withChild); QVERIFY(!store.entity(withChild).id.isEmpty());
}

void TaskTests::staleStoreCannotUndoAssociatedOccurrence()
{
    QVERIFY(open());
    TaskStore a(Database::instance().connection()); QVERIFY(a.load());
    TaskStore b(Database::instance().connection()); QVERIFY(b.load());
    const auto createCompleted = [&a](const QString &title) {
        const QString source = a.createTask(title); Entity value = a.entity(source);
        value.dueAt = utc(2026, 10, 1); value.recurrence = QStringLiteral("daily");
        a.saveTask(value); a.setCompleted(source, true); return source;
    };
    const auto nextFor = [](TaskStore &store, const QString &source) {
        QString result;
        for (const Entity &candidate : store.entities())
            if (candidate.id != source && candidate.recurrenceSeriesId == store.entity(source).recurrenceSeriesId) result = candidate.id;
        return result;
    };
    const QString attachmentSource = createCompleted(QStringLiteral("双 store 附件"));
    const QString withAttachment = nextFor(a, attachmentSource); QVERIFY(!withAttachment.isEmpty());
    QVERIFY(a.load()); QVERIFY(b.load()); // 双方在关联写入前都持有同一代快照
    const qint64 oldRevision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(withAttachment)).toLongLong();
    const QString file = m_directory->filePath(QStringLiteral("stale.txt")); QVERIFY(writeBytes(file, QByteArray("still-present")));
    const Attachment attachment = b.importAttachment(withAttachment, file); QVERIFY2(!attachment.id.isEmpty(), qPrintable(b.lastError()));
    QVERIFY(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(withAttachment)).toLongLong() > oldRevision);
    QVERIFY(!a.setCompleted(attachmentSource, false)); // A 的快照没有附件，必须由 next revision CAS 拒绝
    QVERIFY(a.entity(attachmentSource).completed);
    QCOMPARE(scalar(QStringLiteral("SELECT status FROM entities WHERE id='%1'").arg(attachmentSource)).toString(), QStringLiteral("completed"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links WHERE source_id='%1'").arg(attachmentSource)).toInt(), 1);
    QVERIFY(b.load()); QCOMPARE(b.attachments(withAttachment).size(), 1);
    QVERIFY(QFile::exists(attachmentPath(attachment.relativePath))); QVERIFY(!b.entity(withAttachment).id.isEmpty());

    QVERIFY(a.load());
    const QString childSource = createCompleted(QStringLiteral("双 store 子任务"));
    const QString withChild = nextFor(a, childSource); QVERIFY(!withChild.isEmpty());
    QVERIFY(a.load()); QVERIFY(b.load());
    Entity childValue; childValue.title = QStringLiteral("另一 store 的子任务");
    childValue.listId = b.entity(withChild).listId; childValue.parentId = withChild;
    QVERIFY2(b.saveTask(childValue), qPrintable(b.lastError())); // 直接公开插入式 saveTask，不走 createTask
    QString child;
    for (const Entity &candidate : b.entities())
        if (candidate.parentId == withChild && candidate.title == childValue.title) child = candidate.id;
    QVERIFY(!child.isEmpty());
    QVERIFY(!a.setCompleted(childSource, false)); // A 不知道新子任务，CAS 仍必须失败
    QVERIFY(a.entity(childSource).completed);
    QCOMPARE(scalar(QStringLiteral("SELECT status FROM entities WHERE id='%1'").arg(childSource)).toString(), QStringLiteral("completed"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links WHERE source_id='%1'").arg(childSource)).toInt(), 1);
    QVERIFY(b.load()); QCOMPARE(b.entity(child).parentId, withChild); QVERIFY(!b.entity(withChild).id.isEmpty());
    QCOMPARE(b.attachments(withAttachment).size(), 1); QVERIFY(QFile::exists(attachmentPath(attachment.relativePath)));
}

void TaskTests::cloneCompletedRecurringTask()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString list = store.createList(QStringLiteral("克隆清单")); const QString section = store.createSection(list, QStringLiteral("阶段"));
    const QString source = store.createTask(QStringLiteral("已完成重复源"), list, section);
    Entity value = store.entity(source); value.body = QStringLiteral("**克隆 Markdown**"); value.priority = 3;
    value.sortOrder = 17; value.pinnedAt = utc(2026, 8, 31, 12);
    value.startAt = utc(2026, 9, 1, 9); value.dueAt = utc(2026, 9, 1, 18); value.allDayDate = QDate(); value.hasScheduleRange = true;
    value.tags = QStringList{QStringLiteral("克隆标签")}; value.recurrence = QStringLiteral("daily"); value.reminderAt = value.dueAt.addSecs(-300);
    QVERIFY(store.saveTask(value));
    const QString file = m_directory->filePath(QStringLiteral("clone.txt")); QVERIFY(writeBytes(file, QByteArray("not-copied")));
    QVERIFY(!store.importAttachment(source, file).id.isEmpty());
    QVERIFY(store.saveDesktopState(source, NoteColor::Blue, QRect(1, 2, 300, 200), 4, NoteLayer::OnTop, true));
    QVERIFY(store.setCompleted(source, true)); QCOMPARE(store.entities().size(), 2);
    const QString series = store.entity(source).recurrenceSeriesId;
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(series)).toInt(), 2);
    const QString clone = store.cloneTask(source); QVERIFY2(isUuid(clone), qPrintable(store.lastError())); QVERIFY(clone != source);
    QCOMPARE(store.entities().size(), 3); // 克隆已完成重复源不得额外生成 occurrence
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(series)).toInt(), 2);
    const Entity copy = store.entity(clone); QCOMPARE(copy.title, value.title); QCOMPARE(copy.body, value.body);
    QCOMPARE(copy.listId, list); QCOMPARE(copy.sectionId, section); QCOMPARE(copy.priority, 3);
    QCOMPARE(copy.sortOrder, qint64(17)); QCOMPARE(copy.pinnedAt, value.pinnedAt);
    QCOMPARE(copy.startAt, value.startAt); QCOMPARE(copy.dueAt, value.dueAt); QVERIFY(!copy.allDayDate.isValid());
    QCOMPARE(copy.tags, value.tags); QCOMPARE(copy.status, TaskStatus::Active); QVERIFY(!copy.completed && !copy.deleted);
    QVERIFY(!copy.completedAt.isValid() && !copy.abandonedAt.isValid() && !copy.deletedAt.isValid());
    QVERIFY(copy.recurrenceSeriesId.isEmpty()); QVERIFY(copy.occurrenceKey.isEmpty()); QCOMPARE(copy.recurrence, QStringLiteral("none"));
    QVERIFY(copy.reminders.isEmpty() && !copy.reminderAt.isValid()); QVERIFY(copy.parentId.isEmpty());
    QVERIFY(store.attachments(clone).isEmpty()); QVERIFY(store.desktopNote(clone).id.isEmpty());
    QVERIFY(!store.desktopNote(clone).open || store.desktopNote(clone).id.isEmpty());
    QVERIFY(store.cloneTask(newUuid()).isEmpty());
}

void TaskTests::multipleRemindersDeliveryRestartAndSnooze()
{
    QVERIFY(open());
    {
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString id = store.createTask(QStringLiteral("多提醒")); const QString completed = store.createTask(QStringLiteral("完成不提醒"));
    const QString deleted = store.createTask(QStringLiteral("删除不提醒"));
    for (const QString &entityId : {id, completed, deleted}) {
        Entity value = store.entity(entityId); value.reminderAt = now.addSecs(-3600); QVERIFY(store.saveTask(value));
    }
    const TaskReminder second = store.addReminder(id, now.addSecs(-7200)); QVERIFY(!second.id.isEmpty());
    QCOMPARE(store.entity(id).reminderAt, second.triggerAt);
    const QString note = newUuid(), noteReminder = newUuid();
    QVERIFY(execute(QStringLiteral("INSERT INTO entities(id,kind,title,body_markdown,list_id,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','note','保留提醒的笔记','','%2',1,1)")
                        .arg(note, store.inboxListId())));
    QSqlQuery noteReminderQuery(Database::instance().connection());
    noteReminderQuery.prepare(QStringLiteral("INSERT INTO reminders(id,entity_id,trigger_at_utc_ms,created_at_utc_ms,updated_at_utc_ms) VALUES(?,?,?,1,1)"));
    noteReminderQuery.addBindValue(noteReminder); noteReminderQuery.addBindValue(note); noteReminderQuery.addBindValue(utcMs(now.addSecs(-3600)));
    QVERIFY(noteReminderQuery.exec()); QVERIFY(store.load());
    QVERIFY(store.setCompleted(completed, true)); QVERIFY(store.trashTask(deleted));
    ReminderScheduler scheduler(&store); QSignalSpy due(&scheduler, &ReminderScheduler::reminderDue);
    QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_delivery BEFORE INSERT ON reminder_deliveries BEGIN SELECT RAISE(ABORT,'claim failure'); END")));
    scheduler.checkDue(now); QCOMPARE(due.count(), 0); QCOMPARE(store.entity(id).notifiedAt.isValid(), false);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_delivery")));
    scheduler.checkDue(now); QCOMPARE(due.count(), 3); QCOMPARE(triggered.count(), 3);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries WHERE reminder_id='%1'").arg(noteReminder)).toInt(), 1);
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QCOMPARE(store.entity(note).reminders.size(), 1);
    QCOMPARE(store.entity(note).status, TaskStatus::Active); QVERIFY(!store.entity(note).completed);
    QVERIFY(store.isReminderDelivered(store.entity(id).reminders.at(0).id)); QVERIFY(store.isReminderDelivered(second.id));
    scheduler.checkDue(now.addDays(1)); QCOMPARE(due.count(), 3);
    const QDateTime before = QDateTime::currentDateTimeUtc(); QVERIFY(scheduler.snooze(id));
    const Entity snoozed = store.entity(id); const int primary = primaryReminderIndex(snoozed);
    QVERIFY(snoozed.reminderAt >= before.addSecs(600)); QVERIFY(!store.isReminderDelivered(snoozed.reminders.at(primary).id));
    QCOMPARE(snoozed.reminders.at(primary).id, second.id);
    scheduler.checkDue(before); QCOMPARE(due.count(), 3);
    scheduler.checkDue(store.entity(id).reminderAt.addSecs(1)); QCOMPARE(due.count(), 4);
    // 每安装独立 delivery：模拟另一安装后，同一提醒可以再次投递。
    const QString otherInstallation = newUuid();
    QVERIFY(execute(QStringLiteral("UPDATE local_meta SET value='%1' WHERE key='installation_id'").arg(otherInstallation)));
    QVERIFY(store.load()); scheduler.checkDue(QDateTime::currentDateTimeUtc().addSecs(1));
    QVERIFY(due.count() >= 5);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(DISTINCT installation_id) FROM reminder_deliveries")).toInt(), 2);
    }
    Database::instance().close(); QVERIFY(open()); TaskStore restarted(Database::instance().connection()); QVERIFY(restarted.load());
    ReminderScheduler afterRestart(&restarted); QSignalSpy restartedSpy(&afterRestart, &ReminderScheduler::reminderDue);
    afterRestart.checkDue(QDateTime::currentDateTimeUtc()); QCOMPARE(restartedSpy.count(), 0);
}

void TaskTests::attachmentsShaStateAndTransactions()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("附件"));
    const QByteArray text("# 你好 KylinList\n"); const QString source = m_directory->filePath(QStringLiteral("说明.md"));
    QVERIFY(writeBytes(source, text)); const Attachment doc = store.importAttachment(id, source);
    QVERIFY2(!doc.id.isEmpty(), qPrintable(store.lastError())); QVERIFY(isUuid(doc.id)); QCOMPARE(doc.entityId, id);
    QCOMPARE(doc.sha256, AttachmentStore::sha256(text)); QCOMPARE(doc.localState, AttachmentFileState::Available);
    QCOMPARE(readBytes(attachmentPath(doc.relativePath)), text); QVERIFY(QFile::exists(source));
    QCOMPARE(store.attachmentFilePath(doc), attachmentPath(doc.relativePath));
    const QString copy = m_directory->filePath(QStringLiteral("副本.md")); QVERIFY(store.copyAttachmentTo(doc.id, copy)); QCOMPARE(readBytes(copy), text);
    const QString imageSource = m_directory->filePath(QStringLiteral("伪装.txt")); QVERIFY(writeBytes(imageSource, pngBytes()));
    const Attachment image = store.importAttachment(id, imageSource); QVERIFY(image.isImage); QCOMPARE(image.mime, QStringLiteral("image/png"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM attachments WHERE entity_id='%1'").arg(id)).toInt(), 2);
    QVERIFY(writeBytes(attachmentPath(doc.relativePath), QByteArray("tampered")));
    QVERIFY(!store.copyAttachmentTo(doc.id, copy)); QCOMPARE(store.attachments(id).at(0).localState, AttachmentFileState::Corrupt);
    QVERIFY(QFile::remove(attachmentPath(doc.relativePath))); QVERIFY(store.refreshAttachmentState(doc.id));
    QCOMPARE(store.attachments(id).at(0).localState, AttachmentFileState::Missing);
    QVERIFY(store.removeAttachment(id, doc.id)); QCOMPARE(store.attachments(id).size(), 1); QVERIFY(store.attachmentFilePath(doc).isEmpty());
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_attachment BEFORE INSERT ON attachments BEGIN SELECT RAISE(ABORT,'insert failure'); END")));
    const int filesBefore = attachmentFileCount(); QVERIFY(store.importAttachment(id, source).id.isEmpty());
    QCOMPARE(attachmentFileCount(), filesBefore); // 文件已写但 DB 回滚后必须清理
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_attachment")));
    QVERIFY(store.trashTask(id)); QVERIFY(store.deletePermanently(id)); QVERIFY(!QFile::exists(attachmentPath(image.relativePath)));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM attachments")).toInt(), 0);
}

void TaskTests::desktopNotesAreDeviceIndependent()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("桌面状态"));
    QVERIFY(store.saveDesktopState(id, NoteColor::Blue, QRect(10, 20, 300, 200), 3, NoteLayer::OnTop, true));
    QCOMPARE(store.entity(id).color, NoteColor::Blue); QCOMPARE(store.entity(id).geometry, QRect(10, 20, 300, 200));
    QVERIFY(store.desktopNote(id).open); QCOMPARE(store.entity(id).zOrder, 3); QCOMPARE(store.entity(id).layer, NoteLayer::OnTop);
    QCOMPARE(store.desktopNote(store.entities().first().id).open, true); QCOMPARE(store.entities().first().color, NoteColor::Blue);
    QCOMPARE(store.entities().first().geometry, QRect(10, 20, 300, 200));
    const qint64 revision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
    QVERIFY(store.saveDesktopState(id, store.entity(id).color, store.entity(id).geometry, store.entity(id).zOrder, store.entity(id).layer, false)); QVERIFY(!store.desktopNote(id).open); QVERIFY(!store.desktopNote(id).open);
    QVERIFY(!store.desktopNote(store.entities().first().id).open); QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong(), revision);
    QVERIFY(store.saveDesktopState(id, store.entity(id).color, store.entity(id).geometry, store.entity(id).zOrder, store.entity(id).layer, true)); QVERIFY(store.desktopNote(id).open);
    DesktopNote other; other.entityId = id; other.deviceId = newUuid(); other.color = NoteColor::Pink;
    other.geometry = QRect(900, 900, 100, 100); other.layer = NoteLayer::OnDesktop; QVERIFY(store.saveDesktopNote(other));
    QCOMPARE(store.desktopNote(id, other.deviceId).geometry, other.geometry);
    QCOMPARE(store.entity(id).geometry, QRect(10, 20, 300, 200)); // 其他设备不覆盖本机门面
    QVERIFY(store.removeDesktopNote(id, other.deviceId)); QVERIFY(store.desktopNote(id, other.deviceId).id.isEmpty());
    QVERIFY(!store.desktopNote(id).id.isEmpty());
    QVERIFY(store.removeDesktopNote(id)); QVERIFY(store.desktopNote(id).id.isEmpty());
    Entity closedPresentation = store.entity(id); closedPresentation.geometry = QRect(40, 50, 320, 240); closedPresentation.color = NoteColor::Pink;
    QVERIFY(store.saveTask(closedPresentation)); QCOMPARE(store.entity(id).geometry, closedPresentation.geometry);
    QCOMPARE(store.entity(id).color, NoteColor::Pink); QVERIFY(!store.desktopNote(id).open); QVERIFY(!store.desktopNote(id).open);
}

void TaskTests::allDayRecurrenceAndFlatBackup()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString root = store.createFolder(QStringLiteral("根")); const QString list = store.createList(QStringLiteral("清单"), root);
    const QString parent = store.createTask(QStringLiteral("全天父任务"), list);
    const QString child = store.createTask(QStringLiteral("子任务"), list, QString(), parent);
    QVERIFY(store.saveDesktopState(child, NoteColor::Yellow, QRect(), 0, NoteLayer::Normal, true)); // 即使外观全是默认值，也必须创建独立投影
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM desktop_notes WHERE entity_id='%1'").arg(child)).toInt(), 1);
    Entity task = store.entity(parent); task.allDayDate = QDate(2025, 1, 31);
    ScheduleValue schedule = scheduleFromEntity(task); schedule.timeZone = QStringLiteral("UTC");
    schedule.repeating = true; schedule.recurrence.frequency = RecurrenceSpec::Frequency::Monthly;
    schedule.recurrence.monthDays = QVector<int>{-1};
    ReminderSpec reminderSpec; reminderSpec.allDay = true; reminderSpec.dayOffset = 1; reminderSpec.localTime = QTime(9, 0);
    TaskReminder reminder; reminder.triggerAt = utc(2025, 1, 30, 9); reminder.ruleJson = reminderRuleText(reminderSpec);
    schedule.reminders.append(reminder);
    QVERIFY2(store.saveSchedule(task, schedule), qPrintable(store.lastError())); QVERIFY(store.setCompleted(parent, true));
    Entity next;
    for (const Entity &candidate : store.entities())
        if (candidate.id != parent && candidate.recurrenceSeriesId == store.entity(parent).recurrenceSeriesId) next = candidate;
    QCOMPARE(next.allDayDate, QDate(2025, 2, 28)); QCOMPARE(next.reminderAt, utc(2025, 2, 27, 9));
    QVERIFY(!next.dueAt.isValid()); const QString nextId = next.id;
    QVERIFY(execute(QStringLiteral("UPDATE recurrence_series SET server_revision=22")));
    QVERIFY(execute(QStringLiteral("UPDATE recurrence_links SET server_revision=23")));
    QVERIFY(store.load()); next = store.entity(nextId);
    const QList<QVariantList> beforeSeries = queryRows(QStringLiteral("SELECT * FROM recurrence_series ORDER BY id"));
    const QList<QVariantList> beforeEntities = queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id"));
    const QList<QVariantList> beforeLinks = queryRows(QStringLiteral("SELECT * FROM recurrence_links ORDER BY id"));
    const QList<QVariantList> beforeReminders = queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id"));
    QVERIFY(store.exportBackup(backupPath())); QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(store.folders().size(), 1); QCOMPARE(store.lists().size(), 2); QCOMPARE(store.groups().size(), 1); QCOMPARE(store.entities().size(), 3);
    QCOMPARE(store.entity(child).parentId, parent); QCOMPARE(store.entity(parent).id, parent);
    QCOMPARE(store.entity(nextId).allDayDate, QDate(2025, 2, 28));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM recurrence_series ORDER BY id")), beforeSeries);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id")), beforeEntities);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM recurrence_links ORDER BY id")), beforeLinks);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id")), beforeReminders);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM desktop_notes")).toInt(), 0); // 本机桌面投影不属于备份
}

void TaskTests::dateOnlyAndMidnightBackup_data()
{
    QTest::addColumn<QDate>("allDayDate");
    QTest::addColumn<QDateTime>("startAt");
    QTest::addColumn<QDateTime>("dueAt");
    const QDate date(2026, 11, 15);
    const QDateTime midnight = fromAppTime(QDateTime(date, QTime(0, 0)));
    QTest::newRow("date-only") << date << QDateTime() << QDateTime();
    QTest::newRow("explicit-midnight") << QDate() << QDateTime() << midnight;
}

void TaskTests::dateOnlyAndMidnightBackup()
{
    QFETCH(QDate, allDayDate);
    QFETCH(QDateTime, startAt);
    QFETCH(QDateTime, dueAt);
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("日期语义")); QVERIFY(!id.isEmpty());
    Entity task = store.entity(id);
    task.allDayDate = allDayDate; task.startAt = startAt; task.dueAt = dueAt;
    QVERIFY2(store.saveTask(task), qPrintable(store.lastError()));
    QVERIFY(store.load());
    QCOMPARE(store.entity(id).allDayDate, allDayDate);
    QCOMPARE(store.entity(id).startAt, startAt);
    QCOMPARE(store.entity(id).dueAt, dueAt);
    QCOMPARE(scalar(QStringLiteral("SELECT all_day_date IS NULL FROM entities WHERE id='%1'").arg(id)).toBool(), !allDayDate.isValid());
    QCOMPARE(scalar(QStringLiteral("SELECT start_at_utc_ms IS NULL FROM entities WHERE id='%1'").arg(id)).toBool(), !startAt.isValid());
    QCOMPARE(scalar(QStringLiteral("SELECT due_at_utc_ms IS NULL FROM entities WHERE id='%1'").arg(id)).toBool(), !dueAt.isValid());
    const QList<QVariantList> before = queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id"));
    QVERIFY2(store.exportBackup(backupPath()), qPrintable(store.lastError()));
    const QJsonObject root = readMetadata(backupPath());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6);
    const QJsonArray entities = root.value(QStringLiteral("entities")).toArray();
    QCOMPARE(entities.size(), 1);
    const QJsonObject exported = entities.first().toObject();
    QCOMPARE(exported.value(QStringLiteral("allDayDate")).isNull(), !allDayDate.isValid());
    if (allDayDate.isValid())
        QCOMPARE(exported.value(QStringLiteral("allDayDate")).toString(), allDayDate.toString(Qt::ISODate));
    QCOMPARE(exported.value(QStringLiteral("startAtUtcMs")).isNull(), !startAt.isValid());
    QCOMPARE(exported.value(QStringLiteral("dueAtUtcMs")).isNull(), !dueAt.isValid());
    if (dueAt.isValid())
        QCOMPARE(static_cast<qint64>(exported.value(QStringLiteral("dueAtUtcMs")).toDouble()), dueAt.toMSecsSinceEpoch());
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM entities ORDER BY id")), before);
    QCOMPARE(store.entity(id).allDayDate, allDayDate);
    QCOMPARE(store.entity(id).startAt, startAt);
    QCOMPARE(store.entity(id).dueAt, dueAt);
    QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), Database::schemaVersion());
}

void TaskTests::entityCodecAndBackupRoundTrip()
{
    const QDateTime created = utc(2026, 6, 1, 8);
    Entity note;
    note.id = newUuid(); note.kind = EntityKind::Note; note.title = QStringLiteral("保留任务字段的笔记");
    note.body = QStringLiteral("**完整正文**"); note.listId = newUuid(); note.sortOrder = 91; note.pinnedAt = created.addSecs(60);
    note.status = TaskStatus::Completed; note.completed = true; note.completedAt = created.addDays(1);
    note.startAt = created; note.dueAt = created.addDays(2); note.hasScheduleRange = true; note.priority = 3;
    note.createdAt = created; note.updatedAt = created.addDays(1);
    note.recurrenceSeriesId = newUuid(); note.occurrenceKey = QStringLiteral("2026-06-03");
    note.rrule = QStringLiteral("FREQ=DAILY;INTERVAL=1"); note.recurrence = QStringLiteral("daily");
    TaskReminder reminder; reminder.id = newUuid(); reminder.entityId = note.id; reminder.triggerAt = note.dueAt.addSecs(-600);
    reminder.createdAt = created; reminder.updatedAt = created; note.reminders.append(reminder); note.reminderAt = reminder.triggerAt;
    QString error; QVERIFY2(entitycodec::validate(note, &error), qPrintable(error));
    const QJsonObject json = entitycodec::toJson(note);
    QCOMPARE(json.value(QStringLiteral("bodyMarkdown")).toString(), note.body);
    QCOMPARE(json.value(QStringLiteral("kind")).toString(), QStringLiteral("note"));
    QCOMPARE(json.value(QStringLiteral("sortOrder")).toInt(), 91);
    QVERIFY(json.value(QStringLiteral("pinnedAtUtcMs")).isDouble()); QVERIFY(!json.contains(QStringLiteral("description")));
    Entity decoded; QVERIFY2(entitycodec::fromJson(json, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.kind, EntityKind::Note); QCOMPARE(decoded.body, note.body); QCOMPARE(decoded.status, TaskStatus::Completed);
    QCOMPARE(decoded.startAt, note.startAt); QCOMPARE(decoded.dueAt, note.dueAt); QCOMPARE(decoded.priority, 3);
    QCOMPARE(decoded.recurrenceSeriesId, note.recurrenceSeriesId); QCOMPARE(decoded.occurrenceKey, note.occurrenceKey);
    QCOMPARE(decoded.sortOrder, qint64(91)); QCOMPARE(decoded.pinnedAt, note.pinnedAt);
    QCOMPARE(decoded.reminders.first().entityId, note.id);
    Entity invalid = note; invalid.parentId = note.id; QVERIFY(!entitycodec::validate(invalid, &error));
    invalid = note; invalid.kind = EntityKind::Task; invalid.completed = false; QVERIFY(!entitycodec::validate(invalid, &error));

    EntitySnapshot snapshot;
    Folder homeFolder; homeFolder.id = newUuid(); homeFolder.name = QStringLiteral("家庭");
    homeFolder.icon = QStringLiteral("🏡"); homeFolder.createdAt = created; homeFolder.updatedAt = created;
    snapshot.folders.append(homeFolder);
    TaskList inbox; inbox.id = note.listId; inbox.name = QStringLiteral("收集箱"); inbox.inbox = true;
    inbox.icon = QStringLiteral("👨‍👩‍👧‍👦"); inbox.color = QStringLiteral("#FF8800");
    inbox.createdAt = created; inbox.updatedAt = created; snapshot.lists.append(inbox);
    RecurrenceSpec recurrenceSpec; recurrenceSpec.frequency = RecurrenceSpec::Frequency::Daily;
    RecurrenceSeries series; series.id = note.recurrenceSeriesId; series.rrule = note.rrule; series.timeZone = note.timeZone;
    series.ruleJson = recurrenceRuleText(recurrenceSpec); series.occurrenceOrdinal = 4; series.completionCount = 2;
    series.dtstartAt = note.startAt; series.createdAt = created; series.updatedAt = created; snapshot.recurrenceSeries.append(series);
    StoredEntity stored; stored.entity = note; stored.revision = 7; snapshot.entities.append(stored);
    snapshot.localMeta.append(LocalMeta{QStringLiteral("default_timezone"), QStringLiteral("UTC")});
    EntityEvent first; first.id = newUuid(); first.entityId = note.id; first.entityKind = EntityKind::Note;
    first.entityTitle = note.title; first.batchId = newUuid(); first.type = QStringLiteral("changed"); first.field = QStringLiteral("body");
    first.newValue = note.body; first.occurredAt = created.addSecs(120);
    EntityEvent second; second.id = newUuid(); second.entityId = newUuid(); second.entityKind = EntityKind::Task;
    second.entityTitle = QStringLiteral("已删除实体"); second.batchId = newUuid(); second.type = QStringLiteral("deleted");
    second.oldValue = QStringLiteral("before"); second.newValue = QStringLiteral(""); second.occurredAt = created.addSecs(180);
    const QVector<EntityEvent> events{first, second};
    const QJsonObject root = backupcodec::toJson(snapshot, events);
    QCOMPARE(root.value(QStringLiteral("format")).toString(), QStringLiteral("KylinList.Entities"));
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6);
    QCOMPARE(root.value(QStringLiteral("defaultTimezone")).toString(), QStringLiteral("UTC"));
    QCOMPARE(root.value(QStringLiteral("entities")).toArray().size(), 1);
    QCOMPARE(root.value(QStringLiteral("events")).toArray().size(), 2);
    QVERIFY(!root.contains(QStringLiteral("tasks"))); QVERIFY(!root.contains(QStringLiteral("localMeta"))); QVERIFY(!root.contains(QStringLiteral("workspaces")));
    {
        const QJsonObject exportedList = root.value(QStringLiteral("lists")).toArray().first().toObject();
        QCOMPARE(exportedList.value(QStringLiteral("icon")).toString(), QStringLiteral("👨‍👩‍👧‍👦"));
        QCOMPARE(exportedList.value(QStringLiteral("color")).toString(), QStringLiteral("#FF8800"));
        QCOMPARE(exportedList.value(QStringLiteral("archivedAtUtcMs")).toInt(), 0);
        const QJsonObject exportedFolder = root.value(QStringLiteral("folders")).toArray().first().toObject();
        QCOMPARE(exportedFolder.value(QStringLiteral("icon")).toString(), QStringLiteral("🏡"));   // v4：文件夹图标
    }
    EntitySnapshot restored; QVector<EntityEvent> restoredEvents; QString restoredTimezone;
    QVERIFY2(backupcodec::fromJson(root, &restored, &restoredEvents, &restoredTimezone, &error), qPrintable(error));
    QCOMPARE(restoredTimezone, QStringLiteral("UTC")); QCOMPARE(restored.entities.size(), 1); QCOMPARE(restored.localMeta.size(), 0);
    QCOMPARE(restored.folders.size(), 1); QCOMPARE(restored.folders.first().icon, QStringLiteral("🏡"));
    QCOMPARE(restored.folders.first().name, QStringLiteral("家庭"));
    QCOMPARE(restored.lists.first().icon, QStringLiteral("👨‍👩‍👧‍👦"));
    QCOMPARE(restored.lists.first().color, QStringLiteral("#FF8800"));
    QCOMPARE(restored.lists.first().archivedAtUtcMs, qint64(0));
    QCOMPARE(restored.entities.first().entity.kind, EntityKind::Note); QCOMPARE(restored.entities.first().entity.body, note.body);
    QCOMPARE(restored.recurrenceSeries.first().ruleJson, series.ruleJson);
    QCOMPARE(restored.recurrenceSeries.first().occurrenceOrdinal, 4);
    QCOMPARE(restored.recurrenceSeries.first().completionCount, 2);
    QVERIFY(restored.entities.first().entity.hasScheduleRange);
    QCOMPARE(restoredEvents.size(), 2); QVERIFY(!restoredEvents.first().oldValue.isValid());
    QCOMPARE(restoredEvents.first().newValue.toString(), note.body);
    QCOMPARE(restoredEvents.at(1).oldValue.toString(), QStringLiteral("before"));
    QVERIFY(restoredEvents.at(1).newValue.isValid()); QCOMPARE(restoredEvents.at(1).newValue.toString(), QString());
    QJsonObject old = root; old.insert(QStringLiteral("format"), QStringLiteral("KylinList.Tasks")); old.insert(QStringLiteral("version"), 1);
    EntitySnapshot rejected; QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; old.remove(QStringLiteral("events")); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; old.remove(QStringLiteral("defaultTimezone")); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; old.insert(QStringLiteral("checklistItems"), QJsonArray()); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; old.insert(QStringLiteral("reminders"), QJsonArray()); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; QJsonArray invalidSeries = old.value(QStringLiteral("recurrenceSeries")).toArray();
    QJsonObject seriesObject = invalidSeries.first().toObject(); seriesObject.insert(QStringLiteral("rrule"), QStringLiteral("FREQ=DAILY;COUNT=2"));
    invalidSeries.replace(0, seriesObject); old.insert(QStringLiteral("recurrenceSeries"), invalidSeries);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; QJsonArray duplicateEvents = old.value(QStringLiteral("events")).toArray(); duplicateEvents.append(duplicateEvents.first());
    old.insert(QStringLiteral("events"), duplicateEvents); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; QJsonArray duplicateInbox = old.value(QStringLiteral("lists")).toArray();
    QJsonObject secondInbox = duplicateInbox.first().toObject(); secondInbox.insert(QStringLiteral("id"), newUuid()); duplicateInbox.append(secondInbox);
    old.insert(QStringLiteral("lists"), duplicateInbox); QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; QJsonArray invalidEntities = old.value(QStringLiteral("entities")).toArray();
    QJsonObject invalidReference = invalidEntities.first().toObject(); invalidReference.insert(QStringLiteral("listId"), newUuid());
    invalidEntities.replace(0, invalidReference); old.insert(QStringLiteral("entities"), invalidEntities);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; invalidEntities = old.value(QStringLiteral("entities")).toArray(); invalidReference = invalidEntities.first().toObject();
    invalidReference.remove(QStringLiteral("serverRevision")); invalidEntities.replace(0, invalidReference); old.insert(QStringLiteral("entities"), invalidEntities);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));

    // 只读当前版本；过去版本与未来版本都不得进入补字段或转换逻辑。
    for (int version : {2, 3, 4, 5, backupcodec::kVersion + 1}) {
        old = root; old.insert(QStringLiteral("version"), version);
        QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
        QVERIFY2(error.contains(QStringLiteral("v%1").arg(backupcodec::kVersion)), qPrintable(error));
    }
    // 当前格式的非法外观同样拒绝。
    old = root; QJsonArray badFolders = old.value(QStringLiteral("folders")).toArray();
    QJsonObject badFolder = badFolders.first().toObject(); badFolder.insert(QStringLiteral("icon"), QString(17, QChar(0x4E2D)));
    badFolders.replace(0, badFolder); old.insert(QStringLiteral("folders"), badFolders);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; badFolders = old.value(QStringLiteral("folders")).toArray();
    badFolder = badFolders.first().toObject(); badFolder.insert(QStringLiteral("icon"), 7);
    badFolders.replace(0, badFolder); old.insert(QStringLiteral("folders"), badFolders);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; QJsonArray badLists = old.value(QStringLiteral("lists")).toArray();
    QJsonObject badList = badLists.first().toObject(); badList.insert(QStringLiteral("color"), QStringLiteral("#ZZZZZZ"));
    badLists.replace(0, badList); old.insert(QStringLiteral("lists"), badLists);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; badLists = old.value(QStringLiteral("lists")).toArray();
    badList = badLists.first().toObject(); badList.insert(QStringLiteral("icon"), QString(17, QChar(0x4E2D)));
    badLists.replace(0, badList); old.insert(QStringLiteral("lists"), badLists);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
    old = root; badLists = old.value(QStringLiteral("lists")).toArray();
    badList = badLists.first().toObject(); badList.insert(QStringLiteral("archivedAtUtcMs"), -1);
    badLists.replace(0, badList); old.insert(QStringLiteral("lists"), badLists);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
}

void TaskTests::listAppearanceArchiveAndAtomicUpdate()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folderA = store.createFolder(QStringLiteral("文件夹A"));
    const QString folderB = store.createFolder(QStringLiteral("文件夹B"));
    const QString list = store.createList(QStringLiteral("  装修  "), folderA);
    QVERIFY(isUuid(list));
    const QString family = QStringLiteral("👨‍👩‍👧‍👦");   // 4 emoji + 3 ZWJ = 11 个 UTF-16 码元
    const qint64 revisionBefore = scalar(QStringLiteral("SELECT revision FROM lists WHERE id='%1'").arg(list)).toLongLong();
    QVERIFY2(store.updateList(list, QStringLiteral("  装修 "), family, QStringLiteral("#fF8800"), folderB), qPrintable(store.lastError()));
    TaskList updated;
    for (const TaskList &candidate : store.lists()) if (candidate.id == list) updated = candidate;
    QCOMPARE(updated.name, QStringLiteral("装修"));
    QCOMPARE(updated.icon, family);
    QCOMPARE(updated.color, QStringLiteral("#fF8800"));
    QCOMPARE(updated.folderId, folderB);
    QCOMPARE(updated.kind, EntityKind::Task); QVERIFY(!updated.inbox); QCOMPARE(updated.archivedAtUtcMs, qint64(0));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM lists WHERE id='%1'").arg(list)).toLongLong(), revisionBefore + 1); // 单事务单次更新
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(), QStringLiteral("red"), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(), QStringLiteral("#12345"), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(), QStringLiteral("#1234567"), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(), QStringLiteral("#12345W"), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(17, QChar(0x4E2D)), QString(), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("  "), QString(), QString(), folderB));
    QVERIFY(!store.updateList(list, QString(65, QChar(0x540D)), QString(), QString(), folderB));
    QVERIFY(!store.updateList(list, QStringLiteral("x"), QString(), QString(), newUuid()));
    QVERIFY(!store.updateList(newUuid(), QStringLiteral("x"), QString(), QString(), QString()));
    QVERIFY(!store.updateList(store.inboxListId(), QStringLiteral("改名"), QString(), QString(), folderA)); // 收集箱不可移动/改归档外观之外的语义
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM lists WHERE id='%1'").arg(list)).toLongLong(), revisionBefore + 1); // 失败不推进
    QVERIFY2(store.updateList(list, QStringLiteral("装修"), QStringLiteral("🏡"), QStringLiteral("#A1b2C3"), QString()), qPrintable(store.lastError()));

    // saveList 门面：kind/inbox/归档状态以库内现值为准，调用方给什么都改不掉。
    TaskList facade;
    for (const TaskList &candidate : store.lists()) if (candidate.id == list) facade = candidate;
    facade.kind = EntityKind::Note; facade.inbox = true; facade.archivedAtUtcMs = 12345;
    facade.name = QStringLiteral("装修 v2"); facade.icon = family; facade.color = QStringLiteral("#00AA55");
    QVERIFY2(store.saveList(facade), qPrintable(store.lastError()));
    TaskList saved;
    for (const TaskList &candidate : store.lists()) if (candidate.id == list) saved = candidate;
    QCOMPARE(saved.kind, EntityKind::Task); QVERIFY(!saved.inbox); QCOMPARE(saved.archivedAtUtcMs, qint64(0));
    QCOMPARE(saved.name, QStringLiteral("装修 v2")); QCOMPARE(saved.icon, family); QCOMPARE(saved.color, QStringLiteral("#00AA55"));
    QVERIFY(saved.folderId.isEmpty());

    QVERIFY(!store.setListArchived(store.inboxListId(), true));   // 收集箱禁止归档
    const QString task = store.createTask(QStringLiteral("归档清单任务"), list);
    Entity before = store.entity(task);
    before.dueAt = utc(2026, 12, 1, 9); before.reminderAt = utc(2026, 12, 1, 8);
    QVERIFY(store.saveTask(before));
    before = store.entity(task);
    const int entitiesBefore = store.entities().size();
    QVERIFY2(store.setListArchived(list, true), qPrintable(store.lastError()));
    TaskList archived;
    for (const TaskList &candidate : store.lists()) if (candidate.id == list) archived = candidate;
    QVERIFY(archived.archivedAtUtcMs > 0);
    QVERIFY(store.isListArchived(list));
    QCOMPARE(store.archivedListIds(), QSet<QString>{list});
    const Entity archivedTask = store.entity(task);                 // 归档只改清单，不动任务
    QCOMPARE(archivedTask.status, TaskStatus::Active); QVERIFY(!archivedTask.deleted);
    QCOMPARE(archivedTask.dueAt, before.dueAt);
    QCOMPARE(archivedTask.reminders.size(), before.reminders.size());
    QCOMPARE(archivedTask.reminderAt, before.reminderAt);
    QCOMPARE(store.entities().size(), entitiesBefore);              // 不生成重复实例
    QVERIFY(store.setListArchived(list, true));                     // 幂等
    for (const TaskList &candidate : store.lists()) if (candidate.id == list)
        QCOMPARE(candidate.archivedAtUtcMs, archived.archivedAtUtcMs);
    QVERIFY(store.setListArchived(list, false));
    for (const TaskList &candidate : store.lists()) if (candidate.id == list)
        QCOMPARE(candidate.archivedAtUtcMs, qint64(0));
    QVERIFY(!store.isListArchived(list)); QVERIFY(store.archivedListIds().isEmpty());
    QVERIFY(store.setListArchived(newUuid(), true) == false);       // 不存在的清单
    // 创建时外观随同一事务落库，无需 create 后二次保存。
    const QString created = store.createList(QStringLiteral("  新家  "), folderA, EntityKind::Task,
                                             QStringLiteral("🏡"), QStringLiteral("#00CCAA"));
    QVERIFY2(isUuid(created), qPrintable(store.lastError()));
    TaskList createdRow;
    for (const TaskList &candidate : store.lists()) if (candidate.id == created) createdRow = candidate;
    QCOMPARE(createdRow.name, QStringLiteral("新家"));
    QCOMPARE(createdRow.icon, QStringLiteral("🏡"));
    QCOMPARE(createdRow.color, QStringLiteral("#00CCAA"));
    QCOMPARE(createdRow.folderId, folderA);
    QCOMPARE(scalar(QStringLiteral("SELECT icon FROM lists WHERE id='%1'").arg(created)).toString(), QStringLiteral("🏡"));
    QCOMPARE(scalar(QStringLiteral("SELECT color FROM lists WHERE id='%1'").arg(created)).toString(), QStringLiteral("#00CCAA"));
    QCOMPARE(scalar(QStringLiteral("SELECT archived_at_utc_ms FROM lists WHERE id='%1'").arg(created)).toLongLong(), qint64(0));
    QVERIFY(store.createList(QStringLiteral("坏颜色"), QString(), EntityKind::Task, QString(), QStringLiteral("red")).isEmpty());
    QVERIFY(store.createList(QStringLiteral("坏图标"), QString(), EntityKind::Task, QString(17, QChar(0x56FE))).isEmpty());
}

void TaskTests::listUpdatesRejectStaleRevisions()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString list = store.createList(QStringLiteral("原清单"));
    const QString other = store.createList(QStringLiteral("同批清单"));
    QVERIFY(isUuid(list)); QVERIFY(isUuid(other));
    const auto currentList = [&store](const QString &id) {
        for (const TaskList &row : store.lists()) if (row.id == id) return row;
        return TaskList();
    };
    TaskList staleDraft = currentList(list);
    QVERIFY(store.updateList(list, QStringLiteral("已更新名称"), QStringLiteral("🏡"), QStringLiteral("#123456"), QString()));
    const QString rowsSql = QStringLiteral("SELECT * FROM lists ORDER BY id");
    const auto beforeDraft = queryRows(rowsSql);
    staleDraft.color = QStringLiteral("#ABCDEF");
    QVERIFY(!store.saveList(staleDraft));
    QVERIFY(store.lastError().contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(rowsSql), beforeDraft);

    TaskStore staleStore(Database::instance().connection()); QVERIFY(staleStore.load());
    const TaskList beforeArchive = currentList(list);
    QVERIFY(store.setListArchived(list, true));
    const auto archivedRows = queryRows(rowsSql);
    QVERIFY(!store.saveList(beforeArchive));                      // 未改外观的旧草稿同样必须拒绝
    QVERIFY(!staleStore.updateList(list, QStringLiteral("陈旧覆盖"), QString(), QStringLiteral("#ABCDEF"), QString()));
    QVERIFY(staleStore.lastError().contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(rowsSql), archivedRows);                    // 旧 store 不得撤销归档

    TaskRepository repository(Database::instance().connection());
    EntityChanges changes; EntitySnapshot after; QString error;
    TaskList valid = currentList(other); valid.name = QStringLiteral("应回滚的同批修改");
    TaskList invalid = beforeArchive; invalid.color = QStringLiteral("#ABCDEF");
    changes.groupSaves.append(valid); changes.groupSaves.append(invalid);
    QVERIFY(!repository.apply(&changes, &after, &error));           // 绕过门面仍由 repository CAS 拒绝
    QVERIFY(error.contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(rowsSql), archivedRows);                    // 同批先写入的清单也整体回滚

    QVERIFY(staleStore.load()); QVERIFY(staleStore.isListArchived(list));
    QVERIFY(staleStore.updateList(list, QStringLiteral("重新加载后修改"), QStringLiteral("🏡"), QStringLiteral("#ABCDEF"), QString()));
    QVERIFY(staleStore.isListArchived(list));
    const auto updatedRows = queryRows(rowsSql);
    QVERIFY(!store.setListArchived(list, false));                  // 归档入口也不能使用陈旧 revision
    QCOMPARE(queryRows(rowsSql), updatedRows);
    QVERIFY(store.load()); QVERIFY(store.setListArchived(list, false));
    QVERIFY(!store.isListArchived(list));
    QCOMPARE(currentList(list).name, QStringLiteral("重新加载后修改"));
    QCOMPARE(currentList(list).color, QStringLiteral("#ABCDEF"));
}

// 文件夹完整编辑入口：icon 校验、name trim 1..64、快照 revision 与数据库 CAS 双层保护。
void TaskTests::folderIconEditAndStaleRevisionGuard()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral(" 旅行 "), QStringLiteral("🧳"));
    QVERIFY2(isUuid(folder), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM folders WHERE id='%1'").arg(folder)).toString(), QStringLiteral("旅行"));
    QCOMPARE(scalar(QStringLiteral("SELECT icon FROM folders WHERE id='%1'").arg(folder)).toString(), QStringLiteral("🧳"));
    QVERIFY(store.createFolder(QStringLiteral("")).isEmpty());                       // 名称 trim 后为空
    QVERIFY(store.createFolder(QString(65, QChar(0x540D))).isEmpty());               // 名称超长
    QVERIFY(store.createFolder(QStringLiteral("坏图标"), QString(17, QChar(0x56FE))).isEmpty());
    QVERIFY(store.createFolder(QStringLiteral("控制字符"), QStringLiteral("a\tb")).isEmpty());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM folders")).toInt(), 1);     // 失败不落半成品

    const auto currentFolder = [&store, &folder]() {
        for (const Folder &row : store.folders()) if (row.id == folder) return row;
        return Folder();
    };
    Folder edited = currentFolder();
    edited.name = QStringLiteral("  家庭旅行  "); edited.icon = QStringLiteral("🏡");
    const qint64 revisionBefore = scalar(QStringLiteral("SELECT revision FROM folders WHERE id='%1'").arg(folder)).toLongLong();
    QVERIFY2(store.saveFolder(edited), qPrintable(store.lastError()));
    Folder saved = currentFolder();
    QCOMPARE(saved.name, QStringLiteral("家庭旅行")); QCOMPARE(saved.icon, QStringLiteral("🏡"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM folders WHERE id='%1'").arg(folder)).toLongLong(), revisionBefore + 1);
    QVERIFY(store.saveFolder(saved));                                                // 幂等：无变化不再推进
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM folders WHERE id='%1'").arg(folder)).toLongLong(), revisionBefore + 1);
    Folder invalid = saved; invalid.icon = QString(17, QChar(0x56FE));
    QVERIFY(!store.saveFolder(invalid));
    invalid = saved; invalid.name = QStringLiteral("  ");
    QVERIFY(!store.saveFolder(invalid));
    invalid = saved; invalid.name = QString(65, QChar(0x65B0));
    QVERIFY(!store.saveFolder(invalid));

    // 陈旧草稿：快照 revision 不一致直接拒绝；绕过门面由 repository CAS 拒绝并整体回滚。
    Folder stale = saved;
    Folder committed = saved; committed.name = QStringLiteral("已推进"); committed.icon = QStringLiteral("🗺");
    QVERIFY(store.saveFolder(committed));
    const auto folderRows = queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id"));
    QVERIFY(!store.saveFolder(stale));
    QVERIFY(store.lastError().contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), folderRows);
    QVERIFY2(store.renameFolder(folder, QStringLiteral("  新名字  ")), qPrintable(store.lastError()));
    QCOMPARE(currentFolder().name, QStringLiteral("新名字"));
    QCOMPARE(currentFolder().icon, QStringLiteral("🗺"));                            // 未提供的字段保持不变

    TaskRepository repository(Database::instance().connection());
    EntityChanges changes; EntitySnapshot after; QString error;
    Folder bypass = currentFolder(); bypass.name = QStringLiteral("应回滚");
    changes.folderSaves.append(bypass);
    Folder staleRow = bypass; staleRow.revision = currentFolder().revision - 1; staleRow.name = QStringLiteral("陈旧CAS");
    changes.folderSaves.append(staleRow);
    const auto renamedRows = queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id"));
    QVERIFY(!repository.apply(&changes, &after, &error));                             // 同批先写的也回滚
    QVERIFY(error.contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), renamedRows);

    // 重载后可继续编辑，图标持久化。
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
    Folder reloadedFolder;
    for (const Folder &row : reloaded.folders()) if (row.id == folder) reloadedFolder = row;
    QCOMPARE(reloadedFolder.name, QStringLiteral("新名字")); QCOMPARE(reloadedFolder.icon, QStringLiteral("🗺"));
    reloadedFolder.icon = QString();
    QVERIFY(reloaded.saveFolder(reloadedFolder));
    QCOMPARE(scalar(QStringLiteral("SELECT icon FROM folders WHERE id='%1'").arg(folder)).toString(), QString());
}

// createList/createFolder 落在所在容器尾部；顶层清单与文件夹共用排序序列。
void TaskTests::listAndFolderTailSortOrder()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folderA = store.createFolder(QStringLiteral("文件夹A"), QStringLiteral("📁"));
    QVERIFY(isUuid(folderA));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(folderA)).toInt(), 1);  // inbox 占 0
    const QString listA = store.createList(QStringLiteral("清单A"));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(listA)).toInt(), 2);       // 顶层共用序列尾部
    const QString folderB = store.createFolder(QStringLiteral("文件夹B"));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(folderB)).toInt(), 3);
    const QString inFolder = store.createList(QStringLiteral("文件夹内清单"), folderA);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(inFolder)).toInt(), 0);   // 文件夹容器独立从 0 开始
    const QString inFolderSecond = store.createList(QStringLiteral("第二"), folderA);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(inFolderSecond)).toInt(), 1);
    // 手工把顶层序列拉高后，新清单/文件夹仍落在共用尾部。
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=40 WHERE id='%1'").arg(listA)));
    QVERIFY(store.load());
    const QString tailList = store.createList(QStringLiteral("尾部清单"));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(tailList)).toInt(), 41);
    const QString tailFolder = store.createFolder(QStringLiteral("尾部文件夹"));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(tailFolder)).toInt(), 42);   // 共用序列再往后
}

// moveList：同容器重排、跨容器移动、目标空=末尾、非法输入零写入、容器规范化、持久化重载。
void TaskTests::listOrganizationMoveCommands()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("工作"));
    const QString a = store.createList(QStringLiteral("A"));
    const QString b = store.createList(QStringLiteral("B"));
    const QString c = store.createList(QStringLiteral("C"));
    const QString inner1 = store.createList(QStringLiteral("内一"), folder);
    const QString inner2 = store.createList(QStringLiteral("内二"), folder);
    QVERIFY(isUuid(folder) && isUuid(a) && isUuid(b) && isUuid(c) && isUuid(inner1) && isUuid(inner2));
    const auto topLevelOrder = [&store]() {
        QStringList ids;
        QVector<TaskList> rows;
        for (const TaskList &row : store.lists()) if (row.folderId.isEmpty()) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &x, const TaskList &y) {
            return x.sortOrder != y.sortOrder ? x.sortOrder < y.sortOrder : x.id < y.id;
        });
        for (const TaskList &row : rows) ids.append(row.name);
        return ids.join(QStringLiteral(","));
    };
    const auto folderOrder = [&store, &folder]() {
        QStringList ids;
        QVector<TaskList> rows;
        for (const TaskList &row : store.lists()) if (row.folderId == folder) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &x, const TaskList &y) {
            return x.sortOrder != y.sortOrder ? x.sortOrder < y.sortOrder : x.id < y.id;
        });
        for (const TaskList &row : rows) ids.append(row.name);
        return ids.join(QStringLiteral(","));
    };
    QCOMPARE(folderOrder(), QStringLiteral("内一,内二"));

    // 同容器重排：把 C 移到 A 之前；顶层混合序列（平铺清单+文件夹）规范为 0..n-1。
    QVERIFY2(store.moveList(c, QString(), a, false), qPrintable(store.lastError()));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,C,A,B"));
    QList<QVariantList> mergedRows = queryRows(QStringLiteral(
        "SELECT sort_order FROM (SELECT sort_order FROM lists WHERE folder_id IS NULL "
        "UNION ALL SELECT sort_order FROM folders) ORDER BY sort_order"));
    QCOMPARE(mergedRows.size(), 5);
    for (int i = 0; i < mergedRows.size(); ++i) QCOMPARE(mergedRows.at(i).first().toInt(), i);   // 混合序列非负连续
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(folder)).toInt(), 1);   // 文件夹仍在 inbox 与 C 之间
    // after=true 放到锚点之后。
    QVERIFY(store.moveList(c, QString(), a, true));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,A,C,B"));
    // 目标空 = 容器末尾。
    QVERIFY(store.moveList(a, QString()));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,C,B,A"));
    // 无位移的移动是幂等成功，不产生写入。
    const qint64 aRevision = scalar(QStringLiteral("SELECT revision FROM lists WHERE id='%1'").arg(a)).toLongLong();
    QVERIFY(store.moveList(a, QString()));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM lists WHERE id='%1'").arg(a)).toLongLong(), aRevision);

    // 跨容器：顶层 B 移入文件夹内一之前；源容器（顶层）同步规范化。
    QVERIFY2(store.moveList(b, folder, inner1, false), qPrintable(store.lastError()));
    QCOMPARE(folderOrder(), QStringLiteral("B,内一,内二"));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,C,A"));
    // 文件夹移回平铺末尾。
    QVERIFY(store.moveList(inner2, QString()));
    QCOMPARE(folderOrder(), QStringLiteral("B,内一"));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,C,A,内二"));
    // 文件夹内重排到末尾。
    QVERIFY(store.moveList(b, folder));
    QCOMPARE(folderOrder(), QStringLiteral("内一,B"));

    // 非法输入零写入（先备好归档清单再快照，保证对照完整）。
    const QString archived = store.createList(QStringLiteral("归档"));
    QVERIFY(store.setListArchived(archived, true));
    const auto listsSnapshot = queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id"));
    const auto foldersSnapshot = queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id"));
    QVERIFY(!store.moveList(store.inboxListId(), folder));                    // inbox 不可移动
    QVERIFY(!store.moveList(newUuid(), folder));                              // 不存在
    QVERIFY(!store.moveList(a, newUuid()));                                   // 目标文件夹不存在
    QVERIFY(!store.moveList(a, folder, newUuid()));                           // 锚点不存在
    QVERIFY(!store.moveList(a, folder, a));                                   // 相对自身
    QVERIFY(!store.moveList(a, folder, c));                                   // 锚点在顶层而目标为文件夹
    QVERIFY(!store.moveList(a, QString(), inner1));                           // 锚点属于文件夹而目标为平铺
    QVERIFY(!store.moveList(a, QString(), store.inboxListId()));              // inbox 不可作锚点
    QVERIFY(!store.moveList(archived, folder));                               // 已归档不可移动
    QVERIFY(!store.moveList(a, folder, archived));                            // 已归档不可作锚点
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id")), listsSnapshot);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), foldersSnapshot);

    // 持久化重载后顺序保持。
    QVERIFY(store.load());
    QCOMPARE(folderOrder(), QStringLiteral("内一,B"));
    QCOMPARE(topLevelOrder(), QStringLiteral("收集箱,C,A,内二,归档"));
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
    QStringList reloadedTop;
    {
        QVector<TaskList> rows;
        for (const TaskList &row : reloaded.lists()) if (row.folderId.isEmpty()) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &x, const TaskList &y) {
            return x.sortOrder != y.sortOrder ? x.sortOrder < y.sortOrder : x.id < y.id;
        });
        for (const TaskList &row : rows) reloadedTop.append(row.name);
    }
    QCOMPARE(reloadedTop.join(QStringLiteral(",")), QStringLiteral("收集箱,C,A,内二,归档"));
}

// createFolderFromLists：合法路径原子建夹并移入；非法源/目标无半成品。
void TaskTests::folderFromListsAtomicCommand()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString other = store.createFolder(QStringLiteral("既有文件夹"));
    const QString top1 = store.createList(QStringLiteral("顶层一"));
    const QString top2 = store.createList(QStringLiteral("顶层二"));
    const QString top3 = store.createList(QStringLiteral("顶层三"));
    const QString inFolder = store.createList(QStringLiteral("夹内清单"), other);
    QVERIFY(isUuid(other) && isUuid(top1) && isUuid(top2) && isUuid(top3) && isUuid(inFolder));

    // 非法输入：无文件夹、无移动（先备好归档清单再快照，保证对照完整）。
    const QString archived = store.createList(QStringLiteral("归档源"));
    QVERIFY(store.setListArchived(archived, true));
    const auto listsSnapshot = queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id"));
    const auto foldersSnapshot = queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id"));
    QVERIFY(store.createFolderFromLists(QStringLiteral(""), QString(), top1, top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QString(65, QChar(0x540D)), QString(), top1, top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("坏图标"), QString(17, QChar(0x56FE)), top1, top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("源不存在"), QString(), newUuid(), top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("目标不存在"), QString(), top1, newUuid()).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("源是inbox"), QString(), store.inboxListId(), top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("目标是inbox"), QString(), top1, store.inboxListId()).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("目标在文件夹"), QString(), top1, inFolder).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("同名"), QString(), top1, top1).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("归档源"), QString(), archived, top2).isEmpty());
    QVERIFY(store.createFolderFromLists(QStringLiteral("归档目标"), QString(), top1, archived).isEmpty());
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id")), listsSnapshot);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), foldersSnapshot);

    // 合法：target 平铺、source 在其他文件夹；folder 占 target 原顶层位置，两清单保持相对顺序。
    const int targetOrder = scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(top2)).toInt();
    const QString created = store.createFolderFromLists(QStringLiteral("  新家庭  "), QStringLiteral("🏡"), inFolder, top2);
    QVERIFY2(isUuid(created), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM folders WHERE id='%1'").arg(created)).toString(), QStringLiteral("新家庭"));
    QCOMPARE(scalar(QStringLiteral("SELECT icon FROM folders WHERE id='%1'").arg(created)).toString(), QStringLiteral("🏡"));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(created)).toInt(), targetOrder);
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(top2)).toString(), created);
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(inFolder)).toString(), created);
    // 源（夹内 0）在前、目标（顶层 3）在后：保持相对顺序进入新文件夹。
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(inFolder)).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(top2)).toInt(), 1);
    // 顶层其余清单原位不动（folder 恰好占住 top2 的旧位置）。
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(top1)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(top3)).toInt(), 4);
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(top1)).toString(), QString());
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(top3)).toString(), QString());
    // 源容器（other 文件夹）清空。
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM lists WHERE folder_id='%1'").arg(other)).toInt(), 0);

    // 顶层平铺 source 同样合法。
    const QString created2 = store.createFolderFromLists(QStringLiteral("第二夹"), QString(), top3, top1);
    QVERIFY2(isUuid(created2), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(top1)).toString(), created2);
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(top3)).toString(), created2);

    // 持久化重载保持。
    QVERIFY(store.load());
    bool found = false;
    for (const Folder &row : store.folders()) if (row.id == created) found = row.name == QStringLiteral("新家庭") && row.icon == QStringLiteral("🏡");
    QVERIFY(found);
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
    int folderLists = 0;
    for (const TaskList &row : reloaded.lists()) if (row.folderId == created) ++folderLists;
    QCOMPARE(folderLists, 2);
}

// 当前 schema 允许排序值相同：同序号时平铺清单在前、文件夹在后，各自按 id 排序。
// moveList 与 createFolderFromLists 必须使用相同的稳定顺序计算位置。
void TaskTests::allZeroTopLevelOrderIsDeterministic()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString fWork = store.createFolder(QStringLiteral("工作"));
    const QString fHome = store.createFolder(QStringLiteral("家"));
    const QString lA = store.createList(QStringLiteral("A"));
    const QString lB = store.createList(QStringLiteral("B"));
    const QString lC = store.createList(QStringLiteral("C"));
    const QString lD = store.createList(QStringLiteral("D"));
    QVERIFY(isUuid(fWork) && isUuid(fHome) && isUuid(lA) && isUuid(lB) && isUuid(lC) && isUuid(lD));
    // 当前库的合法并列顺序夹具：所有顶层项都使用默认值 0。
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=0 WHERE folder_id IS NULL")));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=0")));
    QVERIFY(store.load());
    const auto mergedNames = []() {
        QStringList names;
        const QList<QVariantList> rows = queryRows(QStringLiteral(
            "SELECT name FROM (SELECT sort_order,0 AS k,id,name FROM lists WHERE folder_id IS NULL "
            "UNION ALL SELECT sort_order,1 AS k,id,name FROM folders) ORDER BY sort_order,k,id"));
        for (const QVariantList &row : rows) names.append(row.first().toString());
        return names;
    };
    using IdName = QPair<QString, QString>;                   // (id, name)
    const auto listsById = [&store]() {
        QVector<IdName> rows;
        for (const TaskList &row : store.lists()) if (row.folderId.isEmpty()) rows.append(IdName{row.id, row.name});
        std::sort(rows.begin(), rows.end(), [](const IdName &a, const IdName &b) { return a.first < b.first; });
        return rows;
    };
    const auto foldersById = [&store]() {
        QVector<IdName> rows;
        for (const Folder &row : store.folders()) rows.append(IdName{row.id, row.name});
        std::sort(rows.begin(), rows.end(), [](const IdName &a, const IdName &b) { return a.first < b.first; });
        return rows;
    };
    const auto namesOf = [](const QVector<IdName> &rows) {
        QStringList names; for (const IdName &row : rows) names.append(row.second); return names;
    };
    const auto indexOfId = [](const QVector<IdName> &rows, const QString &id) {
        for (int i = 0; i < rows.size(); ++i) if (rows.at(i).first == id) return i;
        return -1;
    };
    QVector<IdName> listBlock = listsById(), folderBlock = foldersById();
    QCOMPARE(listBlock.size(), 5); QCOMPARE(folderBlock.size(), 2);
    QStringList expected = namesOf(listBlock) + namesOf(folderBlock);
    QCOMPARE(mergedNames(), expected);                        // 全 0：清单块在前、文件夹块在后

    // 全 0 数据上带锚点重排：D 移到 B 之前。
    QVector<IdName> reordered = listBlock;
    reordered.remove(indexOfId(reordered, lD));
    reordered.insert(indexOfId(reordered, lB), listBlock.at(indexOfId(listBlock, lD)));
    QVERIFY2(store.moveList(lD, QString(), lB, false), qPrintable(store.lastError()));
    QCOMPARE(mergedNames(), namesOf(reordered) + namesOf(folderBlock));
    const QList<QVariantList> mergedRows = queryRows(QStringLiteral(
        "SELECT sort_order FROM (SELECT sort_order FROM lists WHERE folder_id IS NULL "
        "UNION ALL SELECT sort_order FROM folders) ORDER BY sort_order"));
    QCOMPARE(mergedRows.size(), 7);
    for (int i = 0; i < mergedRows.size(); ++i) QCOMPARE(mergedRows.at(i).first().toInt(), i);   // 混合序列连续
    QVERIFY(store.load());
    QCOMPARE(mergedNames(), namesOf(reordered) + namesOf(folderBlock));   // 持久化重载

    // 恢复全 0 后从两清单建夹：新夹精确占据 target(C) 的稳定排序位置。
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=0 WHERE folder_id IS NULL")));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=0")));
    QVERIFY(store.load());
    const int aIndex = indexOfId(listBlock, lA), cIndex = indexOfId(listBlock, lC);
    const QString created = store.createFolderFromLists(QStringLiteral("新夹"), QStringLiteral("📁"), lA, lC);
    QVERIFY2(isUuid(created), qPrintable(store.lastError()));
    const int expectedSlot = cIndex - (aIndex < cIndex ? 1 : 0);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(created)).toInt(), expectedSlot);
    // 两清单保持稳定排序的相对顺序进入新夹。
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lA)).toInt(), aIndex < cIndex ? 0 : 1);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lC)).toInt(), aIndex < cIndex ? 1 : 0);
    // 合并序列 = 清单块去 A/C + 新夹@slot + 文件夹块，且连续 0..n-1。
    QVector<IdName> remaining = listBlock;
    remaining.remove(indexOfId(remaining, lC));
    remaining.remove(indexOfId(remaining, lA));
    QStringList createdExpectation = namesOf(remaining);
    createdExpectation.insert(expectedSlot, QStringLiteral("新夹"));
    createdExpectation += namesOf(folderBlock);
    QCOMPARE(mergedNames(), createdExpectation);
    const QList<QVariantList> createdRows = queryRows(QStringLiteral(
        "SELECT sort_order FROM (SELECT sort_order FROM lists WHERE folder_id IS NULL "
        "UNION ALL SELECT sort_order FROM folders) ORDER BY sort_order"));
    QCOMPARE(createdRows.size(), 6);
    for (int i = 0; i < createdRows.size(); ++i) QCOMPARE(createdRows.at(i).first().toInt(), i);
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
    bool restored = false;
    for (const Folder &row : reloaded.folders())
        if (row.id == created) restored = row.sortOrder == expectedSlot && row.icon == QStringLiteral("📁");
    QVERIFY(restored);
}

// 显式顺序数据：createFolderFromLists 占据 target 中位（source 在前/在后两种方向），
// 清单跨越已有文件夹移动后顶层混合序列规范化、文件夹仅在顺序变化时保存恰一次。
void TaskTests::mixedTopLevelOrderAndCrossFolderMoves()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString fWork = store.createFolder(QStringLiteral("工作"));
    const QString fHome = store.createFolder(QStringLiteral("家"));
    const QString lA = store.createList(QStringLiteral("A"));
    const QString lB = store.createList(QStringLiteral("B"));
    const QString lC = store.createList(QStringLiteral("C"));
    const QString lD = store.createList(QStringLiteral("D"));
    const QString lE = store.createList(QStringLiteral("E"));
    const QString inbox = store.inboxListId();
    QVERIFY(isUuid(fWork) && isUuid(fHome) && isUuid(lA) && isUuid(lB) && isUuid(lC) && isUuid(lD) && isUuid(lE));
    // 显式混合顺序：收集箱,A,B,C,D,E,工作,家 → 0..7。
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=0 WHERE id='%1'").arg(inbox)));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=1 WHERE id='%1'").arg(lA)));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=2 WHERE id='%1'").arg(lB)));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=3 WHERE id='%1'").arg(lC)));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=4 WHERE id='%1'").arg(lD)));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=5 WHERE id='%1'").arg(lE)));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=6 WHERE id='%1'").arg(fWork)));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=7 WHERE id='%1'").arg(fHome)));
    QVERIFY(store.load());
    const auto mergedNames = []() {
        QStringList names;
        const QList<QVariantList> rows = queryRows(QStringLiteral(
            "SELECT name FROM (SELECT sort_order,0 AS k,id,name FROM lists WHERE folder_id IS NULL "
            "UNION ALL SELECT sort_order,1 AS k,id,name FROM folders) ORDER BY sort_order,k,id"));
        for (const QVariantList &row : rows) names.append(row.first().toString());
        return names;
    };
    const auto revisionOf = [](const QString &table, const QString &id) {
        return scalar(QStringLiteral("SELECT revision FROM %1 WHERE id='%2'").arg(table, id)).toLongLong();
    };
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("A"), QStringLiteral("B"),
               QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("E"),
               QStringLiteral("工作"), QStringLiteral("家")}));

    // source(A) 在 target(C) 之前：新夹占据 C 的原 UI 位置（中位），后续项依次前移恰一格。
    const qint64 inboxRev = revisionOf(QStringLiteral("lists"), inbox);
    const qint64 bRev = revisionOf(QStringLiteral("lists"), lB);
    const qint64 workRev = revisionOf(QStringLiteral("folders"), fWork);
    const QString folderOne = store.createFolderFromLists(QStringLiteral("夹一"), QString(), lA, lC);
    QVERIFY2(isUuid(folderOne), qPrintable(store.lastError()));
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("B"), QStringLiteral("夹一"),
               QStringLiteral("D"), QStringLiteral("E"), QStringLiteral("工作"), QStringLiteral("家")}));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(folderOne)).toInt(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lA)).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lC)).toInt(), 1);
    // 每个被移动的行恰保存一次（revision +1），未动的收集箱不变：无 CAS 重复保存。
    QCOMPARE(revisionOf(QStringLiteral("lists"), inbox), inboxRev);
    QCOMPARE(revisionOf(QStringLiteral("lists"), lB), bRev + 1);
    QCOMPARE(revisionOf(QStringLiteral("folders"), fWork), workRev + 1);

    // source(D) 在 target(B) 之后：夹二仍占据 B 的位置（source 在后不前移槽位）。
    const QString folderTwo = store.createFolderFromLists(QStringLiteral("夹二"), QString(), lD, lB);
    QVERIFY2(isUuid(folderTwo), qPrintable(store.lastError()));
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("E"), QStringLiteral("工作"), QStringLiteral("家")}));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(folderTwo)).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lB)).toInt(), 0);   // B 在前
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lD)).toInt(), 1);   // D 在后

    // setListFolder 转调 moveList：进入已有内容的容器分配末尾顺序，不保留旧 sortOrder。
    QVERIFY2(store.setListFolder(lC, folderTwo), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lC)).toInt(), 2);   // 夹二尾部
    QCOMPARE(scalar(QStringLiteral("SELECT folder_id FROM lists WHERE id='%1'").arg(lC)).toString(), folderTwo);

    // 制造顶层间隙（工作被挪到 8），清单跨入已有文件夹后顶层混合序列规范化，后续文件夹前移恰一次。
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=8 WHERE id='%1'").arg(fWork)));
    QVERIFY(store.load());
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("E"), QStringLiteral("家"), QStringLiteral("工作")}));
    const qint64 homeRev = revisionOf(QStringLiteral("folders"), fHome);
    const qint64 workRevLate = revisionOf(QStringLiteral("folders"), fWork);
    QVERIFY2(store.moveList(lE, folderOne, lA, true), qPrintable(store.lastError()));   // E 进夹一（A 之后）
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("家"), QStringLiteral("工作")}));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lE)).toInt(), 1);   // 夹一内 A 前 0、E 后 1
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lA)).toInt(), 0);
    QCOMPARE(revisionOf(QStringLiteral("folders"), fHome), homeRev + 1);                // 家 5→3 保存一次
    QCOMPARE(revisionOf(QStringLiteral("folders"), fWork), workRevLate + 1);            // 工作 8→4 保存一次

    // 文件夹 → 顶层末尾；顶层 → 已有文件夹（setListFolder 尾部）。
    QVERIFY2(store.moveList(lD, QString()), qPrintable(store.lastError()));
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("家"), QStringLiteral("工作"), QStringLiteral("D")}));
    QVERIFY2(store.setListFolder(lD, fWork), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(lD)).toInt(), 0);   // 工作 内 0
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("家"), QStringLiteral("工作")}));                           // D 移出后顶层已连续，无额外写入

    // 持久化重载：合并序列与文件夹内顺序保持。
    QVERIFY(store.load());
    QCOMPARE(mergedNames(), (QStringList{QStringLiteral("收集箱"), QStringLiteral("夹二"), QStringLiteral("夹一"),
               QStringLiteral("家"), QStringLiteral("工作")}));
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
    QStringList reloadedInOne, reloadedInTwo, reloadedInWork;
    {
        QVector<TaskList> rows;
        for (const TaskList &row : reloaded.lists()) if (row.folderId == folderOne) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &a, const TaskList &b) { return a.sortOrder < b.sortOrder; });
        for (const TaskList &row : rows) reloadedInOne.append(row.name);
        rows.clear();
        for (const TaskList &row : reloaded.lists()) if (row.folderId == folderTwo) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &a, const TaskList &b) { return a.sortOrder < b.sortOrder; });
        for (const TaskList &row : rows) reloadedInTwo.append(row.name);
        rows.clear();
        for (const TaskList &row : reloaded.lists()) if (row.folderId == fWork) rows.append(row);
        std::sort(rows.begin(), rows.end(), [](const TaskList &a, const TaskList &b) { return a.sortOrder < b.sortOrder; });
        for (const TaskList &row : rows) reloadedInWork.append(row.name);
    }
    QCOMPARE(reloadedInOne, (QStringList{QStringLiteral("A"), QStringLiteral("E")}));
    QCOMPARE(reloadedInTwo, (QStringList{QStringLiteral("B"), QStringLiteral("C")}));     // D 已移去工作
    QCOMPARE(reloadedInWork, (QStringList{QStringLiteral("D")}));
}

void TaskTests::organizationNameLengthLimits()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString maximumName(kListNameMaxLength, QChar(0x540D));
    const QString tooLongName(kListNameMaxLength + 1, QChar(0x540D));
    const QString folder = store.createFolder(QStringLiteral("  %1  ").arg(maximumName));
    const QString list = store.createList(QStringLiteral("  %1  ").arg(maximumName));
    QVERIFY(isUuid(folder)); QVERIFY(isUuid(list));
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM folders WHERE id='%1'").arg(folder)).toString(), maximumName);
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM lists WHERE id='%1'").arg(list)).toString(), maximumName);
    Folder folderRow;
    TaskList listRow;
    for (const Folder &row : store.folders()) if (row.id == folder) folderRow = row;
    for (const TaskList &row : store.lists()) if (row.id == list) listRow = row;
    const auto beforeFolders = queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id"));
    const auto beforeLists = queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id"));
    QVERIFY(store.createFolder(tooLongName).isEmpty());
    QVERIFY(store.createList(tooLongName).isEmpty());
    QVERIFY(!store.renameFolder(folder, tooLongName));
    QVERIFY(!store.renameList(list, tooLongName));
    QVERIFY(!store.updateList(list, tooLongName, QString(), QString(), folder));
    Folder invalidFolder = folderRow; invalidFolder.name = tooLongName;
    TaskList invalidList = listRow; invalidList.name = tooLongName;
    QVERIFY(!store.saveFolder(invalidFolder));
    QVERIFY(!store.saveList(invalidList));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), beforeFolders);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id")), beforeLists);

    // 绕过门面的新建与更新也执行同一长度限制，失败不推进 revision。
    TaskRepository repository(Database::instance().connection());
    EntityChanges changes; EntitySnapshot after; QString error;
    changes.folderSaves.append(invalidFolder);
    QVERIFY(!repository.apply(&changes, &after, &error));
    changes = EntityChanges(); changes.groupSaves.append(invalidList);
    QVERIFY(!repository.apply(&changes, &after, &error));
    invalidFolder.id = newUuid();
    changes = EntityChanges(); changes.folderSaves.append(invalidFolder);
    QVERIFY(!repository.apply(&changes, &after, &error));
    invalidList.id = newUuid();
    changes = EntityChanges(); changes.groupSaves.append(invalidList);
    QVERIFY(!repository.apply(&changes, &after, &error));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM folders ORDER BY id")), beforeFolders);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id")), beforeLists);

    const QString renamed(kListNameMaxLength, QChar(0x65B0));
    QVERIFY(store.renameFolder(folder, QStringLiteral(" %1 ").arg(renamed)));
    QVERIFY(store.renameList(list, QStringLiteral(" %1 ").arg(renamed)));
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM folders WHERE id='%1'").arg(folder)).toString(), renamed);
    QCOMPARE(scalar(QStringLiteral("SELECT name FROM lists WHERE id='%1'").arg(list)).toString(), renamed);
}

void TaskTests::deleteListMovesContentToTrashAtomically()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("删除文件夹"));
    const QString list = store.createList(QStringLiteral("待删清单"), folder);
    const QString section = store.createSection(list, QStringLiteral("分区"));
    const QString active = store.createTask(QStringLiteral("活动任务"), list, section);
    {
        Entity value = store.entity(active);
        value.dueAt = utc(2026, 3, 1, 9); value.reminderAt = utc(2026, 3, 1, 8); value.folderId = folder;
        value.tags = QStringList{QStringLiteral("随清单删除")};
        QVERIFY(store.saveTask(value));
    }
    const QString done = store.createTask(QStringLiteral("完成任务"), list, section);
    QVERIFY(store.setCompleted(done, true));
    const QString abandoned = store.createTask(QStringLiteral("放弃任务"), list, section);
    QVERIFY(store.abandonTask(abandoned));
    const QString trashed = store.createTask(QStringLiteral("回收站任务"), list, section);
    QVERIFY(store.trashTask(trashed));
    const QString note = store.createNote(QStringLiteral("清单笔记"), list, section);
    const qint64 originalDeletedAt = utcMs(store.entity(trashed).deletedAt);
    const qint64 originalCompletedAt = utcMs(store.entity(done).completedAt);
    const int count = store.entities().size();
    const qint64 before = utcMs(utcNow());
    QVERIFY2(store.deleteList(list), qPrintable(store.lastError()));
    const qint64 after = utcMs(utcNow());
    QCOMPARE(store.entities().size(), count);                       // 全部保留，只是进入回收站
    bool sawList = false;
    for (const TaskList &candidate : store.lists()) sawList = sawList || candidate.id == list;
    QVERIFY(!sawList);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sections WHERE list_id='%1'").arg(list)).toInt(), 0);
    const QString inbox = store.inboxListId();
    for (const QString &id : {active, done, abandoned, trashed, note}) {
        const Entity entity = store.entity(id);
        QVERIFY2(!entity.id.isEmpty(), "实体随清单删除而丢失");
        QCOMPARE(entity.listId, inbox);
        QVERIFY(entity.sectionId.isEmpty());
        QVERIFY(entity.folderId.isEmpty());
        QVERIFY(entity.deleted);
        QVERIFY(entity.deletedAt.isValid());
    }
    QCOMPARE(store.entity(active).status, TaskStatus::Active);
    QVERIFY(utcMs(store.entity(active).deletedAt) >= before);
    QVERIFY(utcMs(store.entity(active).deletedAt) <= after);        // 未删除者记本次提交时间
    QCOMPARE(store.entity(done).status, TaskStatus::Completed);
    QCOMPARE(utcMs(store.entity(done).completedAt), originalCompletedAt);
    QCOMPARE(store.entity(abandoned).status, TaskStatus::Abandoned);
    QCOMPARE(utcMs(store.entity(trashed).deletedAt), originalDeletedAt);  // 已删除者保留原时间
    QCOMPARE(store.entity(note).kind, EntityKind::Note);
    QCOMPARE(store.entity(active).reminders.size(), 1);             // 提醒原样保留
    QCOMPARE(store.entity(active).tags, QStringList{QStringLiteral("随清单删除")});
    QVERIFY(store.restoreTask(active));                             // 沿用现有恢复机制
    QVERIFY(!store.entity(active).deleted);
    QCOMPARE(store.entity(active).listId, inbox);

    // 原子性：清单内实体搬移被拒时整个删除回滚。
    const QString otherList = store.createList(QStringLiteral("阻断清单"));
    const QString otherTask = store.createTask(QStringLiteral("阻断任务"), otherList);
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_list_recycle BEFORE UPDATE ON entities "
        "WHEN NEW.list_id=(SELECT id FROM lists WHERE is_inbox=1) AND OLD.list_id<>NEW.list_id "
        "BEGIN SELECT RAISE(ABORT,'recycle rejected'); END")));
    QVERIFY(!store.deleteList(otherList));
    bool otherListExists = false;
    for (const TaskList &candidate : store.lists()) otherListExists = otherListExists || candidate.id == otherList;
    QVERIFY(otherListExists);
    QVERIFY(!store.entity(otherTask).deleted);
    QCOMPARE(store.entity(otherTask).listId, otherList);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_list_recycle")));
    QVERIFY(store.deleteList(otherList));
    QVERIFY(store.entity(otherTask).deleted);
}

void TaskTests::viewSpecScopesAndArchivedFiltering()
{
    const QDate today(2026, 11, 15);
    const QString normalList = newUuid(), archivedList = newUuid();
    const QSet<QString> noArchive;
    const QSet<QString> archiveSet{archivedList};
    const auto task = [](const QString &listId, const QDate &date) {
        Entity value; value.id = newUuid(); value.kind = EntityKind::Task;
        value.title = QStringLiteral("任务"); value.listId = listId; value.allDayDate = date;
        return value;
    };
    const Entity overdueNormal = task(normalList, today.addDays(-3));
    const Entity todayNormal = task(normalList, today);
    const Entity tomorrowNormal = task(normalList, today.addDays(1));
    const Entity day6Normal = task(normalList, today.addDays(6));
    const Entity day7Normal = task(normalList, today.addDays(7));
    const Entity overdueArchived = task(archivedList, today.addDays(-2));
    const Entity tomorrowArchived = task(archivedList, today.addDays(1));
    Entity archivedNote; archivedNote.id = newUuid(); archivedNote.kind = EntityKind::Note;
    archivedNote.title = QStringLiteral("归档清单笔记"); archivedNote.listId = archivedList;
    Entity archivedDone; archivedDone.id = newUuid(); archivedDone.kind = EntityKind::Task;
    archivedDone.title = QStringLiteral("归档清单完成"); archivedDone.listId = archivedList;
    archivedDone.status = TaskStatus::Completed; archivedDone.completed = true; archivedDone.completedAt = utc(2026, 11, 1);
    Entity archivedTrash; archivedTrash.id = newUuid(); archivedTrash.kind = EntityKind::Task;
    archivedTrash.title = QStringLiteral("归档清单回收站"); archivedTrash.listId = archivedList;
    archivedTrash.deleted = true; archivedTrash.deletedAt = utc(2026, 11, 2);
    const QVector<Entity> all{overdueNormal, todayNormal, tomorrowNormal, day6Normal, day7Normal,
                              overdueArchived, tomorrowArchived, archivedNote, archivedDone, archivedTrash};

    ViewSpec spec;
    spec.scope = ViewScope::Today;
    QVERIFY(contains(overdueNormal, spec, today, noArchive));       // Today 保持包含逾期
    QVERIFY(contains(todayNormal, spec, today, noArchive));
    QVERIFY(!contains(tomorrowNormal, spec, today, noArchive));
    QVERIFY(contains(overdueArchived, spec, today, noArchive));
    QVERIFY(!contains(overdueArchived, spec, today, archiveSet));   // 归档清单排除

    spec.scope = ViewScope::Tomorrow;
    QVERIFY(!contains(overdueNormal, spec, today, noArchive));      // 只含明天，不含今天与逾期
    QVERIFY(!contains(todayNormal, spec, today, noArchive));
    QVERIFY(contains(tomorrowNormal, spec, today, noArchive));
    QVERIFY(!contains(day6Normal, spec, today, noArchive));
    QVERIFY(contains(tomorrowArchived, spec, today, noArchive));
    QVERIFY(!contains(tomorrowArchived, spec, today, archiveSet));

    spec.scope = ViewScope::Next7Days;
    QVERIFY(contains(overdueNormal, spec, today, noArchive));       // 保留全部逾期，未来含今天至今天+6
    const Entity longOverdue = task(normalList, today.addDays(-730));
    QVERIFY(contains(longOverdue, spec, today, noArchive));
    QVERIFY(contains(todayNormal, spec, today, noArchive));
    QVERIFY(contains(tomorrowNormal, spec, today, noArchive));
    QVERIFY(contains(day6Normal, spec, today, noArchive));          // 今天+6 为最后一天
    QVERIFY(!contains(day7Normal, spec, today, noArchive));         // 今天+7 排除
    QVERIFY(!contains(overdueArchived, spec, today, archiveSet));
    QVERIFY(!contains(tomorrowArchived, spec, today, archiveSet));

    QVector<Entity> recent;
    for (int i = 0; i < 4; ++i) {
        Entity pinned = task(normalList, today.addDays(i)); pinned.pinnedAt = utc(2026, 11, 14);
        recent.append(pinned);
    }
    recent << longOverdue << overdueNormal << todayNormal << task(normalList, today)
           << tomorrowNormal << task(normalList, today.addDays(2)) << task(normalList, today.addDays(4));
    recent << day7Normal << overdueArchived << tomorrowArchived;
    const QVector<ViewSection> recentSections = buildSections(recent, spec, today, archiveSet);
    QCOMPARE(recentSections.size(), 5);
    QCOMPARE(recentSections.at(0).key, GroupKey::pinned().stableId());
    QCOMPARE(recentSections.at(0).items.size(), 4);
    QCOMPARE(recentSections.at(1).key, GroupKey::forTime(TimeBucket::Overdue).stableId());
    QCOMPARE(recentSections.at(1).items.size(), 2);
    QCOMPARE(recentSections.at(2).key, GroupKey::forTime(TimeBucket::Today).stableId());
    QCOMPARE(recentSections.at(2).items.size(), 2);
    QCOMPARE(recentSections.at(3).key, GroupKey::forTime(TimeBucket::Tomorrow).stableId());
    QCOMPARE(recentSections.at(3).items.size(), 1);
    QCOMPARE(recentSections.at(4).key, GroupKey::forTime(TimeBucket::Next7Days).stableId());
    QCOMPARE(recentSections.at(4).items.size(), 2);                  // 默认 Time 分组，筛选口径仍为 4+2+5=11。

    spec.scope = ViewScope::Overdue;
    QVERIFY(contains(overdueNormal, spec, today, noArchive));
    QVERIFY(!contains(todayNormal, spec, today, noArchive));
    QVERIFY(contains(overdueArchived, spec, today, noArchive));
    QVERIFY(!contains(overdueArchived, spec, today, archiveSet));

    spec.scope = ViewScope::AllLists;
    QVERIFY(contains(archivedNote, spec, today, noArchive));
    QVERIFY(!contains(archivedNote, spec, today, archiveSet));      // AllLists 排除归档清单活动实体
    QVERIFY(!contains(archivedDone, spec, today, archiveSet));      // 完成任务本就不在活动视图

    spec.scope = ViewScope::List; spec.listId = archivedList;
    QVERIFY(contains(tomorrowArchived, spec, today, archiveSet));   // 显式 List 作用域可查看归档清单
    QVERIFY(contains(archivedNote, spec, today, archiveSet));
    QVERIFY(!contains(tomorrowNormal, spec, today, archiveSet));

    spec.scope = ViewScope::Completed;
    QVERIFY(contains(archivedDone, spec, today, archiveSet));       // 历史视图保留可见性
    spec.scope = ViewScope::Trash;
    QVERIFY(contains(archivedTrash, spec, today, archiveSet));

    spec = ViewSpec(); spec.scope = ViewScope::AllLists;
    Entity pinned = overdueNormal; pinned.pinnedAt = utc(2026, 11, 14);
    const QVector<Entity> pinnedAll{pinned, todayNormal, archivedNote};
    const QVector<ViewSection> sections = buildSections(pinnedAll, spec, today, archiveSet);
    QVERIFY(!sections.isEmpty());
    QCOMPARE(sections.first().key, QStringLiteral("pinned"));       // 稳定机器标识
    QCOMPARE(sections.first().title, GroupKey::pinned().title(today));
    for (const ViewSection &section : sections)
        for (const Entity &entity : section.items) QVERIFY(entity.id != archivedNote.id);
    spec.scope = ViewScope::Completed;
    const QVector<ViewSection> archiveSections = buildSections(QVector<Entity>{archivedDone}, spec, today, archiveSet);
    QCOMPARE(archiveSections.size(), 1);
    QCOMPARE(archiveSections.first().key, GroupKey::forTime(TimeBucket::Undated).stableId()); // 历史视角同样按所选模式分组。
    QCOMPARE(archiveSections.first().items.first().id, archivedDone.id);
    QCOMPARE(archiveSections.first().items.first().status, TaskStatus::Completed);
    spec.scope = ViewScope::Tomorrow; spec.renderer = ViewRenderer::Time;
    const QVector<ViewSection> tomorrowSections = buildSections(QVector<Entity>{todayNormal, tomorrowNormal, day6Normal}, spec, today, noArchive);
    QCOMPARE(tomorrowSections.size(), 1);
    QCOMPARE(tomorrowSections.first().items.size(), 1);
    QCOMPARE(tomorrowSections.first().items.first().id, tomorrowNormal.id);
}

void TaskTests::archivedListsSuppressReminders()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString list = store.createList(QStringLiteral("归档提醒清单"));
    const QString first = store.createTask(QStringLiteral("归档前到期"), list);
    {
        Entity value = store.entity(first);
        value.dueAt = now.addSecs(3600); value.reminderAt = now.addSecs(-60);
        QVERIFY(store.saveTask(value));
    }
    ReminderScheduler scheduler(&store);
    QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    scheduler.checkDue(now); QCOMPARE(triggered.count(), 1);        // 未归档：正常提醒
    QVERIFY(store.setListArchived(list, true));
    // 在已归档清单里补一个已到期任务：归档期间任何入口都不能提醒。
    const QString second = store.createTask(QStringLiteral("归档后到期"), list);
    {
        Entity value = store.entity(second);
        value.dueAt = now.addSecs(3600); value.reminderAt = now.addSecs(-30);
        QVERIFY(store.saveTask(value));
    }
    const TaskReminder secondReminder = store.entity(second).reminders.first();
    scheduler.checkDue(now); QCOMPARE(triggered.count(), 1);        // 归档清单：初查即排除
    QVERIFY(!store.markReminderDelivered(secondReminder.id, secondReminder.triggerAt, now));
    QVERIFY(!store.markReminderNotified(second, secondReminder.triggerAt, now));
    QVERIFY(!scheduler.snooze(second));
    QVERIFY(!scheduler.snoozeReminder(secondReminder.id));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), 1);
    // Repository SQL 是权威闸门：绕过 Store 直接投递也必须失败。
    {
        TaskRepository repository(Database::instance().connection());
        EntityChanges changes; EntitySnapshot after; QString error;
        ReminderDelivery delivery;
        delivery.reminderId = secondReminder.id; delivery.installationId = store.installationId();
        delivery.scheduledAt = secondReminder.triggerAt; delivery.deliveredAt = now;
        changes.deliverySaves.append(delivery);
        QVERIFY(!repository.apply(&changes, &after, &error));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), 1);
    }
    QCOMPARE(store.entity(second).reminders.size(), 1);             // 提醒不被删除
    QCOMPARE(store.entity(second).status, TaskStatus::Active);      // 任务状态不变
    QVERIFY(store.setListArchived(list, false));
    scheduler.checkDue(now); QCOMPARE(triggered.count(), 2);        // 取消归档恢复提醒
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), 2);
}

void TaskTests::postponeOverdueTasksShiftsDates()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today(2026, 11, 15);
    const QString list = store.createList(QStringLiteral("顺延清单"));
    const QString timed = store.createTask(QStringLiteral("定时逾期"), list);
    {
        Entity value = store.entity(timed);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(9, 5)));
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(18, 30)));
        value.hasScheduleRange = true;
        value.reminderAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(18, 0)));
        QVERIFY(store.saveTask(value));
    }
    const QString allDay = store.createTask(QStringLiteral("全天逾期"), list);
    {
        Entity value = store.entity(allDay); value.allDayDate = QDate(2026, 11, 12);
        QVERIFY(store.saveTask(value));
    }
    const QString dueToday = store.createTask(QStringLiteral("今天到期"), list);
    {
        Entity value = store.entity(dueToday); value.allDayDate = today;
        QVERIFY(store.saveTask(value));
    }
    const QString future = store.createTask(QStringLiteral("未来到期"), list);
    {
        Entity value = store.entity(future); value.allDayDate = today.addDays(3);
        QVERIFY(store.saveTask(value));
    }
    const QString done = store.createTask(QStringLiteral("完成逾期"));
    {
        Entity value = store.entity(done); value.allDayDate = today.addDays(-1);
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(store.setCompleted(done, true));
    const QString trashed = store.createTask(QStringLiteral("回收站逾期"));
    {
        Entity value = store.entity(trashed); value.allDayDate = today.addDays(-1);
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(store.trashTask(trashed));
    const QString archivedList = store.createList(QStringLiteral("归档顺延清单"));
    const QString archivedTask = store.createTask(QStringLiteral("归档清单逾期"), archivedList);
    {
        Entity value = store.entity(archivedTask); value.allDayDate = today.addDays(-2);
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(store.setListArchived(archivedList, true));

    QVERIFY(!store.postponeOverdueTasks(QStringList{timed, newUuid()}, today));   // 未知 id 直接失败
    QStringList ids{timed, allDay, dueToday, future, done, trashed, archivedTask};
    QVERIFY2(store.postponeOverdueTasks(ids, today), qPrintable(store.lastError()));
    const Entity timedAfter = store.entity(timed);
    QCOMPARE(toAppTime(timedAfter.startAt).date(), today);                         // 保留时刻只改日期
    QCOMPARE(toAppTime(timedAfter.startAt).time(), QTime(9, 5));
    QCOMPARE(toAppTime(timedAfter.dueAt).date(), today);
    QCOMPARE(toAppTime(timedAfter.dueAt).time(), QTime(18, 30));
    QCOMPARE(store.entity(timed).status, TaskStatus::Active);
    QCOMPARE(store.entity(timed).reminders.size(), 1);
    QCOMPARE(store.entity(allDay).allDayDate, today);
    QCOMPARE(store.entity(dueToday).allDayDate, today);            // 今天的不动
    QCOMPARE(store.entity(future).allDayDate, today.addDays(3));   // 未来不动
    QCOMPARE(store.entity(done).allDayDate, today.addDays(-1));    // 完成不动
    QVERIFY(store.entity(done).completed);
    QCOMPARE(store.entity(trashed).allDayDate, today.addDays(-1)); // 回收站不动
    QVERIFY(store.entity(trashed).deleted);
    QCOMPARE(store.entity(archivedTask).allDayDate, today.addDays(-2)); // 归档清单不动

    // 原子失败：任一更新被拒则全部回滚。
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_postpone BEFORE UPDATE ON entities "
        "WHEN NEW.all_day_date='2026-11-15' AND OLD.all_day_date<'2026-11-15' "
        "BEGIN SELECT RAISE(ABORT,'postpone rejected'); END")));
    {
        Entity value = store.entity(timed);                        // 两个任务都重置回逾期
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(9, 5)));
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(18, 30)));
        value.hasScheduleRange = true;
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(allDay); value.allDayDate = QDate(2026, 11, 12);
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(!store.postponeOverdueTasks(QStringList{timed, allDay}, today));
    QCOMPARE(store.entity(allDay).allDayDate, QDate(2026, 11, 12)); // 无部分成功
    QCOMPARE(toAppTime(store.entity(timed).dueAt).date(), QDate(2026, 11, 10));
    QCOMPARE(toAppTime(store.entity(timed).dueAt).time(), QTime(18, 30));
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_postpone")));
    QVERIFY(store.postponeOverdueTasks(QStringList{allDay, timed}, today));
    QCOMPARE(store.entity(allDay).allDayDate, today);
    QCOMPARE(toAppTime(store.entity(timed).dueAt).date(), today);
    QCOMPARE(toAppTime(store.entity(timed).dueAt).time(), QTime(18, 30));
}

void TaskTests::postponePreservesDateSpansAndReminders()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today(2026, 11, 15);
    const QString overnight = store.createTask(QStringLiteral("跨午夜逾期"));
    const QString multiDay = store.createTask(QStringLiteral("跨多日逾期"));
    const QString startOnly = store.createTask(QStringLiteral("单点截止日期逾期"));
    const QString allDay = store.createTask(QStringLiteral("全天且保留开始日期"));
    {
        Entity value = store.entity(overnight);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(22, 15, 25, 123)));
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 11), QTime(7, 30, 40, 456)));
        value.hasScheduleRange = true;
        for (int i = 0; i < 3; ++i) {
            TaskReminder reminder; reminder.triggerAt = value.startAt.addSecs((i - 2) * 3600);
            value.reminders.append(reminder);
        }
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(multiDay);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 7), QTime(9, 5)));
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 11), QTime(18, 30)));
        value.hasScheduleRange = true;
        value.recurrence = QStringLiteral("daily");
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(startOnly);
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 12), QTime(0, 30)));
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(allDay);
        value.allDayDate = QDate(2026, 11, 11);
        value.allDayEndDate = QDate(2026, 11, 12);
        value.hasScheduleRange = true;
        QVERIFY(store.saveTask(value));
    }
    const TaskReminder delivered = store.entity(overnight).reminders.first();
    QVERIFY(store.markReminderDelivered(delivered.id, delivered.triggerAt, utc(2026, 11, 14)));
    QVERIFY(store.saveDesktopState(overnight, NoteColor::Blue, QRect(30, 40, 320, 300), 3, NoteLayer::OnTop, true));
    const Entity overnightBefore = store.entity(overnight), multiBefore = store.entity(multiDay);
    const auto reminderRows = queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id"));
    const auto deliveryRows = queryRows(QStringLiteral("SELECT * FROM reminder_deliveries ORDER BY reminder_id,installation_id"));
    const auto desktopRows = queryRows(QStringLiteral("SELECT * FROM desktop_notes ORDER BY id"));
    const int count = store.entities().size();

    QVERIFY2(store.postponeOverdueTasks(QStringList{overnight, multiDay, startOnly, allDay}, today), qPrintable(store.lastError()));
    const Entity overnightAfter = store.entity(overnight), multiAfter = store.entity(multiDay);
    QCOMPARE(toAppTime(overnightAfter.startAt).date(), today.addDays(-1));
    QCOMPARE(toAppTime(overnightAfter.startAt).time(), QTime(22, 15, 25, 123));
    QCOMPARE(toAppTime(overnightAfter.dueAt).date(), today);
    QCOMPARE(toAppTime(overnightAfter.dueAt).time(), QTime(7, 30, 40, 456));
    QCOMPARE(overnightAfter.startAt.msecsTo(overnightAfter.dueAt), overnightBefore.startAt.msecsTo(overnightBefore.dueAt));
    QCOMPARE(toAppTime(multiAfter.startAt).date(), today.addDays(-4));
    QCOMPARE(toAppTime(multiAfter.startAt).time(), QTime(9, 5));
    QCOMPARE(toAppTime(multiAfter.dueAt).date(), today);
    QCOMPARE(toAppTime(multiAfter.dueAt).time(), QTime(18, 30));
    QCOMPARE(multiAfter.startAt.msecsTo(multiAfter.dueAt), multiBefore.startAt.msecsTo(multiBefore.dueAt));
    QCOMPARE(multiAfter.recurrenceSeriesId, multiBefore.recurrenceSeriesId);
    QCOMPARE(multiAfter.rrule, multiBefore.rrule);
    QCOMPARE(store.entities().size(), count);                      // 顺延重复任务不生成下一实例
    QCOMPARE(toAppTime(store.entity(startOnly).dueAt).date(), today);
    QCOMPARE(toAppTime(store.entity(startOnly).dueAt).time(), QTime(0, 30));
    QVERIFY(!store.entity(startOnly).startAt.isValid());
    QCOMPARE(store.entity(allDay).allDayDate, today.addDays(-1));
    QCOMPARE(store.entity(allDay).allDayEndDate, today);
    QVERIFY(!store.entity(allDay).startAt.isValid());
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id")), reminderRows);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminder_deliveries ORDER BY reminder_id,installation_id")), deliveryRows);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM desktop_notes ORDER BY id")), desktopRows);
    QCOMPARE(overnightAfter.reminders.size(), 3);
    QVERIFY(store.isReminderDelivered(delivered.id));
}

// 拖放分区纯规则：固定 today 的四时间分区映射、非法 key/非法 today 的无效值。
void TaskTests::createSubtaskInheritsDatesOnly()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QString list = store.createList(QStringLiteral("子任务清单"));
    const QString section = store.createSection(list, QStringLiteral("段落"));
    const QString parent = store.createTask(QStringLiteral("父任务"), list, section);
    {
        Entity value = store.entity(parent);
        value.allDayDate = QDate();
        value.startAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        value.dueAt = fromAppTime(QDateTime(today, QTime(18, 0)));
        value.hasScheduleRange = true;
        value.recurrence = QStringLiteral("daily");
        value.reminderAt = fromAppTime(QDateTime(today, QTime(8, 30)));
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(store.setPinnedAt(parent, utc(2026, 5, 1, 6)));
    const Entity parentEntity = store.entity(parent);
    QVERIFY(parentEntity.pinnedAt.isValid());
    QVERIFY(!parentEntity.rrule.isEmpty());
    QCOMPARE(parentEntity.reminders.size(), 1);

    const QString child = store.createSubtask(QStringLiteral("子任务"), parent);
    QVERIFY2(isUuid(child), qPrintable(store.lastError()));
    const Entity value = store.entity(child);
    QCOMPARE(value.parentId, parent);
    QCOMPARE(value.listId, list);
    QCOMPARE(value.sectionId, section);
    QVERIFY(!value.allDayDate.isValid());
    QVERIFY(value.hasScheduleRange);
    QCOMPARE(toAppTime(value.startAt).time(), QTime(9, 0));
    QCOMPARE(toAppTime(value.dueAt).time(), QTime(18, 0));
    QCOMPARE(value.timeZone, parentEntity.timeZone);
    QVERIFY(!value.pinnedAt.isValid());                              // 不继承置顶
    QCOMPARE(value.reminders.size(), 0);                             // 不继承提醒
    QVERIFY(!value.reminderAt.isValid());
    QCOMPARE(value.recurrence, QStringLiteral("none"));              // 不继承重复
    QVERIFY(value.rrule.isEmpty());
    QVERIFY(value.recurrenceSeriesId.isEmpty());
    QCOMPARE(value.status, TaskStatus::Active);
    QVERIFY(!value.completed);
    QCOMPARE(value.sortOrder, 0);                                    // 同级末尾 max+1（尚无兄弟）
    ViewSpec todaySpec; todaySpec.scope = ViewScope::Today; todaySpec.renderer = ViewRenderer::Time;
    QVERIFY(contains(store.entity(child), todaySpec, today));        // 时间视图立即可见

    const QString grandchild = store.createSubtask(QStringLiteral("孙任务"), child);
    QVERIFY(isUuid(grandchild));                                    // 活动任务链可以继续嵌套
    QCOMPARE(store.entity(grandchild).sortOrder, 0);
    {
        Entity draft = store.entity(child); draft.sortOrder = 5; QVERIFY(store.saveTask(draft));
    }
    const QString second = store.createSubtask(QStringLiteral("二子"), parent);
    QCOMPARE(store.entity(second).sortOrder, 6);                     // 同级末尾取兄弟 max+1

    const QString done = store.createTask(QStringLiteral("完成父"));
    QVERIFY(store.setCompleted(done, true));
    QVERIFY(store.createSubtask(QStringLiteral("无效"), done).isEmpty());
    const QString abandoned = store.createTask(QStringLiteral("放弃父"));
    QVERIFY(store.abandonTask(abandoned));
    QVERIFY(store.createSubtask(QStringLiteral("无效"), abandoned).isEmpty());
    const QString trashed = store.createTask(QStringLiteral("回收父"));
    QVERIFY(store.trashTask(trashed));
    QVERIFY(store.createSubtask(QStringLiteral("无效"), trashed).isEmpty());
    const QString note = store.createNote(QStringLiteral("笔记父"), list);
    QVERIFY(store.createSubtask(QStringLiteral("无效"), note).isEmpty());
    QVERIFY(store.createSubtask(QStringLiteral("无效"), newUuid()).isEmpty());
    QVERIFY(store.createSubtask(QStringLiteral("无效"), QString()).isEmpty());
    QVERIFY(store.createSubtask(QStringLiteral("   "), parent).isEmpty());   // 空标题由 prepareSave 拒绝
}

void TaskTests::backupRoundTripAndLegacyRejection()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("文件夹 Ω"), QStringLiteral("🗂")); const QString list = store.createList(QStringLiteral("清单 🐱"), folder);
    QVERIFY2(store.updateList(list, QStringLiteral(" 清单 🐱 "), QStringLiteral("🏠"), QStringLiteral("#AA33CC"), folder), qPrintable(store.lastError()));
    const QString archivedList = store.createList(QStringLiteral("归档清单"));
    QVERIFY(store.setListArchived(archivedList, true));
    const QString section = store.createSection(list, QStringLiteral("段落")); const QString id = store.createTask(QStringLiteral("完整备份"), list, section);
    const QString target = store.createTask(QStringLiteral("内链目标"));
    Entity task = store.entity(id); task.body = QStringLiteral("**Markdown** ") + entityReference(target, QStringLiteral("目标")); task.priority = 3;
    task.sortOrder = 23; task.pinnedAt = utc(2026, 4, 30, 12);
    task.tags = QStringList{QStringLiteral("家庭"), QStringLiteral("标签 Ω")}; task.startAt = utc(2026, 5, 1, 9); task.dueAt = utc(2026, 5, 1, 17);
    task.hasScheduleRange = true;
    task.reminderAt = utc(2026, 5, 1, 8);
    task.color = NoteColor::Green; task.geometry = QRect(-20, 30, 360, 300); QVERIFY(store.saveTask(task));
    task = store.entity(id); // 保存后取得关联数据 UUID 供覆盖恢复比较
    QVERIFY(store.saveDesktopState(id, store.entity(id).color, store.entity(id).geometry, store.entity(id).zOrder, store.entity(id).layer, true));
    const TaskReminder extra = store.addReminder(id, utc(2026, 5, 1, 8, 30)); QVERIFY(!extra.id.isEmpty());
    const QByteArray content("backup-attachment"); const QString source = m_directory->filePath(QStringLiteral("资料.txt")); QVERIFY(writeBytes(source, content));
    const Attachment attachment = store.importAttachment(id, source); QVERIFY(!attachment.id.isEmpty());
    const QString doomed = store.createTask(QStringLiteral("永久删除后的孤儿事件")); QVERIFY(store.trashTask(doomed)); QVERIFY(store.deletePermanently(doomed));
    const QString viewId = newUuid();
    QVERIFY(execute(QStringLiteral("INSERT INTO view_defs(id,name,spec_json,sort_order,created_at_utc_ms,updated_at_utc_ms,revision,server_revision) "
        "VALUES('%1','恢复视图','{\"scope\":\"all\"}',7,%2,%2,3,4)").arg(viewId).arg(utcMs(utc(2026, 5, 2, 8)))));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET server_revision=11")));
    QVERIFY(execute(QStringLiteral("UPDATE lists SET server_revision=CASE WHEN is_inbox=1 THEN 12 ELSE 13 END")));
    QVERIFY(execute(QStringLiteral("UPDATE sections SET server_revision=14")));
    QVERIFY(execute(QStringLiteral("UPDATE entities SET server_revision=CASE WHEN id='%1' THEN 15 ELSE 16 END").arg(id)));
    QVERIFY(execute(QStringLiteral("UPDATE tags SET server_revision=18+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE entity_tags SET server_revision=19+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE reminders SET server_revision=20+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE attachments SET server_revision=21")));
    QVERIFY(store.load());
    Entity localSave = store.entity(target); localSave.body = QStringLiteral("普通保存不改 server_revision"); QVERIFY(store.saveTask(localSave));
    QCOMPARE(scalar(QStringLiteral("SELECT server_revision FROM entities WHERE id='%1'").arg(target)).toLongLong(), qint64(16));
    const QStringList businessTables = {QStringLiteral("folders"),QStringLiteral("lists"),QStringLiteral("sections"),
        QStringLiteral("recurrence_series"),QStringLiteral("entities"),QStringLiteral("tags"),
        QStringLiteral("entity_tags"),QStringLiteral("reminders"),QStringLiteral("recurrence_links"),QStringLiteral("attachments"),
        QStringLiteral("view_defs"),QStringLiteral("entity_events")};
    QHash<QString, QList<QVariantList>> beforeTables;
    for (const QString &table : businessTables) beforeTables.insert(table, queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)));
    TaskRepository repository(Database::instance().connection()); QString eventError;
    const QVector<EntityEvent> expectedEvents = repository.allEvents(&eventError); QVERIFY(eventError.isEmpty()); QVERIFY(expectedEvents.size() > 5);
    QVERIFY(!store.events(doomed).isEmpty());
    const QString installation = store.installationId(), device = store.deviceId(), originalInbox = store.inboxListId();

    QVERIFY2(store.exportBackup(backupPath()), qPrintable(store.lastError()));
    const QByteArray bytes = readBytes(backupPath()); QVERIFY(attachmentcodec::isBackupContainer(bytes));
    QCOMPARE(QString::fromLatin1(bytes.left(4)), QStringLiteral("KLB1"));
    QVector<attachmentcodec::BackupFrame> frames; const QJsonObject root = readMetadata(backupPath(), &frames);
    QCOMPARE(root.value(QStringLiteral("format")).toString(), QStringLiteral("KylinList.Entities"));
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 6); QCOMPARE(frames.size(), 1);
    QCOMPARE(root.value(QStringLiteral("defaultTimezone")).toString(), QStringLiteral("Asia/Shanghai"));
    QCOMPARE(root.value(QStringLiteral("events")).toArray().size(), expectedEvents.size());
    QVERIFY(!root.contains(QStringLiteral("checklistItems")));
    QCOMPARE(root.value(QStringLiteral("entityTags")).toArray().size(), 2);
    QCOMPARE(root.value(QStringLiteral("reminders")).toArray().size(), 2);
    QCOMPARE(root.value(QStringLiteral("viewDefs")).toArray().size(), 1);
    QVERIFY(!root.contains(QStringLiteral("schemaVersion"))); QVERIFY(!root.contains(QStringLiteral("tasks")));
    QVERIFY(!root.contains(QStringLiteral("workspaces"))); QVERIFY(!root.contains(QStringLiteral("localMeta")));
    int exportedInboxes = 0;
    for (const QJsonValue &value : root.value(QStringLiteral("lists")).toArray())
        if (value.toObject().value(QStringLiteral("isInbox")).toBool()) ++exportedInboxes;
    QCOMPARE(exportedInboxes, 1);
    int exportedArchived = 0; QString exportedIcon, exportedColor;
    for (const QJsonValue &value : root.value(QStringLiteral("lists")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("archivedAtUtcMs")).toDouble() > 0) ++exportedArchived;
        if (object.value(QStringLiteral("id")).toString() == list) {
            exportedIcon = object.value(QStringLiteral("icon")).toString();
            exportedColor = object.value(QStringLiteral("color")).toString();
        }
    }
    QCOMPARE(exportedArchived, 1);
    QCOMPARE(exportedIcon, QStringLiteral("🏠")); QCOMPARE(exportedColor, QStringLiteral("#AA33CC"));
    QString exportedFolderIcon;
    for (const QJsonValue &value : root.value(QStringLiteral("folders")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == folder)
            exportedFolderIcon = value.toObject().value(QStringLiteral("icon")).toString();
    QCOMPARE(exportedFolderIcon, QStringLiteral("🗂"));
    QJsonObject exportedTask;
    for (const QJsonValue &value : root.value(QStringLiteral("entities")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == id) exportedTask = value.toObject();
    QCOMPARE(exportedTask.value(QStringLiteral("kind")).toString(), QStringLiteral("task"));
    QCOMPARE(exportedTask.value(QStringLiteral("bodyMarkdown")).toString(), task.body);
    QCOMPARE(exportedTask.value(QStringLiteral("sortOrder")).toInt(), 23);
    QCOMPARE(exportedTask.value(QStringLiteral("serverRevision")).toInt(), 15);
    QVERIFY(exportedTask.value(QStringLiteral("pinnedAtUtcMs")).isDouble());
    QVERIFY(!exportedTask.contains(QStringLiteral("description"))); QVERIFY(!exportedTask.contains(QStringLiteral("favorite")));
    QVERIFY(!root.contains(QStringLiteral("desktopNotes"))); QVERIFY(!root.contains(QStringLiteral("reminderDeliveries")));

    const QString sentinel = store.createTask(QStringLiteral("导入后必须消失的本地哨兵")); QVERIFY(!sentinel.isEmpty());
    QVERIFY(store.setLocalMeta(QStringLiteral("default_timezone"), QStringLiteral("UTC")));
    QVERIFY(store.setLocalMeta(QStringLiteral("signed_in"), QStringLiteral("1")));
    QVERIFY(store.setLocalMeta(QStringLiteral("sync_base_server_revision"), QStringLiteral("77")));
    QVERIFY(!store.createTask(QStringLiteral("产生 outbox")).isEmpty()); QVERIFY(!store.outbox().isEmpty());
    QVERIFY(execute(QStringLiteral("INSERT INTO sync_bindings(id,entity_type,entity_id,remote_id,updated_at_utc_ms) VALUES('%1','task','%2','remote-old',1)")
        .arg(newUuid(), sentinel)));
    QVERIFY(execute(QStringLiteral("INSERT INTO tombstones(id,entity_type,entity_id,deleted_at_utc_ms) VALUES('%1','task','%2',1)")
        .arg(newUuid(), sentinel)));
    QVERIFY(store.markReminderDelivered(extra.id, extra.triggerAt, utcNow()));
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2); QVERIFY(store.entity(sentinel).id.isEmpty());
    bool restoredAppearance = false, restoredArchived = false;
    for (const TaskList &candidate : store.lists()) {
        if (candidate.id == list)
            restoredAppearance = candidate.icon == QStringLiteral("🏠") && candidate.color == QStringLiteral("#AA33CC")
                && candidate.name == QStringLiteral("清单 🐱") && candidate.archivedAtUtcMs == 0;
        if (candidate.id == archivedList) restoredArchived = candidate.archivedAtUtcMs > 0;
    }
    QVERIFY(restoredAppearance); QVERIFY(restoredArchived);   // 覆盖恢复保留外观与归档状态
    bool restoredFolderIcon = false;
    for (const Folder &candidate : store.folders())
        if (candidate.id == folder) restoredFolderIcon = candidate.icon == QStringLiteral("🗂");
    QVERIFY(restoredFolderIcon);                              // 覆盖恢复保留文件夹图标
    QCOMPARE(store.entity(id).id, id); QCOMPARE(store.entity(id).body, task.body); QCOMPARE(store.entity(target).title, QStringLiteral("内链目标"));
    QCOMPARE(store.entity(id).listId, list); QCOMPARE(store.entity(id).sectionId, section); QCOMPARE(store.entity(id).folderId, task.folderId);
    QCOMPARE(store.entity(id).sortOrder, qint64(23)); QCOMPARE(store.entity(id).pinnedAt, task.pinnedAt);
    QCOMPARE(store.entity(id).reminders.size(), 2);
    QVERIFY(store.entity(id).geometry.isEmpty()); QCOMPARE(store.entity(id).color, NoteColor::Yellow);
    const QVector<Attachment> restoredAttachments = store.attachments(id); QCOMPARE(restoredAttachments.size(), 1);
    QCOMPARE(restoredAttachments.first().id, attachment.id); QCOMPARE(restoredAttachments.first().relativePath, attachment.relativePath);
    QCOMPARE(readBytes(attachmentPath(attachment.relativePath)), content);
    QCOMPARE(store.events(doomed).size(), 3); QCOMPARE(repository.allEvents(&eventError).size(), expectedEvents.size());
    for (const QString &table : businessTables) QCOMPARE(queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)), beforeTables.value(table));
    QCOMPARE(store.inboxListId(), originalInbox); QCOMPARE(store.localMeta(QStringLiteral("default_timezone")), QStringLiteral("Asia/Shanghai"));
    QCOMPARE(store.installationId(), installation); QCOMPARE(store.deviceId(), device);
    QCOMPARE(store.localMeta(QStringLiteral("signed_in")), QStringLiteral("0")); QCOMPARE(store.localMeta(QStringLiteral("sync_base_server_revision")), QStringLiteral("0"));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM desktop_notes")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), 2); // 到期凭证在覆盖事务内重建，不来自备份
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sync_bindings")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tombstones")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM outbox")).toInt(), 0);
    QVERIFY(store.importBackup(backupPath())); QCOMPARE(store.entities().size(), 2); // 重复覆盖不增长、不重写 UUID
    for (const QString &table : businessTables) QCOMPARE(queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)), beforeTables.value(table));

    const QString legacy = m_directory->filePath(QStringLiteral("legacy.klb"));
    QVERIFY(writeBytes(legacy, QJsonDocument(root).toJson())); QVERIFY(!store.importBackup(legacy)); QCOMPARE(store.entities().size(), 2);
    QByteArray oldMagic = bytes; oldMagic.replace(0, 4, QByteArray("KNBK", 4)); QVERIFY(writeBytes(legacy, oldMagic));
    QVERIFY(!store.importBackup(legacy)); QCOMPARE(store.entities().size(), 2);
    QJsonObject oldMetadata = root; oldMetadata.insert(QStringLiteral("format"), QStringLiteral("KylinList.Tasks")); oldMetadata.insert(QStringLiteral("version"), 1);
    QVERIFY(writeContainer(legacy, oldMetadata, frames)); QVERIFY(!store.importBackup(legacy)); QCOMPARE(store.entities().size(), 2);
    oldMetadata = root; QJsonArray oldTasks = oldMetadata.value(QStringLiteral("entities")).toArray();
    QJsonObject oldTask = oldTasks.first().toObject(); oldTask.remove(QStringLiteral("kind"));
    oldTask.insert(QStringLiteral("favorite"), true); oldTask.insert(QStringLiteral("contentFormat"), QStringLiteral("markdown"));
    oldTasks.replace(0, oldTask); oldMetadata.insert(QStringLiteral("entities"), oldTasks);
    QVERIFY(writeContainer(legacy, oldMetadata, frames)); QVERIFY(!store.importBackup(legacy)); QCOMPARE(store.entities().size(), 2);
    oldMetadata = root; QJsonArray oldLists = oldMetadata.value(QStringLiteral("lists")).toArray();
    QJsonObject oldList = oldLists.first().toObject(); oldList.remove(QStringLiteral("kind"));
    oldLists.replace(0, oldList); oldMetadata.insert(QStringLiteral("lists"), oldLists);
    QVERIFY(writeContainer(legacy, oldMetadata, frames)); QVERIFY(!store.importBackup(legacy)); QCOMPARE(store.entities().size(), 2);
    // 不兼容过去或未来的元数据版本：拒绝后业务行、附件和模型都保持不变。
    QSignalSpy rejectedReset(&store, &TaskStore::tasksReset);
    for (int version : {2, 3, 4, 5, backupcodec::kVersion + 1}) {
        oldMetadata = root; oldMetadata.insert(QStringLiteral("version"), version);
        QVERIFY(writeContainer(legacy, oldMetadata, frames));
        QVERIFY(!store.importBackup(legacy));
        QVERIFY2(store.lastError().contains(QStringLiteral("v%1").arg(backupcodec::kVersion)), qPrintable(store.lastError()));
        QCOMPARE(store.entities().size(), 2);
        QCOMPARE(store.entity(id).body, task.body);
        for (const QString &table : businessTables)
            QCOMPARE(queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)), beforeTables.value(table));
        QCOMPARE(readBytes(attachmentPath(attachment.relativePath)), content);
        QCOMPARE(attachmentFileCount(), 1);
    }
    QCOMPARE(rejectedReset.count(), 0);
    TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load()); QCOMPARE(reloaded.attachments(id).size(), 1);
}

void TaskTests::backupValidationAndSqlRollbackAreAtomic()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("原任务")); const QString source = m_directory->filePath(QStringLiteral("a.txt"));
    const QByteArray originalBytes("attachment"); QVERIFY(writeBytes(source, originalBytes));
    const Attachment original = store.importAttachment(id, source); QVERIFY(!original.id.isEmpty());
    QVERIFY(store.exportBackup(backupPath())); QVector<attachmentcodec::BackupFrame> frames; const QJsonObject valid = readMetadata(backupPath(), &frames);
    const QByteArray validContainer = readBytes(backupPath());
    QVERIFY(writeBytes(attachmentPath(original.relativePath), QByteArray("Xttachment")));
    QVERIFY(!store.exportBackup(backupPath())); QCOMPARE(readBytes(backupPath()), validContainer); // QSaveFile 失败不破坏既有备份
    QVERIFY(writeBytes(attachmentPath(original.relativePath), originalBytes));
    const QString invalid = m_directory->filePath(QStringLiteral("invalid.klb"));
    QJsonArray attachments = valid.value(QStringLiteral("attachments")).toArray(); QJsonObject badAttachment = attachments.first().toObject();
    badAttachment.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0'))); attachments.replace(0, badAttachment);
    QJsonObject bad = valid; bad.insert(QStringLiteral("attachments"), attachments); QVERIFY(writeContainer(invalid, bad, frames));
    const int filesBefore = attachmentFileCount(); QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY(!store.importBackup(invalid)); QCOMPARE(store.entities().size(), 1); QCOMPARE(attachmentFileCount(), filesBefore);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), originalBytes);
    bad = valid; QJsonArray tasks = bad.value(QStringLiteral("entities")).toArray(); QJsonObject badTask = tasks.first().toObject();
    badTask.insert(QStringLiteral("priority"), 4); tasks.replace(0, badTask); bad.insert(QStringLiteral("entities"), tasks);
    QVERIFY(writeContainer(invalid, bad, frames)); QVERIFY(!store.importBackup(invalid)); QCOMPARE(store.entities().size(), 1);
    QByteArray truncated = readBytes(backupPath()).left(20); QVERIFY(writeBytes(invalid, truncated)); QVERIFY(!store.importBackup(invalid));
    QByteArray trailing = readBytes(backupPath()); trailing.append("trailing"); QVERIFY(writeBytes(invalid, trailing)); QVERIFY(!store.importBackup(invalid));
    QVector<attachmentcodec::BackupFrame> duplicateFrames = frames; duplicateFrames.append(frames.first());
    QVERIFY(writeContainer(invalid, valid, duplicateFrames)); QVERIFY(!store.importBackup(invalid));
    QCOMPARE(reset.count(), 0); QCOMPARE(readBytes(attachmentPath(original.relativePath)), originalBytes);

    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_import BEFORE INSERT ON entities WHEN NEW.title='原任务' BEGIN SELECT RAISE(ABORT,'import failure'); END")));
    QVERIFY(!store.importBackup(backupPath())); QCOMPARE(store.entities().size(), 1); QCOMPARE(attachmentFileCount(), filesBefore);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), originalBytes);
    QCOMPARE(reset.count(), 0); QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_import")));

    const QString localSource = m_directory->filePath(QStringLiteral("local-only.txt")); QVERIFY(writeBytes(localSource, QByteArray("local-only")));
    const Attachment localOnly = store.importAttachment(id, localSource); QVERIFY(!localOnly.id.isEmpty());
    QCOMPARE(attachmentFileCount(), 2);
    const QString replacementId = newUuid();
    const QString replacementPath = attachmentcodec::attachmentRelativePath(replacementId, QStringLiteral("txt"));
    bad = valid; attachments = bad.value(QStringLiteral("attachments")).toArray(); badAttachment = attachments.first().toObject();
    badAttachment.insert(QStringLiteral("id"), replacementId); badAttachment.insert(QStringLiteral("relativePath"), replacementPath);
    attachments.replace(0, badAttachment); bad.insert(QStringLiteral("attachments"), attachments);
    QVector<attachmentcodec::BackupFrame> replacementFrames = frames; replacementFrames[0].id = replacementId;
    QVERIFY(writeContainer(invalid, bad, replacementFrames));
    QVERIFY(QDir().mkpath(attachmentPath(replacementPath))); // 最终路径被目录占用，旧文件已移动后必须反向恢复
    QVERIFY(!store.importBackup(invalid)); QCOMPARE(store.entities().size(), 1);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), originalBytes);
    QCOMPARE(readBytes(attachmentPath(localOnly.relativePath)), QByteArray("local-only"));
    QVERIFY(QDir().rmdir(attachmentPath(replacementPath)));
    QCOMPARE(reset.count(), 0);
    const QStringList restoreLeftovers = QDir(m_directory->path()).entryList(QStringList{QStringLiteral(".restore-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(restoreLeftovers.size(), 0);

    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError())); QCOMPARE(store.entities().size(), 1);
    QCOMPARE(store.attachments(id).size(), 1); QCOMPARE(store.attachments(id).first().id, original.id);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), originalBytes);
    QVERIFY(!QFile::exists(attachmentPath(localOnly.relativePath))); // 不在备份中的本机旧受管附件随覆盖清理
    QVERIFY(reset.count() > 0);
}

void TaskTests::backupContainerLimitsAndStreaming()
{
    QString error; qint64 containerSize = 0;
    QVERIFY2(attachmentcodec::validateBackupSizes(1, attachmentcodec::kMaximumBackupContainerBytes - 16 - 1 - 44,
                                                 1, &containerSize, &error), qPrintable(error));
    QCOMPARE(containerSize, attachmentcodec::kMaximumBackupContainerBytes);
    QVERIFY(!attachmentcodec::validateBackupSizes(1, attachmentcodec::kMaximumBackupContainerBytes - 16 - 1 - 44 + 1,
                                                  1, nullptr, &error));
    QVERIFY(attachmentcodec::validateBackupSizes(attachmentcodec::kMaximumBackupMetadataBytes, 0, 0, nullptr, &error));
    QVERIFY(!attachmentcodec::validateBackupSizes(attachmentcodec::kMaximumBackupMetadataBytes + 1, 0, 0, nullptr, &error));

    QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(attachmentcodec::writeU32(&buffer, attachmentcodec::kBackupContainerMagic, &error));
    QVERIFY(attachmentcodec::writeU32(&buffer, attachmentcodec::kBackupContainerVersion, &error));
    QVERIFY(attachmentcodec::writeU32(&buffer, static_cast<quint32>(attachmentcodec::kMaximumBackupMetadataBytes + 1), &error));
    buffer.close(); QVERIFY(buffer.open(QIODevice::ReadOnly)); QByteArray metadata; QVector<attachmentcodec::BackupFrameInfo> infos;
    QVERIFY(!attachmentcodec::readBackupContainer(&buffer, &metadata, &infos, &error)); // 仅头部即拒绝，不分配 256 MiB

    const QString frameId = newUuid(); const QVector<attachmentcodec::BackupFrame> frames{{frameId, QByteArray("streamed")}};
    QVERIFY(writeContainer(backupPath(), QJsonObject{{QStringLiteral("format"), QStringLiteral("KylinList.Entities")}}, frames));
    QFile file(backupPath()); QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY2(attachmentcodec::readBackupContainer(&file, &metadata, &infos, &error), qPrintable(error));
    QCOMPARE(infos.size(), 1); QCOMPARE(infos.first().id, frameId); QCOMPARE(infos.first().size, qint64(8));
    QVERIFY(file.seek(infos.first().offset)); QCOMPARE(file.read(infos.first().size), QByteArray("streamed"));
    QByteArray copied; QBuffer destination(&copied); QVERIFY(destination.open(QIODevice::WriteOnly));
    QVERIFY(file.seek(infos.first().offset)); QCryptographicHash hash(QCryptographicHash::Sha256);
    QVERIFY(attachmentcodec::copyBytes(&file, &destination, infos.first().size, &hash, &error));
    QCOMPARE(copied, QByteArray("streamed"));
    QCOMPARE(QString::fromLatin1(hash.result().toHex()), AttachmentStore::sha256(QByteArray("streamed")));
    QVERIFY(!attachmentcodec::writeBackupFrameHeader(&destination, newUuid(), attachmentcodec::kMaximumAttachmentBytes + 1, &error));
}

void TaskTests::backupRestoredReminderDeliveryPolicy()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDateTime now = QDateTime::currentDateTimeUtc(); const QString id = store.createTask(QStringLiteral("恢复提醒策略"));
    const TaskReminder past = store.addReminder(id, now.addSecs(-3600)); const TaskReminder future = store.addReminder(id, now.addSecs(3600));
    QVERIFY(!past.id.isEmpty() && !future.id.isEmpty());
    const QString completed = store.createTask(QStringLiteral("已完成历史提醒")); const TaskReminder completedPast = store.addReminder(completed, now.addSecs(-7200));
    QVERIFY(store.setCompleted(completed, true));
    const QString abandoned = store.createTask(QStringLiteral("已放弃历史提醒")); const TaskReminder abandonedPast = store.addReminder(abandoned, now.addSecs(-7200));
    QVERIFY(store.abandonTask(abandoned));
    const QString trashed = store.createTask(QStringLiteral("回收站历史提醒")); const TaskReminder trashedPast = store.addReminder(trashed, now.addSecs(-7200));
    QVERIFY(store.trashTask(trashed));
    const QString note = store.createTask(QStringLiteral("笔记历史提醒")); const TaskReminder notePast = store.addReminder(note, now.addSecs(-7200));
    QVERIFY(store.convertKind(note, EntityKind::Note));
    const QVector<QString> pastIds{past.id, completedPast.id, abandonedPast.id, trashedPast.id, notePast.id};
    QVERIFY(store.exportBackup(backupPath())); QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), pastIds.size());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(DISTINCT delivered_at_utc_ms) FROM reminder_deliveries")).toInt(), 1);
    for (const QString &reminderId : pastIds) QVERIFY(store.isReminderDelivered(reminderId));
    QVERIFY(!store.isReminderDelivered(future.id)); // 未来提醒不预写
    ReminderScheduler scheduler(&store); QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    scheduler.checkDue(now); QCOMPARE(triggered.count(), 0);
    QVERIFY(store.restoreTask(trashed)); QVERIFY(store.setStatus(abandoned, TaskStatus::Active));
    QVERIFY(store.setCompleted(completed, false)); QVERIFY(store.convertKind(note, EntityKind::Task));
    scheduler.checkDue(now); QCOMPARE(triggered.count(), 0); // 恢复/转回后仍不补弹导入前历史
    scheduler.checkDue(now.addSecs(10800)); QCOMPARE(triggered.count(), 1);
    QCOMPARE(triggered.takeFirst().at(1).toString(), future.id); QVERIFY(store.isReminderDelivered(future.id));
}

void TaskTests::backupDeliveryFailureRestoresEverything()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("delivery 回滚原任务"));
    const TaskReminder past = store.addReminder(id, QDateTime::currentDateTimeUtc().addSecs(-60)); QVERIFY(!past.id.isEmpty());
    const QString source = m_directory->filePath(QStringLiteral("delivery-original.txt"));
    QVERIFY(writeBytes(source, QByteArray("delivery-original")));
    const Attachment original = store.importAttachment(id, source); QVERIFY(!original.id.isEmpty());
    QVERIFY(store.exportBackup(backupPath()));
    const QString sentinel = store.createTask(QStringLiteral("失败后必须保留的哨兵")); QVERIFY(!sentinel.isEmpty());
    const QString localSource = m_directory->filePath(QStringLiteral("delivery-local.txt")); QVERIFY(writeBytes(localSource, QByteArray("delivery-local")));
    const Attachment localOnly = store.importAttachment(id, localSource); QVERIFY(!localOnly.id.isEmpty());
    const QStringList tables{QStringLiteral("folders"),QStringLiteral("lists"),QStringLiteral("sections"),QStringLiteral("entities"),
        QStringLiteral("tags"),QStringLiteral("entity_tags"),QStringLiteral("reminders"),
        QStringLiteral("recurrence_links"),QStringLiteral("attachments"),QStringLiteral("entity_events")};
    QHash<QString, QList<QVariantList>> before;
    for (const QString &table : tables) before.insert(table, queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)));
    const QList<QVariantList> beforeMeta = queryRows(QStringLiteral("SELECT * FROM local_meta ORDER BY key"));
    const QList<QVariantList> beforeDeliveries = queryRows(QStringLiteral("SELECT * FROM reminder_deliveries ORDER BY reminder_id,installation_id"));
    QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_restore_delivery BEFORE INSERT ON reminder_deliveries "
        "BEGIN SELECT RAISE(ABORT,'restore delivery rejected'); END")));
    QVERIFY(!store.importBackup(backupPath())); QVERIFY(!store.lastError().isEmpty());
    for (const QString &table : tables) QCOMPARE(queryRows(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table)), before.value(table));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM local_meta ORDER BY key")), beforeMeta);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminder_deliveries ORDER BY reminder_id,installation_id")), beforeDeliveries);
    QVERIFY(!store.entity(sentinel).id.isEmpty()); QCOMPARE(reset.count(), 0);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), QByteArray("delivery-original"));
    QCOMPARE(readBytes(attachmentPath(localOnly.relativePath)), QByteArray("delivery-local"));
    QCOMPARE(QDir(m_directory->path()).entryList(QStringList{QStringLiteral(".restore-*")}, QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_restore_delivery")));
    QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QVERIFY(store.entity(sentinel).id.isEmpty()); QVERIFY(!QFile::exists(attachmentPath(localOnly.relativePath)));
    QCOMPARE(store.attachments(id).size(), 1); QCOMPARE(store.attachments(id).first().id, original.id);
    QCOMPARE(readBytes(attachmentPath(original.relativePath)), QByteArray("delivery-original"));
    QVERIFY(store.isReminderDelivered(past.id));
}

void TaskTests::backupRecurrenceUndoProtection()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString source = store.createTask(QStringLiteral("备份重复"));
    Entity task = store.entity(source); task.dueAt = utc(2026, 1, 31); task.recurrence = QStringLiteral("monthly");
    QVERIFY(store.saveTask(task)); QVERIFY(store.setCompleted(source, true));
    QString next;
    for (const Entity &candidate : store.entities()) if (candidate.id != source) next = candidate.id;
    Entity edited = store.entity(next); edited.body = QStringLiteral("下一实例已编辑"); QVERIFY(store.saveTask(edited));
    QVERIFY(store.exportBackup(backupPath())); QVERIFY2(store.importBackup(backupPath()), qPrintable(store.lastError()));
    QVERIFY(!store.setCompleted(source, false)); // 原 UUID、下一实例 revision 与凭证原样恢复
    QVERIFY(store.entity(source).completed); QCOMPARE(store.entities().size(), 2);
}

void TaskTests::outboxDoesNotAccumulateWhileLoggedOut()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString offline = store.createTask(QStringLiteral("离线")); QVERIFY(!offline.isEmpty()); QCOMPARE(store.outbox().size(), 0);
    QVERIFY(store.trashTask(offline)); QVERIFY(store.deletePermanently(offline)); QCOMPARE(store.outbox().size(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tombstones")).toInt(), 0);
    QVERIFY(store.setLocalMeta(QStringLiteral("signed_in"), QStringLiteral("1")));
    const QString online = store.createTask(QStringLiteral("在线变更")); QVERIFY(!online.isEmpty()); QCOMPARE(store.outbox().size(), 1);
    QVERIFY(isUuid(store.outbox().first().batchId)); QCOMPARE(store.outbox().first().baseServerRevision, 0);
    QVERIFY(store.saveDesktopState(online, NoteColor::Blue, QRect(1, 2, 300, 200), 0, NoteLayer::Normal, true));
    QCOMPARE(store.outbox().size(), 1); QVERIFY(store.desktopNote(online).open); // desktop state 永不是同步业务事实
    Entity recurring = store.entity(online); recurring.dueAt = utc(2026, 6, 1); recurring.recurrence = QStringLiteral("daily");
    recurring.reminderAt = recurring.dueAt.addSecs(-60); QVERIFY(store.saveTask(recurring));
    const int beforeBatch = store.outbox().size();
    QSet<QString> priorOutboxIds;
    for (const OutboxItem &item : store.outbox()) priorOutboxIds.insert(item.id);
    QVERIFY(store.setCompleted(online, true));
    QVERIFY(store.outbox().size() - beforeBatch >= 4); // 两任务 + 新提醒 + recurrence link
    QString batch;
    int addedToBatch = 0;
    for (const OutboxItem &item : store.outbox()) {
        if (priorOutboxIds.contains(item.id)) continue;
        if (batch.isEmpty()) batch = item.batchId;
        QCOMPARE(item.batchId, batch); QCOMPARE(item.baseServerRevision, 0);
        ++addedToBatch;
    }
    QVERIFY(isUuid(batch)); QCOMPARE(addedToBatch, store.outbox().size() - beforeBatch);
    QVERIFY(store.trashTask(online)); QVERIFY(store.deletePermanently(online));
    QVERIFY(store.outbox().size() > beforeBatch); QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tombstones")).toInt(), 1);
    const int count = store.outbox().size(); QVERIFY(store.setLocalMeta(QStringLiteral("signed_in"), QStringLiteral("0")));
    QVERIFY(!store.createTask(QStringLiteral("再次离线")).isEmpty()); QCOMPARE(store.outbox().size(), count);
    QVERIFY(execute(QStringLiteral("INSERT INTO sync_bindings(id,entity_type,entity_id,remote_id,updated_at_utc_ms) VALUES('%1','task','%2','remote-1',1)")
                        .arg(newUuid(), store.entities().first().id)));
    QVERIFY(store.load()); QCOMPARE(store.outbox().size(), count);
}

void TaskTests::appTimeZoneHelpers()
{
    QCOMPARE(appTimeZone().id(), QByteArray("Asia/Shanghai"));
    const QDateTime wall(QDate(2026, 7, 1), QTime(8, 30, 15, 123), Qt::LocalTime);
    const QDateTime absolute = fromAppTime(wall);
    QCOMPARE(absolute, utc(2026, 7, 1, 0, 30).addSecs(15).addMSecs(123));
    QCOMPARE(toAppTime(absolute).date(), wall.date()); QCOMPARE(toAppTime(absolute).time(), wall.time());
    QVERIFY(appNow().isValid()); QCOMPARE(appNow().timeSpec(), Qt::UTC);
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv); TaskTests tests; return QTest::qExec(&tests, argc, argv);
}

#include "tst_tasks.moc"
