#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "db/TaskRepository.h"
#include "domain/AttachmentCodec.h"
#include "domain/BackupCodec.h"
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
// 与 v5 发布版一致的完整 schema（不含 v6 的 lists.icon/color/archived_at_utc_ms）。
QStringList v5SchemaStatements(const QString &inboxId, const QString &now)
{
    QStringList statements;
    const char *const ddl[] = {
        "CREATE TABLE folders("
        "id TEXT PRIMARY KEY,name TEXT NOT NULL CHECK(length(trim(name))>0),"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0))",
        "CREATE TABLE lists("
        "id TEXT PRIMARY KEY,folder_id TEXT,kind TEXT NOT NULL DEFAULT 'task' CHECK(kind IN ('task','note')),"
        "name TEXT NOT NULL CHECK(length(trim(name))>0),is_inbox INTEGER NOT NULL DEFAULT 0 CHECK(is_inbox IN (0,1)),"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(folder_id) REFERENCES folders(id) ON DELETE RESTRICT)",
        "CREATE TABLE sections("
        "id TEXT PRIMARY KEY,list_id TEXT NOT NULL,name TEXT NOT NULL CHECK(length(trim(name))>0),"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(list_id) REFERENCES lists(id) ON DELETE CASCADE)",
        "CREATE TABLE recurrence_series("
        "id TEXT PRIMARY KEY,rrule TEXT NOT NULL CHECK(rrule LIKE 'FREQ=%'),anchor_day INTEGER NOT NULL DEFAULT 0 CHECK(anchor_day BETWEEN 0 AND 31),"
        "timezone TEXT NOT NULL DEFAULT 'Asia/Shanghai',dtstart_utc_ms INTEGER,dtstart_date TEXT,"
        "created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        "server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0))",
        "CREATE TABLE entities("
        "id TEXT PRIMARY KEY,kind TEXT NOT NULL CHECK(kind IN ('task','note')),"
        "title TEXT NOT NULL CHECK(length(trim(title))>0),body_markdown TEXT NOT NULL DEFAULT '',"
        "folder_id TEXT,list_id TEXT NOT NULL,section_id TEXT,parent_id TEXT,"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),"
        "status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','completed','abandoned')),"
        "deleted_at_utc_ms INTEGER,start_at_utc_ms INTEGER,due_at_utc_ms INTEGER,all_day_date TEXT,"
        "timezone TEXT NOT NULL DEFAULT 'Asia/Shanghai',recurrence_series_id TEXT,occurrence_key TEXT,"
        "priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 3),pinned_at_utc_ms INTEGER,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,completed_at_utc_ms INTEGER,abandoned_at_utc_ms INTEGER,"
        "UNIQUE(id,kind),CHECK(kind='task' OR parent_id IS NULL),CHECK(parent_id IS NULL OR parent_id<>id),"
        "CHECK((status='completed')=(completed_at_utc_ms IS NOT NULL)),"
        "CHECK((status='abandoned')=(abandoned_at_utc_ms IS NOT NULL)),"
        "CHECK((recurrence_series_id IS NULL)=(occurrence_key IS NULL)),UNIQUE(recurrence_series_id,occurrence_key),"
        "FOREIGN KEY(folder_id) REFERENCES folders(id) ON DELETE RESTRICT,"
        "FOREIGN KEY(list_id) REFERENCES lists(id) ON DELETE RESTRICT,"
        "FOREIGN KEY(section_id) REFERENCES sections(id) ON DELETE RESTRICT,"
        "FOREIGN KEY(parent_id,kind) REFERENCES entities(id,kind) ON DELETE CASCADE,"
        "FOREIGN KEY(recurrence_series_id) REFERENCES recurrence_series(id) ON DELETE RESTRICT)",
        "CREATE TABLE recurrence_links("
        "id TEXT PRIMARY KEY,source_id TEXT NOT NULL UNIQUE REFERENCES entities(id) ON DELETE CASCADE,"
        "next_id TEXT UNIQUE REFERENCES entities(id) ON DELETE SET NULL,next_revision INTEGER NOT NULL CHECK(next_revision>0),"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0))",
        "CREATE TABLE checklist_items("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL,text TEXT NOT NULL DEFAULT '',done INTEGER NOT NULL DEFAULT 0 CHECK(done IN (0,1)),"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(entity_id) REFERENCES entities(id) ON DELETE CASCADE)",
        "CREATE TABLE tags("
        "id TEXT PRIMARY KEY,name TEXT NOT NULL CHECK(length(trim(name))>0),normalized_name TEXT NOT NULL,color TEXT NOT NULL DEFAULT '',"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "UNIQUE(normalized_name))",
        "CREATE TABLE entity_tags("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL,tag_id TEXT NOT NULL,revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        "server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),UNIQUE(entity_id,tag_id),"
        "FOREIGN KEY(entity_id) REFERENCES entities(id) ON DELETE CASCADE,"
        "FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE)",
        "CREATE TABLE reminders("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL,trigger_at_utc_ms INTEGER NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),"
        "created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),"
        "server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(entity_id) REFERENCES entities(id) ON DELETE CASCADE)",
        "CREATE TABLE reminder_deliveries("
        "reminder_id TEXT NOT NULL REFERENCES reminders(id) ON DELETE CASCADE,installation_id TEXT NOT NULL,"
        "scheduled_at_utc_ms INTEGER NOT NULL,delivered_at_utc_ms INTEGER NOT NULL,PRIMARY KEY(reminder_id,installation_id))",
        "CREATE TABLE attachments("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL,original_name TEXT NOT NULL,relative_path TEXT NOT NULL UNIQUE,mime TEXT NOT NULL,"
        "size INTEGER NOT NULL CHECK(size>0 AND size<=2097152),sha256 TEXT NOT NULL CHECK(length(sha256)=64),"
        "is_image INTEGER NOT NULL CHECK(is_image IN (0,1)),local_state TEXT NOT NULL DEFAULT 'available' CHECK(local_state IN ('available','missing','corrupt')),"
        "created_at_utc_ms INTEGER NOT NULL,verified_at_utc_ms INTEGER,sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(entity_id) REFERENCES entities(id) ON DELETE CASCADE)",
        "CREATE TABLE desktop_notes("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,device_id TEXT NOT NULL,"
        "color INTEGER NOT NULL DEFAULT 0 CHECK(color BETWEEN 0 AND 5),geometry TEXT NOT NULL DEFAULT '',z_order INTEGER NOT NULL DEFAULT 0,"
        "layer INTEGER NOT NULL DEFAULT 1 CHECK(layer BETWEEN 0 AND 2),open INTEGER NOT NULL DEFAULT 0 CHECK(open IN (0,1)),"
        "created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,UNIQUE(entity_id,device_id))",
        "CREATE TABLE sync_bindings("
        "id TEXT PRIMARY KEY,entity_type TEXT NOT NULL,entity_id TEXT NOT NULL,remote_id TEXT NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "UNIQUE(entity_type,entity_id),UNIQUE(entity_type,remote_id))",
        "CREATE TABLE view_defs("
        "id TEXT PRIMARY KEY,name TEXT NOT NULL CHECK(length(trim(name))>0),spec_json TEXT NOT NULL,"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0))",
        "CREATE TABLE local_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)",
        "CREATE TABLE tombstones("
        "id TEXT PRIMARY KEY,entity_type TEXT NOT NULL,entity_id TEXT NOT NULL,deleted_at_utc_ms INTEGER NOT NULL,"
        "UNIQUE(entity_type,entity_id))",
        "CREATE TABLE outbox("
        "id TEXT PRIMARY KEY,batch_id TEXT NOT NULL,base_server_revision INTEGER NOT NULL DEFAULT 0 CHECK(base_server_revision>=0),"
        "entity_type TEXT NOT NULL,entity_id TEXT NOT NULL,operation TEXT NOT NULL CHECK(operation IN ('upsert','delete')),"
        "payload_json TEXT NOT NULL DEFAULT '{}',created_at_utc_ms INTEGER NOT NULL)",
        "CREATE TABLE entity_events("
        "id TEXT PRIMARY KEY,entity_id TEXT NOT NULL,entity_kind TEXT NOT NULL CHECK(entity_kind IN ('task','note')),"
        "entity_title TEXT NOT NULL,batch_id TEXT NOT NULL,"
        "type TEXT NOT NULL CHECK(type IN ('created','changed','kind_changed','trashed','restored','deleted',"
        "'attachment_added','attachment_removed','reminder_added','reminder_removed')),"
        "field TEXT NOT NULL DEFAULT '',old_value TEXT,new_value TEXT,occurred_at_utc_ms INTEGER NOT NULL)",
        "CREATE UNIQUE INDEX idx_one_inbox ON lists(is_inbox) WHERE is_inbox=1",
        "CREATE TRIGGER prevent_inbox_delete BEFORE DELETE ON lists WHEN OLD.is_inbox=1 BEGIN SELECT RAISE(ABORT,'inbox is permanent'); END",
        "CREATE TRIGGER prevent_inbox_unset BEFORE UPDATE OF is_inbox ON lists WHEN OLD.is_inbox=1 AND NEW.is_inbox<>1 BEGIN SELECT RAISE(ABORT,'inbox is permanent'); END",
        "CREATE INDEX idx_lists_folder ON lists(folder_id,sort_order)",
        "CREATE INDEX idx_sections_list ON sections(list_id,sort_order)",
        "CREATE INDEX idx_entities_list ON entities(list_id,section_id)",
        "CREATE INDEX idx_entities_parent ON entities(parent_id)",
        "CREATE INDEX idx_entities_due ON entities(kind,status,due_at_utc_ms)",
        "CREATE INDEX idx_entities_trash ON entities(deleted_at_utc_ms)",
        "CREATE INDEX idx_entities_pinned ON entities(pinned_at_utc_ms) WHERE pinned_at_utc_ms IS NOT NULL",
        "CREATE UNIQUE INDEX idx_entity_occurrence ON entities(recurrence_series_id,occurrence_key) WHERE recurrence_series_id IS NOT NULL",
        "CREATE INDEX idx_checklist_entity ON checklist_items(entity_id,sort_order)",
        "CREATE INDEX idx_reminders_due ON reminders(trigger_at_utc_ms,entity_id)",
        "CREATE INDEX idx_deliveries_install ON reminder_deliveries(installation_id,scheduled_at_utc_ms)",
        "CREATE INDEX idx_attachments_entity ON attachments(entity_id,sort_order)",
        "CREATE INDEX idx_desktop_device ON desktop_notes(device_id,entity_id)",
        "CREATE INDEX idx_view_defs_order ON view_defs(sort_order,id)",
        "CREATE INDEX idx_tombstones_entity ON tombstones(entity_type,entity_id)",
        "CREATE INDEX idx_outbox_batch ON outbox(batch_id,base_server_revision,id)",
        "CREATE INDEX idx_events_entity ON entity_events(entity_id,occurred_at_utc_ms)"
    };
    for (const char *sql : ddl) statements.append(QString::fromUtf8(sql));
    statements.append(QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1',NULL,'task','收集箱',1,0,%2,%2)").arg(inboxId, now));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('installation_id','%1')").arg(newUuid()));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('device_id','%1')").arg(newUuid()));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('signed_in','0')"));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('default_timezone','Asia/Shanghai')"));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('inbox_list_id','%1')").arg(inboxId));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('schema_created_at_utc_ms','%1')").arg(now));
    statements.append(QStringLiteral("INSERT INTO local_meta(key,value) VALUES('sync_base_server_revision','0')"));
    statements.append(QStringLiteral("PRAGMA user_version=5"));
    statements.append(QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId()));
    return statements;
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
    void freshSchemaNeverMigratesLegacy();
    void rejectsWrongIdentityAndFutureVersion();
    void migratesV5ToV7Atomically();
    void migratesV6ToV7Atomically();
    void completeModelAndUuidCrud();
    void inboxAndOrganizationIntegrity();
    void entityKindAndParentConstraints();
    void eventRepositoryTransactionsAndLazyReads();
    void convertKindPreservesFieldsAndAssociations();
    void convertKindRejectsRelationshipsAndRollsBack();
    void applicationFacadeKindAndPinnedAt();
    void journalEditSessionsCommitFullText();
    void journalScalarBusinessAndLocalOnlyEvents();
    void convertedNotesSuppressRemindersUntilRestored();
    void bodyMarkdownCheckboxesStayOutOfChecklist();
    void taskAutosaveValidationAndTransactions();
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
    void legacyAllZeroTopLevelOrder();
    void mixedTopLevelOrderAndCrossFolderMoves();
    void legacyLongListNamesRemainEditable_data();
    void legacyLongListNamesRemainEditable();
    void deleteListMovesContentToTrashAtomically();
    void viewSpecScopesAndArchivedFiltering();
    void archivedListsSuppressReminders();
    void postponeOverdueTasksShiftsDates();
    void postponePreservesDateSpansAndReminders();
    void taskDropSectionRules();
    void taskDropShiftsDatesAcrossSections();
    void taskDropReorderPinnedAndParentDetach();
    void taskDropSiblingReorderPreservesPinnedAt_data();
    void taskDropSiblingReorderPreservesPinnedAt();
    void taskDropReparentValidationAndPersistence();
    void taskDropListSectionValidatesMembership();
    void taskDropAtomicFailuresAndIdempotence();
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

