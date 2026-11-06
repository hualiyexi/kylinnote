#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "db/TaskRepository.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace kn;

namespace {
QDateTime utc(int year, int month, int day, int hour = 10, int minute = 0)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute), Qt::UTC);
}

const QString kLegacySchema = QStringLiteral(
    "CREATE TABLE notes(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL DEFAULT '',"
    "content TEXT NOT NULL DEFAULT '',color INTEGER NOT NULL DEFAULT 0,geometry TEXT NOT NULL DEFAULT '',"
    "z_order INTEGER NOT NULL DEFAULT 0,created_at TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL DEFAULT '',deleted INTEGER NOT NULL DEFAULT 0)");

bool seed(const QString &path, const QStringList &sql)
{
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("test_seed"));
        db.setDatabaseName(path);
        ok = db.open();
        for (const QString &statement : sql) {
            if (!ok) break;
            QSqlQuery query(db);
            ok = query.exec(statement);
            if (!ok) qWarning() << query.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("test_seed"));
    return ok;
}

QVariant inspect(const QString &path, const QString &sql)
{
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("test_inspect"));
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) value = query.value(0);
            else qWarning() << query.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("test_inspect"));
    return value;
}

bool execute(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql)) return true;
    qWarning() << query.lastError().text();
    return false;
}

QVariant scalar(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql) && query.next()) return query.value(0);
    qWarning() << query.lastError().text();
    return QVariant();
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson();
    return file.write(bytes) == bytes.size();
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

qint64 otherTask(const TaskStore &store, qint64 excluded)
{
    for (const Task &task : store.tasks()) if (task.id != excluded) return task.id;
    return 0;
}
}

class TaskTests : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void migrateV1PreservesSource();
    void migrationRollback();
    void rejectsFutureVersionAndClosesFailures();
    void taskAndGroupCrud();
    void validation_data();
    void validation();
    void transactionalRollback();
    void recurrenceMonthEnd_data();
    void recurrenceMonthEnd();
    void recurrenceUndoAndDuplicateProtection();
    void recurrenceDailyAndWeekly();
    void remindersOverdueRestartCompletedDeleted();
    void reminderFailureClockChangeAndSnooze();
    void backupUnicodeHtmlChecklistAndMerge();
    void backupInvalidImportIsAtomic();
    void backupSqlFailureRollsBack();
    void backupRecurrenceUndo();

private:
    bool open();
    QString dbPath() const { return m_directory->filePath(QStringLiteral("tasks.db")); }
    QString backupPath() const { return m_directory->filePath(QStringLiteral("备份.json")); }
    std::unique_ptr<QTemporaryDir> m_directory;
};

void TaskTests::init()
{
    Database::instance().close();
    m_directory.reset(new QTemporaryDir);
    QVERIFY(m_directory->isValid());
}

void TaskTests::cleanup()
{
    Database::instance().close();
    m_directory.reset();
}

bool TaskTests::open()
{
    QString error;
    const bool ok = Database::instance().openAt(dbPath(), &error);
    if (!ok) qWarning() << error;
    return ok;
}

