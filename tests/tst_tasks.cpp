#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "db/TaskRepository.h"
#include "domain/AttachmentCodec.h"
#include "domain/BackupCodec.h"

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
    void freshSchemaNeverMigratesLegacy();
    void rejectsWrongIdentityAndFutureVersion();
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
    void entityCodecAndBackupV2RoundTrip();
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
    QCOMPARE(Database::schemaVersion(), 5);
    QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), 5);
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
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('lists') WHERE name='folder_id' AND \"notnull\"=0")).toInt(), 1);
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
    QVERIFY(seed(future, {QStringLiteral("CREATE TABLE future_data(id TEXT)"), QStringLiteral("PRAGMA user_version=6"),
                          QStringLiteral("PRAGMA application_id=%1").arg(Database::applicationId())}));
    QVERIFY(!Database::instance().openAt(future, &error));
    QCOMPARE(inspect(future, QStringLiteral("PRAGMA user_version")).toInt(), 6);
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
    QVERIFY(store.deleteList(list)); QCOMPARE(store.entity(parent).listId, store.inboxListId()); QCOMPARE(store.entity(child).listId, store.inboxListId());
    QCOMPARE(store.entity(child).parentId, parent);
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

void TaskTests::entityCodecAndBackupV2RoundTrip()
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
    TaskList inbox; inbox.id = note.listId; inbox.name = QStringLiteral("收集箱"); inbox.inbox = true;
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
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(root.value(QStringLiteral("defaultTimezone")).toString(), QStringLiteral("UTC"));
    QCOMPARE(root.value(QStringLiteral("entities")).toArray().size(), 1);
    QCOMPARE(root.value(QStringLiteral("events")).toArray().size(), 2);
    QVERIFY(!root.contains(QStringLiteral("tasks"))); QVERIFY(!root.contains(QStringLiteral("localMeta"))); QVERIFY(!root.contains(QStringLiteral("workspaces")));
    EntitySnapshot restored; QVector<EntityEvent> restoredEvents; QString restoredTimezone;
    QVERIFY2(backupcodec::fromJson(root, &restored, &restoredEvents, &restoredTimezone, &error), qPrintable(error));
    QCOMPARE(restoredTimezone, QStringLiteral("UTC")); QCOMPARE(restored.entities.size(), 1); QCOMPARE(restored.localMeta.size(), 0);
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
}

void TaskTests::backupRoundTripAndLegacyRejection()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("文件夹 Ω")); const QString list = store.createList(QStringLiteral("清单 🐱"), folder);
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
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 2); QCOMPARE(frames.size(), 1);
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