void TaskTests::freshSchemaNeverMigratesLegacy()
{
    const QString legacy = m_directory->filePath(QStringLiteral("notes.db"));
    QVERIFY(seed(legacy, {QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY,title TEXT,content TEXT)"),
                          QStringLiteral("PRAGMA user_version=3"),
                          QStringLiteral("INSERT INTO notes VALUES(7,'旧数据','<b>不迁移</b>')")}));
    QString error;
    QVERIFY2(Database::instance().open(&error), qPrintable(error));
    QCOMPARE(Database::instance().path(), dbPath());
    QCOMPARE(Database::schemaVersion(), 7);
    QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), 7);
    QCOMPARE(scalar(QStringLiteral("PRAGMA application_id")).toInt(), Database::applicationId());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities")).toInt(), 0);
    QCOMPARE(inspect(legacy, QStringLiteral("SELECT title FROM notes")).toString(), QStringLiteral("旧数据"));
    QCOMPARE(inspect(legacy, QStringLiteral("PRAGMA user_version")).toInt(), 3);
    const char *tables[] = {"folders","lists","sections","entities","checklist_items","tags","entity_tags",
        "recurrence_series","recurrence_links","reminders","reminder_deliveries","attachments","desktop_notes",
        "sync_bindings","view_defs","local_meta","tombstones","outbox","entity_events"};
    for (const char *table : tables)
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%1'").arg(QLatin1String(table))).toInt(), 1);
    const char *removedTables[] = {"workspaces","tasks","task_lists","task_tags","task_recurrence_links","saved_filters"};
    for (const char *table : removedTables)
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%1'").arg(QLatin1String(table))).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE sql LIKE '%workspace_id%' OR sql LIKE '%task_id%'")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('favorite','content_format','description','workspace_id')")).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('entities') WHERE name IN ('kind','body_markdown','sort_order','pinned_at_utc_ms')")).toInt(), 4);
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
        "checklist_items","tags","entity_tags","reminders","attachments","sync_bindings","view_defs"};
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
    QVERIFY(!Database::instance().openAt(v4, &error)); QVERIFY(error.contains(QStringLiteral("v5")));
    QCOMPARE(inspect(v4, QStringLiteral("PRAGMA user_version")).toInt(), 4);
    QCOMPARE(inspect(v4, QStringLiteral("SELECT id FROM tasks")).toString(), QStringLiteral("legacy"));
    const QString future = m_directory->filePath(QStringLiteral("future.db"));
    QVERIFY(seed(future, {QStringLiteral("CREATE TABLE future_data(id TEXT)"), QStringLiteral("PRAGMA user_version=8"),
                          QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId())}));
    QVERIFY(!Database::instance().openAt(future, &error));
    QVERIFY(error.contains(QStringLiteral("v7")));
    QCOMPARE(inspect(future, QStringLiteral("PRAGMA user_version")).toInt(), 8);
    QVERIFY(!Database::instance().openAt(QString(), &error));
    QVERIFY(open());
}