void TaskTests::migrateV1PreservesSource()
{
    QVERIFY(seed(dbPath(), {kLegacySchema, QStringLiteral("PRAGMA user_version=1"),
        QStringLiteral("INSERT INTO notes VALUES(7,'旧便签','<p>你好 <b>世界</b></p>',3,'-20,40,240,260',12,'2024-01-01T02:03:04Z','2024-02-02T03:04:05Z',0)"),
        QStringLiteral("INSERT INTO notes VALUES(9,'已删除','<p>回收站原文</p>',2,'8,9,160,120',4,'2023-01-01T00:00:00Z','2024-01-01T00:00:00Z',1)")}));
    QVERIFY(open());
    QVERIFY(Database::instance().isOpen());
    {
        TaskStore store(Database::instance().connection());
        QVERIFY(store.load());
        QCOMPARE(store.tasks().size(), 2);
        const Task task = store.task(7);
        QCOMPARE(task.title, QStringLiteral("旧便签"));
        QCOMPARE(task.description, QStringLiteral("你好 世界"));
        QCOMPARE(int(task.contentFormat), int(ContentFormat::Markdown));
        QCOMPARE(int(task.color), int(NoteColor::Pink));
        QCOMPARE(task.geometry, QRect(-20, 40, 240, 260));
        QCOMPARE(task.zOrder, 12);
        QCOMPARE(task.createdAt, utc(2024, 1, 1, 2, 3).addSecs(4));
        QCOMPARE(task.updatedAt, utc(2024, 2, 2, 3, 4).addSecs(5));
        QVERIFY(task.pinned);
        QVERIFY(!task.deleted);
        QVERIFY(store.task(9).deleted);
        QVERIFY(!store.task(9).pinned);
        QCOMPARE(scalar(QStringLiteral("SELECT content FROM notes WHERE id=7")).toString(), QStringLiteral("<p>你好 <b>世界</b></p>"));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM notes")).toInt(), 2);
        QCOMPARE(scalar(QStringLiteral("PRAGMA user_version")).toInt(), 2);
    }
    Database::instance().close();
    QVERIFY(open());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tasks")).toInt(), 2); // 再开库不会再次导入
}

void TaskTests::migrationRollback()
{
    QVERIFY(seed(dbPath(), {kLegacySchema, QStringLiteral("PRAGMA user_version=1"),
                           QStringLiteral("CREATE TABLE tasks(conflict TEXT)"),
                           QStringLiteral("INSERT INTO notes(title,content) VALUES('原件','<b>不能丢失</b>')")}));
    QString error;
    QVERIFY(!Database::instance().openAt(dbPath(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!Database::instance().isOpen());
    QCOMPARE(inspect(dbPath(), QStringLiteral("PRAGMA user_version")).toInt(), 1);
    QCOMPARE(inspect(dbPath(), QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='task_groups'")).toInt(), 0);
    QCOMPARE(inspect(dbPath(), QStringLiteral("SELECT content FROM notes")).toString(), QStringLiteral("<b>不能丢失</b>"));
    QVERIFY(seed(dbPath(), {QStringLiteral("DROP TABLE tasks")}));
    QVERIFY(open()); // 失败连接已释放，可正常重试
}

void TaskTests::rejectsFutureVersionAndClosesFailures()
{
    QVERIFY(seed(dbPath(), {QStringLiteral("PRAGMA user_version=99"), QStringLiteral("CREATE TABLE future_data(value TEXT)")}));
    QString error;
    QVERIFY(!Database::instance().openAt(dbPath(), &error));
    QVERIFY(!Database::instance().isOpen());
    QVERIFY(!error.isEmpty());
    QCOMPARE(inspect(dbPath(), QStringLiteral("PRAGMA user_version")).toInt(), 99);
    QCOMPARE(inspect(dbPath(), QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='tasks'")).toInt(), 0);
    QVERIFY(!Database::instance().openAt(m_directory->filePath(QStringLiteral("missing/invalid.db")), &error));
    QVERIFY(!Database::instance().isOpen());
    QVERIFY(!Database::instance().openAt(QStringLiteral(""), &error));
    QVERIFY(!Database::instance().isOpen());
    QVERIFY(Database::instance().openAt(m_directory->filePath(QStringLiteral("good.db")), &error));
    QVERIFY(error.isEmpty());
}

void TaskTests::taskAndGroupCrud()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    QSignalSpy changed(&store, &TaskStore::taskChanged);
    QSignalSpy groupsChanged(&store, &TaskStore::groupsChanged);
    const qint64 group = store.createGroup(QStringLiteral(" 工作 "));
    QVERIFY(group > 0);
    QCOMPARE(store.groups().first().name, QStringLiteral("工作"));
    const qint64 first = store.createTask(QStringLiteral("做测试"), group);
    const qint64 second = store.createTask(QStringLiteral("回收站也要移组"), group);
    QVERIFY(first > 0 && second > first);
    Task task = store.task(first);
    task.description = QStringLiteral("**正文**");
    task.tags = {QStringLiteral("工作"), QStringLiteral("Qt")};
    QVERIFY(store.saveTask(task));
    QVERIFY(store.setPinned(first, true));
    QVERIFY(store.task(first).pinned);
    QVERIFY(store.setCompleted(first, true));
    QVERIFY(store.task(first).completedAt.isValid());
    QVERIFY(store.setCompleted(first, false));
    QVERIFY(!store.task(first).completedAt.isValid());
    QVERIFY(store.trashTask(second));
    QVERIFY(store.renameGroup(group, QStringLiteral("新版工作")));
    QVERIFY(store.deleteGroup(group));
    QVERIFY(store.groups().isEmpty());
    QCOMPARE(store.task(first).groupId, qint64(0));
    QCOMPARE(store.task(second).groupId, qint64(0));
    QVERIFY(store.task(second).deleted);
    QVERIFY(store.restoreTask(second));
    QVERIFY(!store.task(second).deleted);
    QVERIFY(!store.deletePermanently(second));
    QVERIFY(store.trashTask(second));
    QVERIFY(store.deletePermanently(second));
    QCOMPARE(store.task(second).id, qint64(0));
    QVERIFY(!store.trashTask(9999));
    QVERIFY(!store.renameGroup(0, QStringLiteral("未分组")));
    QVERIFY(!store.createGroup(QStringLiteral("  ")));
    QVERIFY(changed.count() > 0);
    QVERIFY(groupsChanged.count() >= 3);
    TaskStore reloaded(Database::instance().connection());
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.tasks().size(), 1);
    QCOMPARE(reloaded.task(first).description, QStringLiteral("**正文**"));
    QCOMPARE(reloaded.task(first).tags.size(), 2);
}

void TaskTests::validation_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("blank title") << 0;
    QTest::newRow("priority") << 1;
    QTest::newRow("content format") << 2;
    QTest::newRow("color") << 3;
    QTest::newRow("layer") << 4;
    QTest::newRow("start after due") << 5;
    QTest::newRow("recurrence rule") << 6;
    QTest::newRow("recurrence without plan") << 7;
    QTest::newRow("nonexistent group") << 8;
    QTest::newRow("invalid date") << 9;
    QTest::newRow("negative id") << 10;
}

void TaskTests::validation()
{
    QFETCH(int, kind);
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 id = store.createTask(QStringLiteral("有效标题"));
    QVERIFY(id);
    Task task = store.task(id);
    switch (kind) {
    case 0: task.title = QStringLiteral(" \n\t"); break;
    case 1: task.priority = 3; break;
    case 2: task.contentFormat = static_cast<ContentFormat>(8); break;
    case 3: task.color = static_cast<NoteColor>(8); break;
    case 4: task.layer = static_cast<NoteLayer>(8); break;
    case 5: task.startAt = utc(2026, 3, 2); task.dueAt = utc(2026, 3, 1); break;
    case 6: task.recurrence = QStringLiteral("yearly"); break;
    case 7: task.recurrence = QStringLiteral("daily"); break;
    case 8: task.groupId = 987; break;
    case 9: task.dueAt = QDateTime(QDate(), QTime(12, 0), Qt::UTC); break;
    case 10: task.id = -1; break;
    }
    QSignalSpy changed(&store, &TaskStore::taskChanged);
    QSignalSpy reset(&store, &TaskStore::tasksReset);
    QSignalSpy errors(&store, &TaskStore::errorOccurred);
    QVERIFY(!store.saveTask(task));
    QVERIFY(!store.lastError().isEmpty());
    QCOMPARE(changed.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(store.task(id).title, QStringLiteral("有效标题"));
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM tasks WHERE id=%1").arg(id)).toString(), QStringLiteral("有效标题"));
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM tasks WHERE id=%1").arg(id)).toInt(), 1);
}

void TaskTests::transactionalRollback()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 group = store.createGroup(QStringLiteral("组"));
    const qint64 id = store.createTask(QStringLiteral("循环"), group);
    Task task = store.task(id);
    task.dueAt = utc(2026, 1, 31);
    task.recurrence = QStringLiteral("monthly");
    QVERIFY(store.saveTask(task));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_next BEFORE INSERT ON tasks BEGIN SELECT RAISE(ABORT,'injected insert failure'); END")));
    QSignalSpy changed(&store, &TaskStore::taskChanged);
    QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY(!store.setCompleted(id, true));
    QVERIFY(!store.task(id).completed);
    QCOMPARE(store.tasks().size(), 1);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT completed FROM tasks WHERE id=%1").arg(id)).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM task_recurrence_links")).toInt(), 0);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_next")));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_group BEFORE DELETE ON task_groups BEGIN SELECT RAISE(ABORT,'injected delete failure'); END")));
    QVERIFY(!store.deleteGroup(group));
    QCOMPARE(store.task(id).groupId, group);
    QCOMPARE(scalar(QStringLiteral("SELECT group_id FROM tasks WHERE id=%1").arg(id)).toLongLong(), group);
    QCOMPARE(store.groups().size(), 1);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_group")));
    // 乐观 revision 保护：陈旧 store 不能覆盖另外一个 store 已提交的修改。
    TaskStore stale(Database::instance().connection());
    QVERIFY(stale.load());
    task = store.task(id);
    task.title = QStringLiteral("更新过的标题");
    QVERIFY(store.saveTask(task));
    Task staleTask = stale.task(id);
    staleTask.title = QStringLiteral("不能覆盖");
    QVERIFY(!stale.saveTask(staleTask));
    QCOMPARE(scalar(QStringLiteral("SELECT title FROM tasks WHERE id=%1").arg(id)).toString(), QStringLiteral("更新过的标题"));
}