// 真实 v5 库升级 v7：字段默认、其他数据保留、失败回滚。
void TaskTests::migratesV5ToV7Atomically()
{
    const QString inboxId = newUuid();
    const QString folder = newUuid(), list = newUuid(), section = newUuid();
    const QString active = newUuid(), done = newUuid(), trashed = newUuid(), note = newUuid(), reminderId = newUuid();
    const qint64 now = 1770000000000LL, deletedAt = 1770000001000LL, completedAt = 1770000002000LL;
    const QString v5 = m_directory->filePath(QStringLiteral("v5.db"));
    QStringList statements = v5SchemaStatements(inboxId, QString::number(now));
    statements.append(QStringLiteral("INSERT INTO folders(id,name,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','v5 文件夹',%2,%2)").arg(folder).arg(now));
    statements.append(QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','%2','task','v5 清单',0,0,%3,%3)").arg(list, folder).arg(now));
    statements.append(QStringLiteral("INSERT INTO sections(id,list_id,name,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','%2','v5 分区',%3,%3)").arg(section, list).arg(now));
    statements.append(QStringLiteral("INSERT INTO entities(id,kind,title,list_id,section_id,folder_id,status,due_at_utc_ms,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','task','v5 活动','%2','%3','%4','active',%5,%6,%6)").arg(active, list, section, folder).arg(now + 60000).arg(now));
    statements.append(QStringLiteral("INSERT INTO entities(id,kind,title,list_id,status,completed_at_utc_ms,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','task','v5 完成','%2','completed',%3,%4,%4)").arg(done, list).arg(completedAt).arg(now));
    statements.append(QStringLiteral("INSERT INTO entities(id,kind,title,list_id,status,deleted_at_utc_ms,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','task','v5 回收站','%2','active',%3,%4,%4)").arg(trashed, list).arg(deletedAt).arg(now));
    statements.append(QStringLiteral("INSERT INTO entities(id,kind,title,list_id,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','note','v5 笔记','%2',%3,%3)").arg(note, list).arg(now));
    statements.append(QStringLiteral("INSERT INTO reminders(id,entity_id,trigger_at_utc_ms,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','%2',%3,%4,%4)").arg(reminderId, active).arg(now + 30000).arg(now));
    QVERIFY(seed(v5, statements));
    QString error;
    QVERIFY2(Database::instance().openAt(v5, &error), qPrintable(error));   // 打开即迁移
    QCOMPARE(inspect(v5, QStringLiteral("PRAGMA user_version")).toInt(), 7);
    QCOMPARE(inspect(v5, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name IN ('icon','color','archived_at_utc_ms')")).toInt(), 3);
    QCOMPARE(inspect(v5, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE name='icon'")).toInt(), 1);
    QCOMPARE(inspect(v5, QStringLiteral("SELECT icon FROM folders")).toString(), QString());
    QCOMPARE(inspect(v5, QStringLiteral("SELECT icon FROM lists")).toString(), QString());
    QCOMPARE(inspect(v5, QStringLiteral("SELECT color FROM lists")).toString(), QString());
    QCOMPARE(inspect(v5, QStringLiteral("SELECT archived_at_utc_ms FROM lists")).toLongLong(), qint64(0));
    QCOMPARE(inspect(v5, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name='prevent_inbox_archive'")).toInt(), 1);
    TaskStore store(Database::instance().connection()); QVERIFY2(store.load(), qPrintable(store.lastError()));
    QCOMPARE(store.lists().size(), 2); QCOMPARE(store.entities().size(), 4);
    QCOMPARE(store.entity(active).title, QStringLiteral("v5 活动"));
    QCOMPARE(store.entity(active).reminders.size(), 1);
    QCOMPARE(store.entity(done).status, TaskStatus::Completed);
    QVERIFY(store.entity(trashed).deleted);
    QCOMPARE(store.entity(note).kind, EntityKind::Note);
    QCOMPARE(store.inboxListId(), inboxId);
    QVERIFY(!store.setListArchived(inboxId, true));                        // 迁移后保护依旧有效
    Database::instance().close();

    // 迁移失败整体回滚：构造一个 lists 已带 color 列的畸形 v5，第一条 ALTER 成功、第二条失败。
    const QString broken = m_directory->filePath(QStringLiteral("broken-v5.db"));
    const QString brokenLists = QStringLiteral(
        "CREATE TABLE lists("
        "id TEXT PRIMARY KEY,folder_id TEXT,kind TEXT NOT NULL DEFAULT 'task' CHECK(kind IN ('task','note')),"
        "name TEXT NOT NULL CHECK(length(trim(name))>0),is_inbox INTEGER NOT NULL DEFAULT 0 CHECK(is_inbox IN (0,1)),"
        "color TEXT,"
        "sort_order INTEGER NOT NULL DEFAULT 0 CHECK(sort_order>=0),created_at_utc_ms INTEGER NOT NULL,updated_at_utc_ms INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),server_revision INTEGER NOT NULL DEFAULT 0 CHECK(server_revision>=0),"
        "FOREIGN KEY(folder_id) REFERENCES folders(id) ON DELETE RESTRICT)");
    QStringList rebuilt;
    rebuilt.append(QStringLiteral("CREATE TABLE folders(id TEXT PRIMARY KEY,name TEXT NOT NULL)"));
    rebuilt.append(brokenLists);
    rebuilt.append(QStringLiteral("CREATE TABLE local_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)"));
    rebuilt.append(QStringLiteral("INSERT INTO lists(id,kind,name,is_inbox,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','task','收集箱',1,1,1)").arg(inboxId));
    rebuilt.append(QStringLiteral("INSERT INTO local_meta VALUES('inbox_list_id','%1')").arg(inboxId));
    rebuilt.append(QStringLiteral("PRAGMA user_version=5"));
    rebuilt.append(QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId()));
    QVERIFY(seed(broken, rebuilt));
    QVERIFY(!Database::instance().openAt(broken, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(inspect(broken, QStringLiteral("PRAGMA user_version")).toInt(), 5);
    QCOMPARE(inspect(broken, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name='icon'")).toInt(), 0);
    QCOMPARE(inspect(broken, QStringLiteral("SELECT name FROM lists WHERE is_inbox=1")).toString(), QStringLiteral("收集箱"));
    QVERIFY(!Database::instance().isOpen());
}

// 真实 v6 库升级 v7：folders 补 icon、lists 外观与既有数据保留、失败回滚。
void TaskTests::migratesV6ToV7Atomically()
{
    const QString inboxId = newUuid();
    const QString folder = newUuid(), list = newUuid();
    const qint64 now = 1770000000000LL;
    const QString v6 = m_directory->filePath(QStringLiteral("v6.db"));
    QStringList statements = v5SchemaStatements(inboxId, QString::number(now));
    statements.removeAll(QStringLiteral("PRAGMA user_version=5"));
    statements.append(QStringLiteral("ALTER TABLE lists ADD COLUMN icon TEXT NOT NULL DEFAULT '' CHECK(length(icon)<=16)"));
    statements.append(QStringLiteral("ALTER TABLE lists ADD COLUMN color TEXT NOT NULL DEFAULT ''"));
    statements.append(QStringLiteral("ALTER TABLE lists ADD COLUMN archived_at_utc_ms INTEGER NOT NULL DEFAULT 0"));
    statements.append(QStringLiteral("CREATE TRIGGER prevent_inbox_archive BEFORE UPDATE OF archived_at_utc_ms ON lists "
                                     "WHEN OLD.is_inbox=1 AND NEW.archived_at_utc_ms<>0 BEGIN SELECT RAISE(ABORT,'inbox is permanent'); END"));
    statements.append(QStringLiteral("INSERT INTO folders(id,name,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','v6 文件夹',3,%2,%2)").arg(folder).arg(now));
    statements.append(QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,icon,color,sort_order,created_at_utc_ms,updated_at_utc_ms) "
                                     "VALUES('%1','%2','task','v6 清单',0,'🏠','#112233',4,%3,%3)").arg(list, folder).arg(now));
    statements.append(QStringLiteral("PRAGMA user_version=6"));
    QVERIFY(seed(v6, statements));
    QString error;
    QVERIFY2(Database::instance().openAt(v6, &error), qPrintable(error));   // 打开即迁移
    QCOMPARE(inspect(v6, QStringLiteral("PRAGMA user_version")).toInt(), 7);
    QCOMPARE(inspect(v6, QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('folders') WHERE name='icon'")).toInt(), 1);
    QCOMPARE(inspect(v6, QStringLiteral("SELECT icon FROM folders WHERE id='%1'").arg(folder)).toString(), QString());
    QCOMPARE(inspect(v6, QStringLiteral("SELECT icon FROM lists WHERE id='%1'").arg(list)).toString(), QStringLiteral("🏠"));
    QCOMPARE(inspect(v6, QStringLiteral("SELECT color FROM lists WHERE id='%1'").arg(list)).toString(), QStringLiteral("#112233"));
    QCOMPARE(inspect(v6, QStringLiteral("SELECT sort_order FROM lists WHERE id='%1'").arg(list)).toInt(), 4);
    TaskStore store(Database::instance().connection()); QVERIFY2(store.load(), qPrintable(store.lastError()));
    Folder loaded;
    for (const Folder &candidate : store.folders()) if (candidate.id == folder) loaded = candidate;
    QCOMPARE(loaded.name, QStringLiteral("v6 文件夹")); QVERIFY(loaded.icon.isEmpty()); QCOMPARE(loaded.sortOrder, 3);
    Database::instance().close();

    // 迁移失败整体回滚：畸形 v6（folders 已带 icon 列），ALTER 重复列失败。
    const QString broken = m_directory->filePath(QStringLiteral("broken-v6.db"));
    QStringList rebuilt;
    rebuilt.append(QStringLiteral("CREATE TABLE folders(id TEXT PRIMARY KEY,name TEXT NOT NULL,icon TEXT NOT NULL DEFAULT '')"));
    rebuilt.append(QStringLiteral("CREATE TABLE local_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL)"));
    rebuilt.append(QStringLiteral("INSERT INTO local_meta VALUES('inbox_list_id','%1')").arg(inboxId));
    rebuilt.append(QStringLiteral("PRAGMA user_version=6"));
    rebuilt.append(QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId()));
    QVERIFY(seed(broken, rebuilt));
    QVERIFY(!Database::instance().openAt(broken, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(inspect(broken, QStringLiteral("PRAGMA user_version")).toInt(), 6);
    QVERIFY(!Database::instance().isOpen());
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
    value.allDayDate = QDate(2026, 4, 2); value.timeZone = QStringLiteral("Asia/Shanghai");
    value.tags = QStringList{QStringLiteral("工作"), QStringLiteral("Qt")};
    QVERIFY2(store.saveTask(value), qPrintable(store.lastError()));
    QCOMPARE(store.entity(parent).priority, 3); QCOMPARE(store.entity(parent).body, value.body);
    QCOMPARE(store.entity(parent).sortOrder, 42); QCOMPARE(store.entity(parent).pinnedAt, value.pinnedAt);
    QCOMPARE(store.entity(parent).startAt, value.startAt); QCOMPARE(store.entity(parent).allDayDate, QDate(2026, 4, 2));
    QCOMPARE(scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(parent)).toString(), value.body);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM entities WHERE id='%1'").arg(parent)).toLongLong(), qint64(42));
    QCOMPARE(scalar(QStringLiteral("SELECT pinned_at_utc_ms FROM entities WHERE id='%1'").arg(parent)).toLongLong(), utcMs(value.pinnedAt));
    QVERIFY(scalar(QStringLiteral("SELECT start_at_utc_ms FROM entities WHERE id='%1'").arg(parent)).toString().toLongLong() == utcMs(value.startAt));
    QCOMPARE(scalar(QStringLiteral("SELECT all_day_date FROM entities WHERE id='%1'").arg(parent)).toString(), QStringLiteral("2026-04-02"));
    QCOMPARE(scalar(QStringLiteral("SELECT timezone FROM entities WHERE id='%1'").arg(parent)).toString(), QStringLiteral("Asia/Shanghai"));
    const QString item = store.addChecklistItem(parent, QStringLiteral("独立步骤")); QVERIFY(isUuid(item));
    QVERIFY(store.setChecklistDone(item, true)); QVERIFY(store.entity(parent).checklist.first().done);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM checklist_items WHERE entity_id='%1'").arg(parent)).toInt(), 1);
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
    task.allDayDate = QDate(2026, 8, 31); task.timeZone = QStringLiteral("Asia/Shanghai"); task.recurrence = QStringLiteral("monthly");
    task.tags = QStringList{QStringLiteral("工作"), QStringLiteral("转换")};
    ChecklistItem item; item.text = QStringLiteral("保留清单项"); item.done = true; task.checklist.append(item);
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
    QCOMPARE(originalStored.entity.checklist.size(), 1); QCOMPARE(originalStored.entity.reminders.size(), 1);
    const auto stableEntitySql = [](const QString &entityId) {
        return QStringLiteral("SELECT id,title,body_markdown,folder_id,list_id,section_id,parent_id,sort_order,status,"
            "deleted_at_utc_ms,start_at_utc_ms,due_at_utc_ms,all_day_date,timezone,recurrence_series_id,occurrence_key,"
            "priority,pinned_at_utc_ms,created_at_utc_ms,completed_at_utc_ms,abandoned_at_utc_ms,server_revision "
            "FROM entities WHERE id='%1'").arg(entityId);
    };
    const QList<QVariantList> originalEntity = queryRows(stableEntitySql(id));
    const QList<QVariantList> originalNext = queryRows(stableEntitySql(link.nextId));
    QVERIFY(originalEntity.size() == 1 && originalNext.size() == 1);
    const QString associatedQueries[] = {
        QStringLiteral("SELECT * FROM checklist_items WHERE entity_id='%1' ORDER BY id").arg(id),
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
    QCOMPARE(convertedStored.entity.checklist.size(), 1); QCOMPARE(convertedStored.entity.reminders.size(), 1);
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
    ChecklistItem item; item.text = QStringLiteral("原始清单文本"); initial.checklist.append(item);
    QVERIFY(store.saveTask(initial)); const QString itemId = store.entity(id).checklist.first().id;
    const QString first = store.beginEditSession(id), second = store.beginEditSession(id);
    QVERIFY(isUuid(first) && isUuid(second)); QVERIFY(first != second);
    QVERIFY(store.beginEditSession(newUuid()).isEmpty());
    Entity edited = store.entity(id); edited.title = QStringLiteral("中间标题"); QVERIFY(store.saveTask(edited));
    for (int i = 2; i <= 4; ++i) {
        edited = store.entity(id); edited.title = QStringLiteral("中间标题 %1").arg(i); QVERIFY(store.saveTask(edited));
    }
    edited = store.entity(id); edited.title = QStringLiteral("最终标题"); edited.body = QStringLiteral("**完整新正文**\n第二行");
    edited.checklist[0].text = QStringLiteral("完整新清单文本"); QVERIFY(store.saveTask(edited));
    QVERIFY(!store.addChecklistItem(id, QStringLiteral("会话后新增条目")).isEmpty());
    const QSet<QString> beforeFirst = idsOf(store.events(id));
    QVERIFY2(store.endEditSession(first), qPrintable(store.lastError()));
    const QVector<EntityEvent> firstEvents = addedSince(beforeFirst, id); QCOMPARE(firstEvents.size(), 3);
    const QString firstBatch = firstEvents.first().batchId; QVERIFY(isUuid(firstBatch));
    bool sawTitle = false, sawBody = false, sawChecklist = false;
    for (const EntityEvent &event : firstEvents) {
        QCOMPARE(event.entityId, id); QCOMPARE(event.entityTitle, QStringLiteral("最终标题"));
        QCOMPARE(event.type, QStringLiteral("changed")); QCOMPARE(event.batchId, firstBatch);
        QVERIFY(isUuid(event.id)); QVERIFY(!event.oldValue.isValid()); QVERIFY(event.newValue.isValid());
        if (event.field == QStringLiteral("title")) {
            sawTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("最终标题"));
        } else if (event.field == QStringLiteral("body")) {
            sawBody = true; QCOMPARE(event.newValue.toString(), edited.body);
        } else if (event.field == QStringLiteral("checklist.%1.text").arg(itemId)) {
            sawChecklist = true; QCOMPARE(event.newValue.toString(), QStringLiteral("完整新清单文本"));
        } else QFAIL("会话记录了未要求的字段或新增条目文本");
    }
    QVERIFY(sawTitle && sawBody && sawChecklist); // 同一会话多次保存只写结束时的一条全文差异
    const QString unchanged = store.beginEditSession(id); QVERIFY(isUuid(unchanged));
    const QSet<QString> beforeUnchanged = idsOf(store.events(id));
    QVERIFY(store.endEditSession(unchanged)); QVERIFY(addedSince(beforeUnchanged, id).isEmpty());
    const QSet<QString> beforeSecond = idsOf(store.events(id));
    QVERIFY(store.endAllSessions()); const QVector<EntityEvent> secondEvents = addedSince(beforeSecond, id);
    QCOMPARE(secondEvents.size(), 3); QVERIFY(secondEvents.first().batchId != firstBatch); // 独立编辑器基线和批次

    const QString retry = store.beginEditSession(id); QVERIFY(isUuid(retry));
    edited = store.entity(id); edited.title = QStringLiteral("重试标题"); edited.body.clear(); QVERIFY(store.saveTask(edited));
    const QSet<QString> beforeRetry = idsOf(store.events(id));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_session_event BEFORE INSERT ON entity_events WHEN NEW.type='changed' "
        "BEGIN SELECT RAISE(ABORT,'session event rejected'); END")));
    QVERIFY(!store.endEditSession(retry)); QVERIFY(addedSince(beforeRetry, id).isEmpty());
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_session_event")));
    QVERIFY2(store.endEditSession(retry), qPrintable(store.lastError()));
    const QVector<EntityEvent> retryEvents = addedSince(beforeRetry, id); QCOMPARE(retryEvents.size(), 2);
    bool retryFullText = false, emptyBodyIsText = false;
    for (const EntityEvent &event : retryEvents) {
        if (event.field == QStringLiteral("title") && event.newValue.toString() == QStringLiteral("重试标题")) retryFullText = true;
        if (event.field == QStringLiteral("body") && event.newValue.isValid() && event.newValue.toString().isEmpty()) emptyBodyIsText = true;
    }
    QVERIFY(retryFullText); QVERIFY(emptyBodyIsText); QVERIFY(store.endAllSessions());
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
    QCOMPARE(created.value(QStringLiteral("body")).toString(), QString()); QVERIFY(created.value(QStringLiteral("checklist")).isArray());

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
    changed.startAt = utc(2026, 10, 1, 9).addMSecs(456); changed.allDayDate = QDate(2026, 10, 2);
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
    const QSet<QString> expectedFields = {QStringLiteral("due"), QStringLiteral("start"), QStringLiteral("allDayDate"),
        QStringLiteral("priority"), QStringLiteral("list"), QStringLiteral("section"), QStringLiteral("folder"),
        QStringLiteral("recurrence"), QStringLiteral("timeZone"), QStringLiteral("sortOrder"), QStringLiteral("tags")};
    QVERIFY(fields.contains(expectedFields)); QCOMPARE(batches.size(), 1); QVERIFY(isUuid(*batches.constBegin()));
    before = idsOf(store.events(id)); QVERIFY(store.saveTask(store.entity(id))); QVERIFY(addedSince(before, id).isEmpty());

    const QDateTime pinned = utc(2026, 10, 3, 10).addMSecs(789);
    before = idsOf(store.events(id)); QVERIFY(store.setPinnedAt(id, pinned));
    QVector<EntityEvent> added = addedSince(before, id); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().field, QStringLiteral("pinned")); QCOMPARE(added.first().newValue.toString(), QString::number(utcMs(pinned)));
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

    ChecklistItem check; check.text = QStringLiteral("清单活动"); changed = store.entity(id); changed.checklist.append(check);
    before = idsOf(store.events(id)); QVERIFY(store.saveTask(changed));
    for (const EntityEvent &event : addedSince(before, id)) QVERIFY(!event.field.startsWith(QStringLiteral("checklist."))); // 新增条目由 created 快照或后续操作表达
    const QString checkId = store.entity(id).checklist.first().id; QVERIFY(isUuid(checkId));
    before = idsOf(store.events(id)); QVERIFY(store.setChecklistDone(checkId, true));
    added = addedSince(before, id); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().field, QStringLiteral("checklist.%1.done").arg(checkId));
    QCOMPARE(added.first().oldValue.toString(), QStringLiteral("0")); QCOMPARE(added.first().newValue.toString(), QStringLiteral("1"));
    before = idsOf(store.events(id)); QVERIFY(store.setChecklistDone(checkId, true)); QVERIFY(addedSince(before, id).isEmpty());
    changed = store.entity(id); changed.checklist[0].text = QStringLiteral("清单文本全文");
    before = idsOf(store.events(id)); QVERIFY(store.saveTask(changed));
    added = addedSince(before, id); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().field, QStringLiteral("checklist.%1.text").arg(checkId));
    QVERIFY(!added.first().oldValue.isValid()); QCOMPARE(added.first().newValue.toString(), QStringLiteral("清单文本全文"));
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

void TaskTests::convertedNotesSuppressRemindersUntilRestored()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString id = store.createTask(QStringLiteral("转换提醒")); Entity value = store.entity(id);
    value.body = QStringLiteral("转换后保留"); value.dueAt = now.addSecs(3600); value.reminderAt = now.addSecs(-60);
    QVERIFY(store.saveTask(value)); const TaskReminder reminder = store.entity(id).reminders.first();
    ReminderScheduler scheduler(&store); QSignalSpy due(&scheduler, &ReminderScheduler::reminderDue);
    QSignalSpy triggered(&scheduler, &ReminderScheduler::reminderTriggered);
    QVERIFY(store.convertKind(id, EntityKind::Note));
    scheduler.checkDue(now); QCOMPARE(due.count(), 0); QCOMPARE(triggered.count(), 0);
    QVERIFY(!scheduler.snooze(id)); QVERIFY(!scheduler.snoozeReminder(reminder.id));
    QVERIFY(!store.markReminderNotified(id, reminder.triggerAt, now));
    QVERIFY(!store.markReminderDelivered(reminder.id, reminder.triggerAt, now));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries")).toInt(), 0);
    QCOMPARE(store.entity(id).reminders.size(), 1); QCOMPARE(store.entity(id).reminders.first().triggerAt, reminder.triggerAt);
    QCOMPARE(store.entity(id).body, value.body); QCOMPARE(store.entity(id).dueAt, value.dueAt);

    QVERIFY(store.convertKind(id, EntityKind::Task)); scheduler.checkDue(now);
    QCOMPARE(due.count(), 1); QCOMPARE(triggered.count(), 1);
    const QList<QVariant> args = triggered.takeFirst(); QCOMPARE(args.size(), 2);
    QCOMPARE(args.at(0).toString(), id); QCOMPARE(args.at(1).toString(), reminder.id);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries WHERE reminder_id='%1'").arg(reminder.id)).toInt(), 1);
    QCOMPARE(store.entity(id).body, value.body); QCOMPARE(store.entity(id).dueAt, value.dueAt);
    QVERIFY(scheduler.snooze(id, 10)); QVERIFY(store.entity(id).reminderAt > now);
}

void TaskTests::bodyMarkdownCheckboxesStayOutOfChecklist()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("正文清单不是进度")); QVERIFY(isUuid(id));
    Entity value = store.entity(id); value.body = QStringLiteral("- [ ] 未完成\n- [x] 完成");
    QVERIFY(store.saveTask(value)); QVERIFY(store.load());
    QVERIFY(store.entity(id).checklist.isEmpty());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM checklist_items WHERE entity_id='%1'").arg(id)).toInt(), 0);
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

void TaskTests::recurrenceMonthEndAndOccurrenceUnique()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString january = store.createTask(QStringLiteral("月末"));
    Entity task = store.entity(january); task.startAt = utc(2025, 1, 30, 9); task.dueAt = utc(2025, 1, 31, 10);
    task.recurrence = QStringLiteral("monthly");
    ChecklistItem item; item.text = QStringLiteral("重置步骤"); item.done = true; task.checklist.append(item);
    const QDateTime firstReminder = task.dueAt.addSecs(-1800), secondReminder = task.dueAt.addSecs(-600);
    task.reminderAt = firstReminder; QVERIFY(store.saveTask(task));
    const TaskReminder second = store.addReminder(january, secondReminder); QVERIFY(!second.id.isEmpty());
    QVERIFY(store.setCompleted(january, true)); QCOMPARE(store.entities().size(), 2);
    const QString february = activeTask(store); QVERIFY(!february.isEmpty()); Entity next = store.entity(february);
    QCOMPARE(next.dueAt, utc(2025, 2, 28, 10)); QCOMPARE(next.startAt, next.dueAt.addSecs(-25 * 3600));
    QCOMPARE(next.reminders.size(), 2); QCOMPARE(next.reminders.at(0).triggerAt, next.dueAt.addSecs(-1800));
    QCOMPARE(next.reminders.at(1).triggerAt, next.dueAt.addSecs(-600));
    QCOMPARE(next.checklist.size(), 1); QVERIFY(!next.checklist.first().done);
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
        task.startAt = utc(2020, 1, 1, 23, 15); task.reminderAt = task.startAt.addSecs(-120); task.recurrence = rule;
        task.tags = QStringList{QStringLiteral("重复标签")}; QVERIFY(store.saveTask(task)); QVERIFY(store.setCompleted(id, true));
        QString nextId;
        for (const Entity &candidate : store.entities())
            if (candidate.id != id && candidate.recurrenceSeriesId == store.entity(id).recurrenceSeriesId) nextId = candidate.id;
        Entity next = store.entity(nextId); QVERIFY(!next.id.isEmpty());
        QCOMPARE(next.startAt, task.startAt.addDays(rule == QStringLiteral("daily") ? 1 : 14));
        QCOMPARE(next.reminderAt, next.startAt.addSecs(-120)); QCOMPARE(next.tags, task.tags);
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
    QVERIFY(!store.setCompleted(id, false));
    const int stableTasks = store.entities().size();
    const int stableLinks = scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt();
    const int stableMembers = scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt();
    QVERIFY(store.setCompleted(id, true)); QVERIFY(store.setCompleted(id, true));
    QCOMPARE(store.entities().size(), stableTasks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), stableLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), stableMembers);
    QVERIFY(store.load());
    QCOMPARE(store.entities().size(), stableTasks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), stableLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), stableMembers);
    QVERIFY(store.setCompleted(id, true));
    QCOMPARE(store.entities().size(), stableTasks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM recurrence_links")).toInt(), stableLinks);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE recurrence_series_id='%1'").arg(seriesId)).toInt(), stableMembers);
    QVERIFY(store.entity(id).completed); // 已删 occurrence 不会重复生成
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
    value.startAt = utc(2026, 9, 1, 9); value.dueAt = utc(2026, 9, 1, 18); value.allDayDate = QDate(2026, 9, 1);
    value.tags = QStringList{QStringLiteral("克隆标签")}; value.recurrence = QStringLiteral("daily"); value.reminderAt = value.dueAt.addSecs(-300);
    ChecklistItem item; item.text = QStringLiteral("保留勾选状态"); item.done = true; value.checklist.append(item);
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
    QCOMPARE(copy.startAt, value.startAt); QCOMPARE(copy.dueAt, value.dueAt); QCOMPARE(copy.allDayDate, value.allDayDate);
    QCOMPARE(copy.tags, value.tags); QCOMPARE(copy.status, TaskStatus::Active); QVERIFY(!copy.completed && !copy.deleted);
    QVERIFY(!copy.completedAt.isValid() && !copy.abandonedAt.isValid() && !copy.deletedAt.isValid());
    QVERIFY(copy.recurrenceSeriesId.isEmpty()); QVERIFY(copy.occurrenceKey.isEmpty()); QCOMPARE(copy.recurrence, QStringLiteral("none"));
    QVERIFY(copy.reminders.isEmpty() && !copy.reminderAt.isValid()); QVERIFY(copy.parentId.isEmpty());
    QVERIFY(store.attachments(clone).isEmpty()); QVERIFY(store.desktopNote(clone).id.isEmpty());
    QVERIFY(!store.desktopNote(clone).open || store.desktopNote(clone).id.isEmpty());
    QCOMPARE(copy.checklist.size(), 1); QCOMPARE(copy.checklist.first().text, item.text); QVERIFY(copy.checklist.first().done);
    QVERIFY(isUuid(copy.checklist.first().id)); QVERIFY(copy.checklist.first().id != store.entity(source).checklist.first().id);
    QCOMPARE(copy.checklist.first().entityId, clone);
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
    scheduler.checkDue(now); QCOMPARE(due.count(), 2); QCOMPARE(triggered.count(), 2);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM reminder_deliveries WHERE reminder_id='%1'").arg(noteReminder)).toInt(), 0);
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QCOMPARE(store.entity(note).reminders.size(), 1);
    QVERIFY(store.isReminderDelivered(store.entity(id).reminders.at(0).id)); QVERIFY(store.isReminderDelivered(second.id));
    scheduler.checkDue(now.addDays(1)); QCOMPARE(due.count(), 2);
    const QDateTime before = QDateTime::currentDateTimeUtc(); QVERIFY(scheduler.snooze(id));
    const Entity snoozed = store.entity(id); const int primary = primaryReminderIndex(snoozed);
    QVERIFY(snoozed.reminderAt >= before.addSecs(600)); QVERIFY(!store.isReminderDelivered(snoozed.reminders.at(primary).id));
    QCOMPARE(snoozed.reminders.at(primary).id, second.id);
    scheduler.checkDue(before); QCOMPARE(due.count(), 2);
    scheduler.checkDue(store.entity(id).reminderAt.addSecs(1)); QCOMPARE(due.count(), 3);
    // 每安装独立 delivery：模拟另一安装后，同一提醒可以再次投递。
    const QString otherInstallation = newUuid();
    QVERIFY(execute(QStringLiteral("UPDATE local_meta SET value='%1' WHERE key='installation_id'").arg(otherInstallation)));
    QVERIFY(store.load()); scheduler.checkDue(QDateTime::currentDateTimeUtc().addSecs(1));
    QVERIFY(due.count() >= 4);
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
    Entity task = store.entity(parent); task.allDayDate = QDate(2025, 1, 31); task.recurrence = QStringLiteral("monthly");
    task.reminderAt = utc(2025, 1, 30, 9); QVERIFY(store.saveTask(task)); QVERIFY(store.setCompleted(parent, true));
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
    QTest::newRow("legacy-date-with-start")
        << date << fromAppTime(QDateTime(date.addDays(-1), QTime(14, 30))) << QDateTime();
    QTest::newRow("legacy-date-with-midnight") << date << QDateTime() << midnight;
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
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 4);
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
    QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), 7);
}