void TaskTests::recurrenceMonthEnd_data()
{
    QTest::addColumn<int>("year");
    QTest::addColumn<int>("day");
    QTest::addColumn<int>("februaryDay");
    QTest::newRow("January 31 non-leap") << 2025 << 31 << 28;
    QTest::newRow("January 30 anchor") << 2025 << 30 << 28;
    QTest::newRow("January 31 leap") << 2024 << 31 << 29;
}

void TaskTests::recurrenceMonthEnd()
{
    QFETCH(int, year);
    QFETCH(int, day);
    QFETCH(int, februaryDay);
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 id = store.createTask(QStringLiteral("月末"));
    Task task = store.task(id);
    task.startAt = utc(year, 1, day - 1, 9);
    task.dueAt = utc(year, 1, day, 10);
    task.reminderAt = utc(year, 1, day, 9, 30);
    task.recurrence = QStringLiteral("monthly");
    ChecklistItem item;
    item.text = QStringLiteral("子步骤");
    item.done = true;
    task.checklist.append(item);
    QVERIFY(store.saveTask(task));
    QVERIFY(store.setCompleted(id, true));
    QCOMPARE(store.tasks().size(), 2);
    const qint64 february = otherTask(store, id);
    QVERIFY(february);
    const Task next = store.task(february);
    QCOMPARE(next.dueAt, utc(year, 2, februaryDay, 10));
    QCOMPARE(next.startAt, next.dueAt.addSecs(-25 * 3600));
    QCOMPARE(next.reminderAt, next.dueAt.addSecs(-30 * 60));
    QVERIFY(!next.checklist.first().done);
    QVERIFY(!next.completed && !next.notifiedAt.isValid());
    QVERIFY(store.task(id).completed);
    QVERIFY(store.setCompleted(february, true));
    qint64 march = 0;
    for (const Task &value : store.tasks()) if (!value.completed) march = value.id;
    QVERIFY(march);
    QCOMPARE(store.task(march).dueAt, utc(year, 3, day, 10));
}

void TaskTests::recurrenceUndoAndDuplicateProtection()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 id = store.createTask(QStringLiteral("重复和撤销"));
    Task task = store.task(id);
    task.dueAt = utc(2025, 1, 31);
    task.recurrence = QStringLiteral("monthly");
    QVERIFY(store.saveTask(task));
    QVERIFY(store.setCompleted(id, true));
    const qint64 firstChild = otherTask(store, id);
    QVERIFY(store.setCompleted(id, true));
    QVERIFY(store.saveTask(store.task(id)));
    QCOMPARE(store.tasks().size(), 2);
    QVERIFY(store.load());
    QVERIFY(store.setCompleted(id, true));
    QCOMPARE(store.tasks().size(), 2);
    QVERIFY(store.setCompleted(id, false));
    QCOMPARE(store.tasks().size(), 1);
    QCOMPARE(store.task(firstChild).id, qint64(0));
    QVERIFY(!store.task(id).completed);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM task_recurrence_links")).toInt(), 0);
    QVERIFY(store.setCompleted(id, true));
    const qint64 child = otherTask(store, id);
    QVERIFY(child != firstChild);
    Task edited = store.task(child);
    edited.title = QStringLiteral("后续已编辑，必须保留");
    QVERIFY(store.saveTask(edited));
    QSignalSpy changed(&store, &TaskStore::taskChanged);
    QVERIFY(!store.setCompleted(id, false));
    QVERIFY(store.task(id).completed);
    QCOMPARE(store.task(child).title, edited.title);
    QCOMPARE(changed.count(), 0);
    QVERIFY(store.trashTask(child));
    QVERIFY(store.deletePermanently(child));
    QVERIFY(!store.setCompleted(id, false));
    QVERIFY(store.setCompleted(id, true));
    QCOMPARE(store.tasks().size(), 1); // 凭证在，删除后也不会再次生成
}

void TaskTests::recurrenceDailyAndWeekly()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    for (const QString &rule : {QStringLiteral("daily"), QStringLiteral("weekly")}) {
        const qint64 id = store.createTask(rule);
        Task task = store.task(id);
        task.startAt = utc(2020, 1, 1, 23, 15); // 很久之前：必须从计划时间递增而不是从今天算
        task.reminderAt = task.startAt.addSecs(-120);
        task.recurrence = rule;
        QVERIFY(store.saveTask(task));
        QVERIFY(store.setCompleted(id, true));
        Task next;
        for (const Task &candidate : store.tasks())
            if (candidate.title == rule && !candidate.completed) next = candidate;
        QVERIFY(next.id);
        QCOMPARE(next.startAt, task.startAt.addDays(rule == QStringLiteral("daily") ? 1 : 7));
        QVERIFY(!next.dueAt.isValid());
        QCOMPARE(next.reminderAt, next.startAt.addSecs(-120));
    }
}

void TaskTests::remindersOverdueRestartCompletedDeleted()
{
    QVERIFY(open());
    const QDateTime now = QDateTime::currentDateTimeUtc();
    qint64 due = 0;
    {
        TaskStore store(Database::instance().connection());
        QVERIFY(store.load());
        due = store.createTask(QStringLiteral("补发"));
        const qint64 completed = store.createTask(QStringLiteral("完成不提醒"));
        const qint64 deleted = store.createTask(QStringLiteral("删除不提醒"));
        for (qint64 id : {due, completed, deleted}) {
            Task task = store.task(id);
            task.reminderAt = now.addDays(-2);
            QVERIFY(store.saveTask(task));
        }
        QVERIFY(store.setCompleted(completed, true));
        QVERIFY(store.trashTask(deleted));
        ReminderScheduler scheduler(&store);
        QSignalSpy reminders(&scheduler, &ReminderScheduler::reminderDue);
        scheduler.start(); // 启动立即补发，而不是等待 timer
        QCOMPARE(reminders.count(), 1);
        QCOMPARE(reminders.first().first().toLongLong(), due);
        QVERIFY(store.task(due).notifiedAt >= store.task(due).reminderAt);
        scheduler.checkDue(now.addDays(1));
        QCOMPARE(reminders.count(), 1);
        scheduler.stop();
    }
    Database::instance().close();
    QVERIFY(open());
    TaskStore restarted(Database::instance().connection());
    QVERIFY(restarted.load());
    ReminderScheduler scheduler(&restarted);
    QSignalSpy reminders(&scheduler, &ReminderScheduler::reminderDue);
    scheduler.checkDue(now.addDays(3));
    QCOMPARE(reminders.count(), 0);
    QVERIFY(restarted.task(due).notifiedAt.isValid());
}