void TaskTests::entityCodecAndBackupRoundTrip()
{
    const QDateTime created = utc(2026, 6, 1, 8);
    Entity note;
    note.id = newUuid(); note.kind = EntityKind::Note; note.title = QStringLiteral("保留任务字段的笔记");
    note.body = QStringLiteral("**完整正文**"); note.listId = newUuid(); note.sortOrder = 91; note.pinnedAt = created.addSecs(60);
    note.status = TaskStatus::Completed; note.completed = true; note.completedAt = created.addDays(1);
    note.startAt = created; note.dueAt = created.addDays(2); note.priority = 3;
    note.createdAt = created; note.updatedAt = created.addDays(1);
    note.recurrenceSeriesId = newUuid(); note.occurrenceKey = QStringLiteral("2026-06-03");
    note.rrule = QStringLiteral("FREQ=DAILY;INTERVAL=1"); note.recurrence = QStringLiteral("daily");
    ChecklistItem item; item.id = newUuid(); item.entityId = note.id; item.text = QStringLiteral("关联清单项");
    item.done = true; item.createdAt = created; item.updatedAt = created; note.checklist.append(item);
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
    QCOMPARE(decoded.checklist.first().entityId, note.id); QCOMPARE(decoded.reminders.first().entityId, note.id);
    Entity invalid = note; invalid.parentId = note.id; QVERIFY(!entitycodec::validate(invalid, &error));
    invalid = note; invalid.kind = EntityKind::Task; invalid.completed = false; QVERIFY(!entitycodec::validate(invalid, &error));

    EntitySnapshot snapshot;
    Folder homeFolder; homeFolder.id = newUuid(); homeFolder.name = QStringLiteral("家庭");
    homeFolder.icon = QStringLiteral("🏡"); homeFolder.createdAt = created; homeFolder.updatedAt = created;
    snapshot.folders.append(homeFolder);
    TaskList inbox; inbox.id = note.listId; inbox.name = QStringLiteral("收集箱"); inbox.inbox = true;
    inbox.icon = QStringLiteral("👨‍👩‍👧‍👦"); inbox.color = QStringLiteral("#FF8800");
    inbox.createdAt = created; inbox.updatedAt = created; snapshot.lists.append(inbox);
    RecurrenceSeries series; series.id = note.recurrenceSeriesId; series.rrule = note.rrule; series.timeZone = note.timeZone;
    series.dtstartAt = note.dueAt; series.createdAt = created; series.updatedAt = created; snapshot.recurrenceSeries.append(series);
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
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 4);
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

    // v2 备份没有 icon/color/archivedAtUtcMs，folders 也没有 icon：读入补默认值。
    QJsonObject v2Root = root;
    {
        QJsonArray v2Lists = v2Root.value(QStringLiteral("lists")).toArray();
        QJsonObject v2List = v2Lists.first().toObject();
        v2List.remove(QStringLiteral("icon")); v2List.remove(QStringLiteral("color")); v2List.remove(QStringLiteral("archivedAtUtcMs"));
        v2Lists.replace(0, v2List); v2Root.insert(QStringLiteral("lists"), v2Lists);
        QJsonArray v2Folders = v2Root.value(QStringLiteral("folders")).toArray();
        QJsonObject v2Folder = v2Folders.first().toObject();
        v2Folder.remove(QStringLiteral("icon"));
        v2Folders.replace(0, v2Folder); v2Root.insert(QStringLiteral("folders"), v2Folders);
        v2Root.insert(QStringLiteral("version"), 2);
    }
    EntitySnapshot v2Restored;
    QVERIFY2(backupcodec::fromJson(v2Root, &v2Restored, &restoredEvents, &restoredTimezone, &error), qPrintable(error));
    QVERIFY(v2Restored.lists.first().icon.isEmpty()); QVERIFY(v2Restored.lists.first().color.isEmpty());
    QCOMPARE(v2Restored.lists.first().archivedAtUtcMs, qint64(0));
    QVERIFY(v2Restored.folders.first().icon.isEmpty());       // v2/v3 文件夹无 icon，补空
    // v3 备份 folders 无 icon：同样补默认值。
    QJsonObject v3Root = v2Root; v3Root.insert(QStringLiteral("version"), 3);
    {
        QJsonArray v3Lists = v3Root.value(QStringLiteral("lists")).toArray();
        QJsonObject v3List = v3Lists.first().toObject();
        v3List.insert(QStringLiteral("icon"), QStringLiteral("👨‍👩‍👧‍👦")); v3List.insert(QStringLiteral("color"), QStringLiteral("#FF8800"));
        v3List.insert(QStringLiteral("archivedAtUtcMs"), 0);
        v3Lists.replace(0, v3List); v3Root.insert(QStringLiteral("lists"), v3Lists);
    }
    EntitySnapshot v3Restored;
    QVERIFY2(backupcodec::fromJson(v3Root, &v3Restored, &restoredEvents, &restoredTimezone, &error), qPrintable(error));
    QCOMPARE(v3Restored.lists.first().icon, QStringLiteral("👨‍👩‍👧‍👦"));
    QVERIFY(v3Restored.folders.first().icon.isEmpty());
    // 未来版本与非法外观拒绝（旧程序同样拒绝 v4）。
    old = root; old.insert(QStringLiteral("version"), 5);
    QVERIFY(!backupcodec::fromJson(old, &rejected, &restoredEvents, &restoredTimezone, &error));
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