void TaskTests::reminderFailureClockChangeAndSnooze()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 id = store.createTask(QStringLiteral("提醒可靠性"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    Task task = store.task(id);
    task.dueAt = now.addDays(-1); // 逾期任务也允许 snooze 到截止时间之后
    task.reminderAt = now.addSecs(-3600);
    QVERIFY(store.saveTask(task));
    ReminderScheduler scheduler(&store);
    QSignalSpy reminders(&scheduler, &ReminderScheduler::reminderDue);
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_notify BEFORE UPDATE OF notified_at ON tasks WHEN NEW.notified_at IS NOT NULL BEGIN SELECT RAISE(ABORT,'claim failure'); END")));
    scheduler.checkDue(now);
    QCOMPARE(reminders.count(), 0);
    QVERIFY(!store.task(id).notifiedAt.isValid());
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_notify")));
    scheduler.checkDue(now);
    QCOMPARE(reminders.count(), 1);
    // 新 reminder 比旧 claim 还早，时钟也回退：必须清 claim，不能把这次提醒漏掉。
    task = store.task(id);
    task.reminderAt = now.addSecs(-7200);
    QVERIFY(store.saveTask(task));
    QVERIFY(!store.task(id).notifiedAt.isValid());
    scheduler.checkDue(now.addSecs(-1800));
    QCOMPARE(reminders.count(), 2);
    QVERIFY(!scheduler.snooze(id, 0));
    QVERIFY(!scheduler.snooze(id, -1));
    QVERIFY(!scheduler.snooze(99999));
    const QDateTime before = QDateTime::currentDateTimeUtc();
    QVERIFY(scheduler.snooze(id));
    const Task snoozed = store.task(id);
    QVERIFY(snoozed.reminderAt >= before.addSecs(10 * 60));
    QVERIFY(snoozed.reminderAt <= QDateTime::currentDateTimeUtc().addSecs(10 * 60));
    QVERIFY(!snoozed.notifiedAt.isValid());
    scheduler.checkDue(before);
    QCOMPARE(reminders.count(), 2);
    scheduler.checkDue(snoozed.reminderAt.addSecs(1));
    QCOMPARE(reminders.count(), 3);
    QVERIFY(store.setCompleted(id, true));
    QVERIFY(!scheduler.snooze(id));
    // 清空 reminderAt 也必须清除已有 claim，不会因旧 notifiedAt 拒绝保存。
    task = store.task(id);
    task.reminderAt = QDateTime();
    QVERIFY(store.saveTask(task));
    QVERIFY(!store.task(id).notifiedAt.isValid());
}

void TaskTests::backupUnicodeHtmlChecklistAndMerge()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 group = store.createGroup(QStringLiteral("项目 · 麒麟 🐱"));
    const qint64 id = store.createTask(QStringLiteral("中文与 emoji 📝"), group);
    Task task = store.task(id);
    task.description = QStringLiteral("<p><b>原样 HTML</b> &amp; Unicode Ω</p>");
    task.contentFormat = ContentFormat::Html;
    task.tags = {QStringLiteral("家庭"), QStringLiteral("标签 Ω")};
    ChecklistItem item;
    item.text = QStringLiteral("第一项 ☑");
    item.done = true;
    task.checklist.append(item);
    item.text = QStringLiteral("第二项");
    item.done = false;
    task.checklist.append(item);
    task.color = NoteColor::Purple;
    task.layer = NoteLayer::OnTop;
    task.pinned = true;
    task.favorite = true;
    task.geometry = QRect(-500, 40, 320, 240);
    task.zOrder = 12;
    task.startAt = utc(2026, 5, 1, 9);
    task.dueAt = utc(2026, 5, 1, 17);
    QVERIFY(store.saveTask(task));
    QVERIFY(store.exportBackup(backupPath()));
    const QJsonObject root = readJson(backupPath());
    QCOMPARE(root.value(QStringLiteral("format")).toString(), QStringLiteral("KylinNote.Tasks"));
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 1);
    QVERIFY(store.importBackup(backupPath()));
    QCOMPARE(store.tasks().size(), 2);
    QCOMPARE(store.groups().size(), 2);
    const qint64 imported = otherTask(store, id);
    const Task copy = store.task(imported);
    QVERIFY(imported != id && copy.groupId != group && copy.groupId > 0);
    QCOMPARE(copy.title, task.title);
    QCOMPARE(copy.description, task.description);
    QCOMPARE(int(copy.contentFormat), int(ContentFormat::Html));
    QCOMPARE(copy.tags, task.tags);
    QCOMPARE(copy.geometry, task.geometry);
    QCOMPARE(int(copy.color), int(task.color));
    QCOMPARE(int(copy.layer), int(task.layer));
    QCOMPARE(copy.zOrder, task.zOrder);
    QVERIFY(copy.pinned && copy.favorite);
    QCOMPARE(copy.checklist.size(), 2);
    QCOMPARE(copy.checklist.first().text, task.checklist.first().text);
    QVERIFY(copy.checklist.first().done && !copy.checklist.last().done);
    QCOMPARE(copy.startAt, task.startAt);
    QCOMPARE(copy.dueAt, task.dueAt);
    QCOMPARE(store.task(id).description, task.description); // 合并不覆盖现有行
}

void TaskTests::backupInvalidImportIsAtomic()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 group = store.createGroup(QStringLiteral("原分组"));
    const qint64 id = store.createTask(QStringLiteral("原任务"), group);
    QVERIFY(store.exportBackup(backupPath()));
    const QJsonObject valid = readJson(backupPath());
    const QString invalidPath = m_directory->filePath(QStringLiteral("invalid.json"));
    QSignalSpy changed(&store, &TaskStore::taskChanged);
    QSignalSpy reset(&store, &TaskStore::tasksReset);
    QSignalSpy groups(&store, &TaskStore::groupsChanged);
    for (int kind = 0; kind < 11; ++kind) {
        QJsonObject root = valid;
        QJsonArray tasks = root.value(QStringLiteral("tasks")).toArray();
        QJsonObject bad = tasks.first().toObject();
        bad.insert(QStringLiteral("id"), QStringLiteral("999"));
        switch (kind) {
        case 0: bad.insert(QStringLiteral("title"), true); break;
        case 1: bad.insert(QStringLiteral("priority"), 0.5); break;
        case 2: bad.insert(QStringLiteral("dueAt"), QStringLiteral("2026-02-30T10:00:00Z")); break;
        case 3: bad.insert(QStringLiteral("groupId"), QStringLiteral("8888")); break;
        case 4: bad.insert(QStringLiteral("tags"), QJsonObject()); break;
        case 5: { QJsonObject item; item.insert(QStringLiteral("text"), QStringLiteral("x")); item.insert(QStringLiteral("done"), QStringLiteral("true")); bad.insert(QStringLiteral("checklist"), QJsonArray{item}); break; }
        case 6: bad.insert(QStringLiteral("completed"), true); break;
        case 7: bad.insert(QStringLiteral("contentFormat"), 99); break;
        case 8: root.insert(QStringLiteral("version"), 9); break;
        case 9: bad.insert(QStringLiteral("id"), QString::number(id)); break;
        case 10: bad.insert(QStringLiteral("startAt"), QStringLiteral("2026-02-01T10:00:00")); break; // 缺时区
        }
        tasks.append(bad);
        root.insert(QStringLiteral("tasks"), tasks);
        QVERIFY(writeJson(invalidPath, root));
        QVERIFY2(!store.importBackup(invalidPath), qPrintable(QString::number(kind)));
        QCOMPARE(store.tasks().size(), 1);
        QCOMPARE(store.groups().size(), 1);
        QCOMPARE(store.task(id).title, QStringLiteral("原任务"));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tasks")).toInt(), 1);
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM task_groups")).toInt(), 1);
    }
    QCOMPARE(changed.count(), 0);
    QCOMPARE(reset.count(), 0);
    QCOMPARE(groups.count(), 0);
}