// 全 0 旧数据（v7 之前历史库）：初始顺序按旧 UI 口径 = 平铺清单在前、文件夹在后、各自 (sortOrder,id)；
// moveList 与 createFolderFromLists 都基于该口径计算位置。
void TaskTests::legacyAllZeroTopLevelOrder()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString fWork = store.createFolder(QStringLiteral("工作"));
    const QString fHome = store.createFolder(QStringLiteral("家"));
    const QString lA = store.createList(QStringLiteral("A"));
    const QString lB = store.createList(QStringLiteral("B"));
    const QString lC = store.createList(QStringLiteral("C"));
    const QString lD = store.createList(QStringLiteral("D"));
    QVERIFY(isUuid(fWork) && isUuid(fHome) && isUuid(lA) && isUuid(lB) && isUuid(lC) && isUuid(lD));
    // 旧库全 0：压平后重载，模拟升级前的历史数据。
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
    const auto legacyLists = [&store]() {
        QVector<IdName> rows;
        for (const TaskList &row : store.lists()) if (row.folderId.isEmpty()) rows.append(IdName{row.id, row.name});
        std::sort(rows.begin(), rows.end(), [](const IdName &a, const IdName &b) { return a.first < b.first; });
        return rows;
    };
    const auto legacyFolders = [&store]() {
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
    QVector<IdName> listBlock = legacyLists(), folderBlock = legacyFolders();
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

    // 恢复全 0 后从两清单建夹：新夹精确占据 target(C) 原旧 UI 位置。
    QVERIFY(execute(QStringLiteral("UPDATE lists SET sort_order=0 WHERE folder_id IS NULL")));
    QVERIFY(execute(QStringLiteral("UPDATE folders SET sort_order=0")));
    QVERIFY(store.load());
    const int aIndex = indexOfId(listBlock, lA), cIndex = indexOfId(listBlock, lC);
    const QString created = store.createFolderFromLists(QStringLiteral("新夹"), QStringLiteral("📁"), lA, lC);
    QVERIFY2(isUuid(created), qPrintable(store.lastError()));
    const int expectedSlot = cIndex - (aIndex < cIndex ? 1 : 0);
    QCOMPARE(scalar(QStringLiteral("SELECT sort_order FROM folders WHERE id='%1'").arg(created)).toInt(), expectedSlot);
    // 两清单保持旧 UI 相对顺序进入新夹。
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

void TaskTests::legacyLongListNamesRemainEditable_data()
{
    QTest::addColumn<QString>("legacyName");
    const QString name(65, QChar(0x540D));
    QTest::newRow("v5-long-name") << name;
    QTest::newRow("v5-padded-long-name") << QStringLiteral("  %1  ").arg(name);
}

void TaskTests::legacyLongListNamesRemainEditable()
{
    QFETCH(QString, legacyName);
    const QString inbox = newUuid(), list = newUuid();
    const QString now = QString::number(utcMs(utcNow()));
    QStringList statements = v5SchemaStatements(inbox, now);
    statements.append(QStringLiteral("INSERT INTO lists(id,name,created_at_utc_ms,updated_at_utc_ms) VALUES('%1','%2',%3,%3)")
                      .arg(list, legacyName, now));
    QVERIFY(seed(dbPath(), statements));
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const auto currentList = [&store, &list]() {
        for (const TaskList &row : store.lists()) if (row.id == list) return row;
        return TaskList();
    };
    QCOMPARE(currentList().name, legacyName);
    QVERIFY(store.setListArchived(list, true));
    QVERIFY(store.isListArchived(list));
    QVERIFY(store.setListArchived(list, false));
    const QString folder = store.createFolder(QStringLiteral("迁移后文件夹")); QVERIFY(isUuid(folder));
    QVERIFY(store.setListFolder(list, folder));
    QVERIFY(store.updateList(list, legacyName, QStringLiteral("🏡"), QStringLiteral("#123456"), folder));
    TaskList appearance = currentList(); appearance.sortOrder = 7;
    QVERIFY(store.saveList(appearance));
    QCOMPARE(currentList().name, legacyName);                      // 不改名的操作保留旧名称原文
    QCOMPARE(currentList().folderId, folder);
    QCOMPARE(currentList().icon, QStringLiteral("🏡"));
    QCOMPARE(currentList().color, QStringLiteral("#123456"));
    QCOMPARE(currentList().sortOrder, 7);

    QVERIFY(store.exportBackup(backupPath()));
    QVERIFY(store.importBackup(backupPath()));                     // v3 备份恢复不截断旧名称
    QCOMPARE(currentList().name, legacyName);
    QVERIFY(store.setListArchived(list, true));
    QVERIFY(store.setListArchived(list, false));

    const auto beforeRejected = queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id"));
    const QString changedLongName(66, QChar(0x65B0));
    QVERIFY(store.createList(legacyName).isEmpty());                // 历史豁免不允许新建长名称
    QVERIFY(!store.renameList(list, changedLongName));
    QVERIFY(!store.updateList(list, changedLongName, QString(), QString(), folder));
    TaskList renamed = currentList(); renamed.name = changedLongName;
    QVERIFY(!store.saveList(renamed));

    TaskRepository repository(Database::instance().connection());
    EntityChanges changes; EntitySnapshot after; QString error;
    changes.groupSaves.append(renamed);
    QVERIFY(!repository.apply(&changes, &after, &error));           // repository 也限制真正改名
    changes = EntityChanges();
    TaskList created; created.id = newUuid(); created.name = legacyName;
    created.createdAt = utcNow(); created.updatedAt = created.createdAt;
    changes.groupSaves.append(created);
    QVERIFY(!repository.apply(&changes, &after, &error));           // 绕过门面新建也不允许超长
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM lists ORDER BY id")), beforeRejected);
    QVERIFY(store.renameList(list, QString(64, QChar(0x540D))));
    QCOMPARE(currentList().name.size(), 64);
    QVERIFY(isUuid(store.createList(QString(64, QChar(0x65B0)))));
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
    archivedDone.status = TaskStatus::Completed; archivedDone.completedAt = utc(2026, 11, 1);
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
    QCOMPARE(recentSections.size(), 3);
    QCOMPARE(recentSections.at(0).key, QStringLiteral("pinned"));
    QCOMPARE(recentSections.at(0).items.size(), 4);
    QCOMPARE(recentSections.at(1).key, QStringLiteral("overdue"));
    QCOMPARE(recentSections.at(1).items.size(), 2);
    QCOMPARE(recentSections.at(2).key, QStringLiteral("upcoming"));
    QCOMPARE(recentSections.at(2).items.size(), 5);                  // 4 置顶+2 逾期+5 当天及未来 = 11

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
    QCOMPARE(sections.first().title, QStringLiteral("置顶"));
    for (const ViewSection &section : sections)
        for (const Entity &entity : section.items) QVERIFY(entity.id != archivedNote.id);
    spec.scope = ViewScope::Completed;
    const QVector<ViewSection> archiveSections = buildSections(QVector<Entity>{archivedDone}, spec, today, archiveSet);
    QCOMPARE(archiveSections.size(), 1);
    QCOMPARE(archiveSections.first().key, QStringLiteral("completed"));
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
    const QString startOnly = store.createTask(QStringLiteral("仅开始日期逾期"));
    const QString allDay = store.createTask(QStringLiteral("全天且保留开始日期"));
    {
        Entity value = store.entity(overnight);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 10), QTime(22, 15, 25, 123)));
        value.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 11), QTime(7, 30, 40, 456)));
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
        value.recurrence = QStringLiteral("daily");
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(startOnly);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 12), QTime(0, 30)));
        QVERIFY(store.saveTask(value));
    }
    {
        Entity value = store.entity(allDay);
        value.allDayDate = QDate(2026, 11, 11);
        value.startAt = fromAppTime(QDateTime(QDate(2026, 11, 9), QTime(22, 0)));
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
    QCOMPARE(toAppTime(store.entity(startOnly).startAt).date(), today);
    QCOMPARE(toAppTime(store.entity(startOnly).startAt).time(), QTime(0, 30));
    QVERIFY(!store.entity(startOnly).dueAt.isValid());
    QCOMPARE(store.entity(allDay).allDayDate, today);
    QCOMPARE(toAppTime(store.entity(allDay).startAt).date(), today.addDays(-2));
    QCOMPARE(toAppTime(store.entity(allDay).startAt).time(), QTime(22, 0));
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id")), reminderRows);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminder_deliveries ORDER BY reminder_id,installation_id")), deliveryRows);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM desktop_notes ORDER BY id")), desktopRows);
    QCOMPARE(overnightAfter.reminders.size(), 3);
    QVERIFY(store.isReminderDelivered(delivered.id));
}

// 拖放分区纯规则：固定 today 的四时间分区映射、非法 key/非法 today 的无效值。
void TaskTests::taskDropSectionRules()
{
    QVERIFY(TaskDrop::isTimeSection(QStringLiteral("overdue")));
    QVERIFY(TaskDrop::isTimeSection(QStringLiteral("today")));
    QVERIFY(TaskDrop::isTimeSection(QStringLiteral("tomorrow")));
    QVERIFY(TaskDrop::isTimeSection(QStringLiteral("next7days")));
    const char *nonTime[] = {"pinned", "upcoming", "undated", "notes", "list:x",
        "date:2026-11-15", "", "今天", "Today", "OVERDUE", "next7"};
    for (const char *key : nonTime) QVERIFY(!TaskDrop::isTimeSection(QString::fromUtf8(key)));

    const QDate today(2026, 11, 15);
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("overdue"), today), QDate(2026, 11, 14));
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("today"), today), QDate(2026, 11, 15));
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("tomorrow"), today), QDate(2026, 11, 16));
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("next7days"), today), QDate(2026, 11, 17));
    const char *invalidKeys[] = {"", "pinned", "upcoming", "undated", "notes", "yesterday",
        "date:2026-11-15", "今天", "next7", "Today"};
    for (const char *key : invalidKeys) {
        QVERIFY(!TaskDrop::dateForSection(QString::fromUtf8(key), today).isValid());  // 非法 key
        QVERIFY(!TaskDrop::dateForSection(QString::fromUtf8(key), QDate()).isValid());
    }
    const char *timeKeys[] = {"overdue", "today", "tomorrow", "next7days"};
    for (const char *key : timeKeys)
        QVERIFY(!TaskDrop::dateForSection(QString::fromUtf8(key), QDate()).isValid()); // 非法 today
    // 映射只做日偏移，跨月/跨年/闰年边界正确。
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("tomorrow"), QDate(2026, 12, 31)), QDate(2027, 1, 1));
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("overdue"), QDate(2026, 3, 1)), QDate(2026, 2, 28));
    QCOMPARE(TaskDrop::dateForSection(QStringLiteral("next7days"), QDate(2024, 2, 27)), QDate(2024, 2, 29));
}