void TaskTests::backupSqlFailureRollsBack()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 group = store.createGroup(QStringLiteral("原分组"));
    const qint64 id = store.createTask(QStringLiteral("原任务"), group);
    QVERIFY(store.exportBackup(backupPath()));
    QJsonObject root = readJson(backupPath());
    QJsonArray tasks = root.value(QStringLiteral("tasks")).toArray();
    QJsonObject rejected = tasks.first().toObject();
    rejected.insert(QStringLiteral("id"), QStringLiteral("999"));
    rejected.insert(QStringLiteral("title"), QStringLiteral("reject-import"));
    tasks.append(rejected);
    root.insert(QStringLiteral("tasks"), tasks);
    QVERIFY(writeJson(backupPath(), root));
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_import BEFORE INSERT ON tasks WHEN NEW.title='reject-import' BEGIN SELECT RAISE(ABORT,'import failure'); END")));
    QSignalSpy reset(&store, &TaskStore::tasksReset);
    QVERIFY(!store.importBackup(backupPath()));
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM tasks")).toInt(), 1);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM task_groups")).toInt(), 1);
    QCOMPARE(store.tasks().size(), 1);
    QCOMPARE(store.task(id).groupId, group);
    QCOMPARE(reset.count(), 0);
}

void TaskTests::backupRecurrenceUndo()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY(store.load());
    const qint64 id = store.createTask(QStringLiteral("导入的重复凭证"));
    Task task = store.task(id);
    task.dueAt = utc(2026, 1, 31);
    task.recurrence = QStringLiteral("monthly");
    QVERIFY(store.saveTask(task));
    QVERIFY(store.setCompleted(id, true));
    const qint64 child = otherTask(store, id);
    QVERIFY(store.exportBackup(backupPath()));
    QVERIFY(store.importBackup(backupPath()));
    QCOMPARE(store.tasks().size(), 4);
    qint64 importedParent = 0;
    for (const Task &candidate : store.tasks()) if (candidate.id != id && candidate.completed) importedParent = candidate.id;
    QVERIFY(importedParent);
    QVERIFY(store.setCompleted(importedParent, false));
    QCOMPARE(store.tasks().size(), 3);
    QVERIFY(store.task(id).completed);
    QVERIFY(store.task(child).id);
    // 已编辑实例的保护在导出、合并并分配新 revision 后仍保留。
    Task edited = store.task(child);
    edited.description = QStringLiteral("编辑后禁止撤回");
    QVERIFY(store.saveTask(edited));
    QVERIFY(store.exportBackup(backupPath()));
    QVERIFY(store.importBackup(backupPath()));
    qint64 newestParent = 0;
    for (const Task &candidate : store.tasks()) if (candidate.completed && candidate.id != id) newestParent = candidate.id;
    QVERIFY(newestParent);
    QVERIFY(!store.setCompleted(newestParent, false));
    QVERIFY(store.task(newestParent).completed);
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    TaskTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_tasks.moc"