// 跨分区拖动的日期语义：纯日期、明确 00:00、带时刻与起止跨度，提醒原样；重载后顺序与日期保持。
void TaskTests::taskDropShiftsDatesAcrossSections()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    QVERIFY(today.isValid());
    const QDate yesterday = today.addDays(-1), tomorrow = today.addDays(1), dayAfter = today.addDays(2);
    const QString list = store.createList(QStringLiteral("拖放清单"));

    const QString a = store.createTask(QStringLiteral("甲"), list);
    const QString b = store.createTask(QStringLiteral("乙"), list);
    const QString c = store.createTask(QStringLiteral("丙"), list);
    for (const QString &id : QStringList{a, b, c}) {
        Entity value = store.entity(id); value.allDayDate = today; QVERIFY(store.saveTask(value));
    }
    QCOMPARE(store.entity(a).sortOrder, 0);                          // 新任务默认全 0
    // 同段重排：甲拖到段尾，sortOrder 规范为 0..n-1，日期不动。
    QVERIFY2(store.moveEntityForDrop(a, QStringList{b, c, a}, QStringLiteral("today"), today, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(b).sortOrder, 0);
    QCOMPARE(store.entity(c).sortOrder, 1);
    QCOMPARE(store.entity(a).sortOrder, 2);
    for (const QString &id : QStringList{a, b, c}) QCOMPARE(store.entity(id).allDayDate, today);
    // 幂等：同一顺序再拖一次零写入（revision/updated_at 都不动）。
    const QString rowsSql = QStringLiteral("SELECT * FROM entities ORDER BY id");
    const auto rows = queryRows(rowsSql);
    QVERIFY(store.moveEntityForDrop(a, QStringList{b, c, a}, QStringLiteral("today"), today, false, false));
    QCOMPARE(queryRows(rowsSql), rows);

    QVERIFY2(store.moveEntityForDrop(b, QStringList{b}, QStringLiteral("tomorrow"), tomorrow, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(b).allDayDate, tomorrow);                  // 今天 -> 明天
    QVERIFY2(store.moveEntityForDrop(a, QStringList{a}, QStringLiteral("overdue"), yesterday, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(a).allDayDate, yesterday);                 // 今天 -> 过期昨天
    QVERIFY2(store.moveEntityForDrop(a, QStringList{a}, QStringLiteral("today"), today, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(a).allDayDate, today);                     // 过期 -> 今天
    QVERIFY2(store.moveEntityForDrop(c, QStringList{c}, QStringLiteral("next7days"), dayAfter, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(c).allDayDate, dayAfter);                  // 今天 -> 最近7天（后天）

    // “最近7天”段内仅排序：第4/5天不能被错误归一为后天。
    const QString day4 = store.createTask(QStringLiteral("第四天"), list);
    const QString day5 = store.createTask(QStringLiteral("第五天"), list);
    Entity day4Value = store.entity(day4); day4Value.allDayDate = today.addDays(4);
    Entity day5Value = store.entity(day5); day5Value.allDayDate = today.addDays(5);
    QVERIFY(store.saveTask(day4Value)); QVERIFY(store.saveTask(day5Value));
    QVERIFY2(store.moveEntityForDrop(day5, QStringList{day5, day4}, QStringLiteral("next7days"),
                                     QDate(), false, false, false), qPrintable(store.lastError()));
    QCOMPARE(store.entity(day4).allDayDate, today.addDays(4));
    QCOMPARE(store.entity(day5).allDayDate, today.addDays(5));
    QCOMPARE(store.entity(day5).sortOrder, 0);
    QCOMPARE(store.entity(day4).sortOrder, 1);

    // 明确 00:00 的定时任务跨区仍保持午夜整点，不伪造成全天。
    const QString midnight = store.createTask(QStringLiteral("零点截止"), list);
    {
        Entity value = store.entity(midnight);
        value.dueAt = fromAppTime(QDateTime(today, QTime(0, 0)));
        QVERIFY(store.saveTask(value));
    }
    QVERIFY2(store.moveEntityForDrop(midnight, QStringList{midnight}, QStringLiteral("tomorrow"), tomorrow, false, false),
             qPrintable(store.lastError()));
    QCOMPARE(toAppTime(store.entity(midnight).dueAt).date(), tomorrow);
    QCOMPARE(toAppTime(store.entity(midnight).dueAt).time(), QTime(0, 0));
    QVERIFY(!store.entity(midnight).allDayDate.isValid());

    // 带时刻和起止跨度跨区：应用时区下保留时刻与跨度，提醒原样。
    // effectiveDate 以 dueAt 日期为锚（昨天 22:15 -> 今天 07:30 的跨午夜跨度）。
    const QString span = store.createTask(QStringLiteral("跨日跨度"), list);
    {
        Entity value = store.entity(span);
        value.startAt = fromAppTime(QDateTime(yesterday, QTime(22, 15, 25, 123)));
        value.dueAt = fromAppTime(QDateTime(today, QTime(7, 30, 40, 456)));
        TaskReminder early; early.triggerAt = fromAppTime(QDateTime(yesterday, QTime(21, 0)));
        TaskReminder late; late.triggerAt = fromAppTime(QDateTime(today, QTime(1, 30)));
        value.reminders.append(early); value.reminders.append(late);
        QVERIFY(store.saveTask(value));
    }
    const auto reminderRows = queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id"));
    const Entity spanBefore = store.entity(span);
    QVERIFY2(store.moveEntityForDrop(span, QStringList{span}, QStringLiteral("overdue"), yesterday, false, false),
             qPrintable(store.lastError()));
    const Entity spanAfter = store.entity(span);
    QCOMPARE(toAppTime(spanAfter.startAt).date(), today.addDays(-2));   // 锚点平移 -1 天
    QCOMPARE(toAppTime(spanAfter.startAt).time(), QTime(22, 15, 25, 123));
    QCOMPARE(toAppTime(spanAfter.dueAt).date(), yesterday);
    QCOMPARE(toAppTime(spanAfter.dueAt).time(), QTime(7, 30, 40, 456));
    QCOMPARE(spanAfter.startAt.msecsTo(spanAfter.dueAt), spanBefore.startAt.msecsTo(spanBefore.dueAt));
    QCOMPARE(spanAfter.reminders.size(), 2);
    QCOMPARE(spanAfter.reminders.first().triggerAt, spanBefore.reminders.first().triggerAt);
    QCOMPARE(queryRows(QStringLiteral("SELECT * FROM reminders ORDER BY id")), reminderRows);

    // 重载后 sortOrder 与日期全部保持。
    QVERIFY(store.load());
    QCOMPARE(store.entity(a).allDayDate, today);
    QCOMPARE(store.entity(b).allDayDate, tomorrow);
    QCOMPARE(store.entity(c).allDayDate, dayAfter);
    QCOMPARE(toAppTime(store.entity(midnight).dueAt).time(), QTime(0, 0));
    QCOMPARE(toAppTime(store.entity(span).startAt).date(), today.addDays(-2));
    QCOMPARE(store.entity(a).sortOrder, 0);
    QCOMPARE(store.entity(b).sortOrder, 0);
}

// 置顶互相拖动与子任务拖出：日期、顺序、置顶、parent detach 同批更新。
void TaskTests::taskDropReorderPinnedAndParentDetach()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QDate tomorrow = today.addDays(1);
    const QString list = store.createList(QStringLiteral("置顶拖放清单"));
    const QString anchor = store.createTask(QStringLiteral("明日锚点"), list);
    {
        Entity value = store.entity(anchor); value.allDayDate = tomorrow; QVERIFY(store.saveTask(value));
    }
    const QString parent = store.createTask(QStringLiteral("父任务"), list);
    {
        Entity value = store.entity(parent); value.allDayDate = today; QVERIFY(store.saveTask(value));
    }
    const QString child = store.createSubtask(QStringLiteral("子任务"), parent);
    QVERIFY2(isUuid(child), qPrintable(store.lastError()));
    QCOMPARE(store.entity(child).allDayDate, today);

    const QString first = store.createTask(QStringLiteral("先置顶"), list);
    const QString second = store.createTask(QStringLiteral("后置顶"), list);
    QVERIFY(store.setPinnedAt(first, utc(2026, 1, 1, 8)));
    QVERIFY(store.setPinnedAt(second, utc(2026, 1, 2, 9)));
    const QDateTime firstPin = store.entity(first).pinnedAt, secondPin = store.entity(second).pinnedAt;

    // 置顶段内互相拖动：只重排 sortOrder，已置顶保持原 pinnedAt，无日期任务不暗改日期。
    QVERIFY2(store.moveEntityForDrop(second, QStringList{second, first}, QStringLiteral("pinned"), QDate(), true, false),
             qPrintable(store.lastError()));
    QCOMPARE(store.entity(second).sortOrder, 0);
    QCOMPARE(store.entity(first).sortOrder, 1);
    QCOMPARE(store.entity(first).pinnedAt, firstPin);
    QCOMPARE(store.entity(second).pinnedAt, secondPin);
    QVERIFY(!store.entity(first).allDayDate.isValid());

    // 置顶子任务拖到明天段顶部：日期平移、顺序、parent detach 一次完成；新置顶取当前时刻。
    QVERIFY2(store.moveEntityForDrop(child, QStringList{child, anchor}, QStringLiteral("tomorrow"), tomorrow, true, true),
             qPrintable(store.lastError()));
    const Entity moved = store.entity(child);
    QCOMPARE(moved.allDayDate, tomorrow);
    QVERIFY(moved.parentId.isEmpty());
    QVERIFY(moved.pinnedAt.isValid());
    QCOMPARE(store.entity(anchor).sortOrder, 1);
    QVERIFY(!store.entity(parent).pinnedAt.isValid());
    QVERIFY(!store.entity(parent).deleted);

    // 同位再拖但取消置顶：仅清空 pinnedAt，日期与父子关系不动。
    const QString rowsSql = QStringLiteral("SELECT id,all_day_date,parent_id,sort_order FROM entities WHERE id='%1'");
    const auto beforeUnpin = queryRows(rowsSql.arg(child));
    QVERIFY2(store.moveEntityForDrop(child, QStringList{child, anchor}, QStringLiteral("tomorrow"), tomorrow, false, false),
             qPrintable(store.lastError()));
    QVERIFY(!store.entity(child).pinnedAt.isValid());
    QCOMPARE(queryRows(rowsSql.arg(child)), beforeUnpin);

    // 子任务拖到原父级前：父级既要记录孩子关系变化，也要更新顺序，同一批只能保存一次。
    const QString beforeParent = store.createSubtask(QStringLiteral("拖到父级前"), parent);
    QVERIFY2(store.moveEntityForDrop(beforeParent, QStringList{beforeParent, parent},
                                     QStringLiteral("today"), today, false, true),
             qPrintable(store.lastError()));
    QVERIFY(store.entity(beforeParent).parentId.isEmpty());
    QCOMPARE(store.entity(beforeParent).sortOrder, 0);
    QCOMPARE(store.entity(parent).sortOrder, 1);
    QCOMPARE(store.entity(beforeParent).allDayDate, today);
    QVERIFY(store.load());
    QVERIFY(store.entity(beforeParent).parentId.isEmpty());
    QCOMPARE(store.entity(parent).sortOrder, 1);
}

// 同父同显示段排序：显示在置顶父任务下的子任务保留实际 pinnedAt，不由目标段推断。
void TaskTests::taskDropSiblingReorderPreservesPinnedAt_data()
{
    QTest::addColumn<bool>("sourcePinned");
    QTest::addColumn<bool>("requestedPinned");
    QTest::newRow("unpinned-child-pinned-target") << false << true;
    QTest::newRow("unpinned-child-unpinned-target") << false << false;
    QTest::newRow("pinned-child-pinned-target") << true << true;
    QTest::newRow("pinned-child-unpinned-target") << true << false;
}
void TaskTests::taskDropSiblingReorderPreservesPinnedAt()
{
    QFETCH(bool, sourcePinned);
    QFETCH(bool, requestedPinned);
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QString list = store.createList(QStringLiteral("置顶父任务清单"));
    const QString parent = store.createTask(QStringLiteral("置顶父任务"), list);
    Entity value = store.entity(parent); value.allDayDate = today; QVERIFY(store.saveTask(value));
    const QDateTime parentPin = utc(2026, 1, 1, 8);
    QVERIFY(store.setPinnedAt(parent, parentPin));
    const QString source = store.createSubtask(QStringLiteral("待排序子任务"), parent);
    const QString sibling = store.createSubtask(QStringLiteral("兄弟任务"), parent);
    QVERIFY(isUuid(source) && isUuid(sibling));
    const QDateTime sourcePin = sourcePinned ? utc(2026, 1, 2, 9) : QDateTime();
    if (sourcePinned) QVERIFY(store.setPinnedAt(source, sourcePin));
    QCOMPARE(store.entity(source).sortOrder, 0);
    QCOMPARE(store.entity(sibling).sortOrder, 1);

    const QStringList ordered{parent, sibling, source};
    QVERIFY2(store.moveEntityForDrop(source, ordered, QStringLiteral("pinned"), QDate(),
                                     requestedPinned, false, false, false), qPrintable(store.lastError()));
    QCOMPARE(store.entity(source).sortOrder, 2);
    QCOMPARE(store.entity(sibling).sortOrder, 1);
    QCOMPARE(store.entity(source).pinnedAt, sourcePin);
    QCOMPARE(store.entity(parent).pinnedAt, parentPin);
    QVERIFY(!store.entity(sibling).pinnedAt.isValid());
    QCOMPARE(store.entity(source).parentId, parent);
    QCOMPARE(store.entity(sibling).parentId, parent);
    QCOMPARE(store.entity(source).allDayDate, today);

    // 无位移再排序不产生 revision、时间戳或日志写入。
    const QString entitiesSql = QStringLiteral("SELECT * FROM entities ORDER BY id");
    const QString eventsSql = QStringLiteral("SELECT * FROM entity_events ORDER BY id");
    const auto entitiesBefore = queryRows(entitiesSql), eventsBefore = queryRows(eventsSql);
    QVERIFY(store.moveEntityForDrop(source, ordered, QStringLiteral("pinned"), QDate(),
                                    requestedPinned, false, false, false));
    QCOMPARE(queryRows(entitiesSql), entitiesBefore);
    QCOMPARE(queryRows(eventsSql), eventsBefore);
    QVERIFY(store.load());
    QCOMPARE(store.entity(source).pinnedAt, sourcePin);
    QCOMPARE(store.entity(source).parentId, parent);
    QCOMPARE(store.entity(source).sortOrder, 2);
}

// 右下半区设父只改父子关系与顺序；非法父项和完整祖先链循环均在写入前拒绝。
void TaskTests::taskDropReparentValidationAndPersistence()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QString list = store.createList(QStringLiteral("设父拖放清单"));
    const QString otherList = store.createList(QStringLiteral("其他设父清单"));
    const QString parent = store.createTask(QStringLiteral("目标父任务"), list);
    const QString source = store.createTask(QStringLiteral("待设父任务"), list);
    const QString grandchild = store.createSubtask(QStringLiteral("源任务后代"), source);
    QVERIFY(isUuid(parent) && isUuid(source) && isUuid(grandchild));
    Entity parentValue = store.entity(parent); parentValue.allDayDate = today;
    QVERIFY(store.saveTask(parentValue));
    Entity sourceValue = store.entity(source);
    sourceValue.dueAt = fromAppTime(QDateTime(today.addDays(2), QTime(9, 30)));
    sourceValue.timeZone = QStringLiteral("Asia/Shanghai");
    QVERIFY(store.saveTask(sourceValue));
    const QDateTime sourcePin = utc(2026, 2, 3, 8, 30);
    QVERIFY(store.setPinnedAt(source, sourcePin));
    const QDateTime sourceDue = store.entity(source).dueAt;
    const QString section = QStringLiteral("list:") + list;
    const QStringList ordered{parent, source, grandchild};
    QVERIFY2(store.moveEntityForDrop(source, ordered, section, QDate(), false,
                                     false, false, false, parent), qPrintable(store.lastError()));
    QCOMPARE(store.entity(source).parentId, parent);
    QCOMPARE(store.entity(source).sortOrder, 1);
    QCOMPARE(store.entity(source).dueAt, sourceDue);
    QCOMPARE(store.entity(source).pinnedAt, sourcePin);
    QCOMPARE(store.entity(grandchild).parentId, source);
    QCOMPARE(store.entity(grandchild).sortOrder, 2);
    QVERIFY(store.load());
    QCOMPARE(store.entity(source).parentId, parent);
    QCOMPARE(store.entity(source).dueAt, sourceDue);
    QCOMPARE(store.entity(source).pinnedAt, sourcePin);
    QCOMPARE(store.entity(grandchild).parentId, source);

    const QString entitiesSql = QStringLiteral("SELECT * FROM entities ORDER BY id");
    const QString eventsSql = QStringLiteral("SELECT * FROM entity_events ORDER BY id");
    auto entitiesBefore = queryRows(entitiesSql), eventsBefore = queryRows(eventsSql);
    QVERIFY(store.moveEntityForDrop(source, ordered, section, QDate(), false,
                                    false, false, false, parent));
    QCOMPARE(queryRows(entitiesSql), entitiesBefore);
    QCOMPARE(queryRows(eventsSql), eventsBefore);

    const auto reject = [&](const QString& dragged, const QString& newParent,
                            const QStringList& ids, const QString& targetSection) {
        const auto entities = queryRows(entitiesSql), events = queryRows(eventsSql);
        QVERIFY(!store.moveEntityForDrop(dragged, ids, targetSection, QDate(), false,
                                         false, false, false, newParent));
        QVERIFY(!store.lastError().isEmpty());
        QCOMPARE(queryRows(entitiesSql), entities);
        QCOMPARE(queryRows(eventsSql), events);
    };
    reject(source, source, QStringList{source, grandchild}, section);
    reject(parent, grandchild, ordered, section);

    const QString otherParent = store.createTask(QStringLiteral("跨清单父任务"), otherList);
    reject(source, otherParent, QStringList{otherParent, source, grandchild}, section);

    const QString noteParent = store.createNote(QStringLiteral("笔记父项"), list);
    const QString candidate = store.createTask(QStringLiteral("合法源任务"), list);
    reject(candidate, noteParent, QStringList{noteParent, candidate}, section);

    const QString completedParent = store.createTask(QStringLiteral("已完成父项"), list);
    QVERIFY(store.setCompleted(completedParent, true));
    reject(candidate, completedParent, QStringList{completedParent, candidate}, section);
    const QString abandonedParent = store.createTask(QStringLiteral("已放弃父项"), list);
    QVERIFY(store.abandonTask(abandonedParent));
    reject(candidate, abandonedParent, QStringList{abandonedParent, candidate}, section);

    const auto entities = queryRows(entitiesSql), events = queryRows(eventsSql);
    QVERIFY(!store.moveEntityForDrop(candidate, QStringList{parent, candidate}, section,
                                     QDate(), false, false, true, true, parent));
    QCOMPARE(queryRows(entitiesSql), entities);
    QCOMPARE(queryRows(eventsSql), events);
}

// list:<id> 只允许该未归档清单内重排：非法目标/跨清单成员必须在写入前整批拒绝。
void TaskTests::taskDropListSectionValidatesMembership()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("拖动文件夹"));
    const QString firstList = store.createList(QStringLiteral("来源清单"), folder);
    const QString secondList = store.createList(QStringLiteral("其他清单"), folder);
    const QString archivedList = store.createList(QStringLiteral("归档清单"), folder);
    const QString source = store.createTask(QStringLiteral("置顶来源"), firstList);
    const QString anchor = store.createNote(QStringLiteral("同清单笔记"), firstList);
    const QString foreign = store.createTask(QStringLiteral("其他清单任务"), secondList);
    const QString archived = store.createTask(QStringLiteral("归档任务"), archivedList);
    QVERIFY(isUuid(source) && isUuid(anchor) && isUuid(foreign) && isUuid(archived));
    Entity value = store.entity(source); value.sortOrder = 7; QVERIFY(store.saveTask(value));
    QVERIFY(store.setPinnedAt(source, utc(2026, 1, 1, 8)));
    QVERIFY(store.setListArchived(archivedList, true));
    const QString firstSection = QStringLiteral("list:") + firstList;
    struct RejectedDrop {
        QString sourceId;
        QStringList orderedIds;
        QString targetSection;
    };
    const QVector<RejectedDrop> rejected{
        {source, {foreign, source}, QStringLiteral("list:") + secondList}, // source 不属于目标
        {source, {anchor, source, foreign}, firstSection},               // 其他段内成员跨清单
        {source, {anchor, source}, QStringLiteral("list:") + newUuid()}, // 目标清单不存在
        {source, {anchor, source}, QStringLiteral("list:")},            // 空清单 ID
        {archived, {archived}, QStringLiteral("list:") + archivedList}  // 同清单但目标已归档
    };
    const QString entitiesSql = QStringLiteral("SELECT * FROM entities ORDER BY id");
    const QString listsSql = QStringLiteral("SELECT * FROM lists ORDER BY id");
    const QString eventsSql = QStringLiteral("SELECT * FROM entity_events ORDER BY id");
    const auto entitiesBefore = queryRows(entitiesSql), listsBefore = queryRows(listsSql);
    const auto eventsBefore = queryRows(eventsSql);
    for (const RejectedDrop &drop : rejected) {
        QVERIFY2(!store.moveEntityForDrop(drop.sourceId, drop.orderedIds, drop.targetSection,
                                          QDate(), false, false, false), qPrintable(drop.targetSection));
        QVERIFY(!store.lastError().isEmpty());
        QCOMPARE(queryRows(entitiesSql), entitiesBefore);                // 包含 revision、置顶、顺序
        QCOMPARE(queryRows(listsSql), listsBefore);
        QCOMPARE(queryRows(eventsSql), eventsBefore);
    }

    // 合法投放仍按默认 adjustPinned=true 取消置顶，允许同清单的任务与笔记一起排序。
    QVERIFY2(store.moveEntityForDrop(source, QStringList{source, anchor}, firstSection,
                                     QDate(), false, false, false), qPrintable(store.lastError()));
    QCOMPARE(store.entity(source).sortOrder, 0);
    QCOMPARE(store.entity(anchor).sortOrder, 1);
    QVERIFY(!store.entity(source).pinnedAt.isValid());
    QCOMPARE(store.entity(source).listId, firstList);
    QCOMPARE(store.entity(anchor).listId, firstList);
    QVERIFY(store.load());
    QCOMPARE(store.entity(source).sortOrder, 0);
    QCOMPARE(store.entity(anchor).sortOrder, 1);
    QVERIFY(!store.entity(source).pinnedAt.isValid());
}

// 原子失败：非法 ID、重复、缺 source、错误目标日期、矛盾置顶、无日期入时间分区、陈旧 revision 全部零写入。
void TaskTests::taskDropAtomicFailuresAndIdempotence()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QDate tomorrow = today.addDays(1);
    const QString list = store.createList(QStringLiteral("原子清单"));
    const QString a = store.createTask(QStringLiteral("原子甲"), list);
    const QString b = store.createTask(QStringLiteral("原子乙"), list);
    const QString noDate = store.createTask(QStringLiteral("无日期"), list);
    for (const QString &id : QStringList{a, b}) {
        Entity value = store.entity(id); value.allDayDate = today; QVERIFY(store.saveTask(value));
    }
    const QString rowsSql = QStringLiteral("SELECT * FROM entities ORDER BY id");
    const auto rows = queryRows(rowsSql);

    QVERIFY(!store.moveEntityForDrop(newUuid(), QStringList{a, b}, QStringLiteral("today"), today, false, false));    // source 不存在
    QVERIFY(!store.moveEntityForDrop(a, QStringList{a, b, newUuid()}, QStringLiteral("today"), today, false, false)); // 段内未知 id
    QVERIFY(!store.moveEntityForDrop(a, QStringList{a, b, b}, QStringLiteral("today"), today, false, false));         // 重复
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b}, QStringLiteral("today"), today, false, false));               // 缺 source
    QVERIFY(!store.moveEntityForDrop(a, QStringList(), QStringLiteral("today"), today, false, false));                // 空目标段
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("today"), tomorrow, false, false));         // 错误目标日期
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("today"), QDate(), false, false));          // 时间分区缺日期
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("upcoming"), today, false, false));         // 非时间分区带日期
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("pinned"), QDate(), false, false));         // 置顶分区必须置顶
    QVERIFY(!store.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("yesterday"), today.addDays(-1), false, false)); // 非法分区 key
    QVERIFY(!store.moveEntityForDrop(noDate, QStringList{noDate}, QStringLiteral("today"), today, false, false));     // 无日期入时间分区
    QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(queryRows(rowsSql), rows);                              // 全部零写入

    // 陈旧 revision：落后快照的 store 被 repository CAS 整批拒绝；重载后同一命令成功。
    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    {
        Entity value = store.entity(a); value.priority = 2; QVERIFY(store.saveTask(value));
    }
    const auto fresh = queryRows(rowsSql);
    QVERIFY(!stale.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("today"), today, false, false));
    QVERIFY(stale.lastError().contains(QStringLiteral("修改")));
    QCOMPARE(queryRows(rowsSql), fresh);                             // 无部分成功
    QVERIFY(stale.load());
    QVERIFY2(stale.moveEntityForDrop(a, QStringList{b, a}, QStringLiteral("today"), today, false, false),
             qPrintable(stale.lastError()));
    QCOMPARE(stale.entity(b).sortOrder, 0);
    QCOMPARE(stale.entity(a).sortOrder, 1);
    QCOMPARE(stale.entity(a).allDayDate, today);
}

// 子任务创建：继承日期/时区/清单/段落，不继承置顶、提醒、重复、完成状态；非法父项拒绝。
void TaskTests::createSubtaskInheritsDatesOnly()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QDate today = toAppTime(appNow()).date();
    const QString list = store.createList(QStringLiteral("子任务清单"));
    const QString section = store.createSection(list, QStringLiteral("段落"));
    const QString parent = store.createTask(QStringLiteral("父任务"), list, section);
    {
        Entity value = store.entity(parent);
        value.allDayDate = today;
        value.startAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        value.dueAt = fromAppTime(QDateTime(today, QTime(18, 0)));
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
    QCOMPARE(value.allDayDate, today);                               // 继承日期语义
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
    Entity task = store.entity(id); task.body = QStringLiteral("**Markdown** [目标](kl:%1)").arg(target); task.priority = 3;
    task.sortOrder = 23; task.pinnedAt = utc(2026, 4, 30, 12);
    task.tags = QStringList{QStringLiteral("家庭"), QStringLiteral("标签 Ω")}; task.startAt = utc(2026, 5, 1, 9); task.dueAt = utc(2026, 5, 1, 17);
    ChecklistItem item; item.text = QStringLiteral("检查项"); item.done = true; task.checklist.append(item); task.reminderAt = utc(2026, 5, 1, 8);
    task.color = NoteColor::Green; task.geometry = QRect(-20, 30, 360, 300); QVERIFY(store.saveTask(task));
    task = store.entity(id); // 保存后取得数据库分配的清单 UUID 供覆盖恢复比较
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
    QVERIFY(execute(QStringLiteral("UPDATE checklist_items SET server_revision=17")));
    QVERIFY(execute(QStringLiteral("UPDATE tags SET server_revision=18+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE entity_tags SET server_revision=19+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE reminders SET server_revision=20+rowid")));
    QVERIFY(execute(QStringLiteral("UPDATE attachments SET server_revision=21")));
    QVERIFY(store.load());
    Entity localSave = store.entity(target); localSave.body = QStringLiteral("普通保存不改 server_revision"); QVERIFY(store.saveTask(localSave));
    QCOMPARE(scalar(QStringLiteral("SELECT server_revision FROM entities WHERE id='%1'").arg(target)).toLongLong(), qint64(16));
    const QStringList businessTables = {QStringLiteral("folders"),QStringLiteral("lists"),QStringLiteral("sections"),
        QStringLiteral("recurrence_series"),QStringLiteral("entities"),QStringLiteral("checklist_items"),QStringLiteral("tags"),
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
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 4); QCOMPARE(frames.size(), 1);
    QCOMPARE(root.value(QStringLiteral("defaultTimezone")).toString(), QStringLiteral("Asia/Shanghai"));
    QCOMPARE(root.value(QStringLiteral("events")).toArray().size(), expectedEvents.size());
    QCOMPARE(root.value(QStringLiteral("checklistItems")).toArray().size(), 1);
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
    QCOMPARE(store.entity(id).checklist.first().id, task.checklist.first().id); QCOMPARE(store.entity(id).reminders.size(), 2);
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
    // v2 容器（无 icon/color/archivedAtUtcMs，folders 无 icon）：读入成功并补默认值。
    QJsonObject v2Metadata = root;
    {
        QJsonArray v2Lists = v2Metadata.value(QStringLiteral("lists")).toArray();
        for (int i = 0; i < v2Lists.size(); ++i) {
            QJsonObject object = v2Lists.at(i).toObject();
            object.remove(QStringLiteral("icon")); object.remove(QStringLiteral("color")); object.remove(QStringLiteral("archivedAtUtcMs"));
            v2Lists.replace(i, object);
        }
        QJsonArray v2Folders = v2Metadata.value(QStringLiteral("folders")).toArray();
        for (int i = 0; i < v2Folders.size(); ++i) {
            QJsonObject object = v2Folders.at(i).toObject();
            object.remove(QStringLiteral("icon"));
            v2Folders.replace(i, object);
        }
        v2Metadata.insert(QStringLiteral("lists"), v2Lists);
        v2Metadata.insert(QStringLiteral("folders"), v2Folders); v2Metadata.insert(QStringLiteral("version"), 2);
    }
    QVERIFY(writeContainer(legacy, v2Metadata, frames));
    QVERIFY2(store.importBackup(legacy), qPrintable(store.lastError()));
    QCOMPARE(store.entities().size(), 2);
    for (const TaskList &candidate : store.lists()) {
        QVERIFY(candidate.icon.isEmpty()); QVERIFY(candidate.color.isEmpty());
        QCOMPARE(candidate.archivedAtUtcMs, qint64(0));       // v2 无归档语义，一律未归档
    }
    for (const Folder &candidate : store.folders()) QVERIFY(candidate.icon.isEmpty());
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
        QStringLiteral("checklist_items"),QStringLiteral("tags"),QStringLiteral("entity_tags"),QStringLiteral("reminders"),
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
