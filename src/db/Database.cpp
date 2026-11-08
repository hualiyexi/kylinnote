#include "Database.h"

#include "../Common.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace kl {
namespace {

const char *const kSchema[] = {
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
}

const char *Database::kConnectionName = "kylinlist_main";
Database::Database() = default;
Database &Database::instance() { static Database database; return database; }
bool Database::fail(const QString &message, QString *error)
{
    m_lastError = message; if (error) *error = message; return false;
}
bool Database::open(QString *error)
{
    if (isOpen()) { if (error) error->clear(); return true; }
    if (ensureDataDir().isEmpty()) return fail(QStringLiteral("无法创建数据目录: ") + dataDirPath(), error);
    return openAt(databasePath(), error);
}
bool Database::openAt(const QString &path, QString *error)
{
    if (isOpen() && m_path == path) { if (error) error->clear(); return true; }
    close(); if (path.trimmed().isEmpty()) return fail(QStringLiteral("数据库路径不能为空"), error);
    QString failure;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kConnectionName));
        database.setDatabaseName(path);
        if (!database.open()) failure = QStringLiteral("打开数据库失败: ") + database.lastError().text();
        else {
            QSqlQuery versionQuery(database), applicationQuery(database), objectsQuery(database);
            if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()
                || !applicationQuery.exec(QStringLiteral("PRAGMA application_id")) || !applicationQuery.next()
                || !objectsQuery.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")) || !objectsQuery.next()) {
                failure = QStringLiteral("读取数据库标识失败");
            } else {
                const int version = versionQuery.value(0).toInt(), application = applicationQuery.value(0).toInt(), objects = objectsQuery.value(0).toInt();
                versionQuery.finish(); applicationQuery.finish(); objectsQuery.finish();
                if (version == 0 && application == 0 && objects == 0) {
                    if (!applyPragmas(&failure) || !initializeSchema(&failure)) {}
                } else if (version != schemaVersion() || application != applicationId()) {
                    failure = QStringLiteral("不是受支持的 KylinList v5 数据库；拒绝迁移或改写旧数据");
                } else if (!applyPragmas(&failure)) {}
            }
        }
    }
    if (!failure.isEmpty()) { close(); return fail(failure, error); }
    m_path = path; m_ready = true; m_lastError.clear(); if (error) error->clear(); return true;
}
void Database::close()
{
    m_ready = false; m_path.clear(); if (!QSqlDatabase::contains(QLatin1String(kConnectionName))) return;
    { QSqlDatabase database = QSqlDatabase::database(QLatin1String(kConnectionName), false); database.close(); }
    QSqlDatabase::removeDatabase(QLatin1String(kConnectionName));
}
bool Database::isOpen() const
{
    return m_ready && QSqlDatabase::contains(QLatin1String(kConnectionName))
        && QSqlDatabase::database(QLatin1String(kConnectionName), false).isOpen();
}
QSqlDatabase Database::connection() const { return QSqlDatabase::database(QLatin1String(kConnectionName), false); }
bool Database::applyPragmas(QString *error)
{
    const char *const pragmas[] = {"PRAGMA busy_timeout=5000", "PRAGMA foreign_keys=ON", "PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL"};
    for (const char *sql : pragmas) {
        QSqlQuery query(connection());
        if (!query.exec(QLatin1String(sql))) return fail(QStringLiteral("执行 %1 失败: %2").arg(QLatin1String(sql), query.lastError().text()), error);
        if (qstrcmp(sql, "PRAGMA journal_mode=WAL") == 0 && query.next()
            && query.value(0).toString().compare(QStringLiteral("wal"), Qt::CaseInsensitive) != 0) return fail(QStringLiteral("SQLite 未进入 WAL 模式"), error);
    }
    return true;
}
bool Database::initializeSchema(QString *error)
{
    QSqlDatabase database = connection();
    if (!database.transaction()) return fail(QStringLiteral("开始初始化 schema 失败: ") + database.lastError().text(), error);
    QString failure;
    for (const char *sql : kSchema) {
        QSqlQuery query(database); if (!query.exec(QLatin1String(sql))) { failure = QStringLiteral("创建 v5 schema 失败: ") + query.lastError().text(); break; }
    }
    const QString inboxId = newUuid(), now = QString::number(utcMs(utcNow()));
    if (failure.isEmpty()) {
        QSqlQuery inbox(database);
        inbox.prepare(QStringLiteral("INSERT INTO lists(id,folder_id,kind,name,is_inbox,sort_order,created_at_utc_ms,updated_at_utc_ms) VALUES(?,NULL,'task','收集箱',1,0,?,?)"));
        inbox.addBindValue(inboxId); inbox.addBindValue(now); inbox.addBindValue(now);
        if (!inbox.exec()) failure = QStringLiteral("创建 inbox 失败: ") + inbox.lastError().text();
    }
    if (failure.isEmpty()) {
        const QString keys[] = {QStringLiteral("installation_id"),QStringLiteral("device_id"),QStringLiteral("signed_in"),
            QStringLiteral("default_timezone"),QStringLiteral("inbox_list_id"),QStringLiteral("schema_created_at_utc_ms"),
            QStringLiteral("sync_base_server_revision")};
        const QString values[] = {newUuid(),newUuid(),QStringLiteral("0"),defaultTimeZone(),inboxId,now,QStringLiteral("0")};
        QSqlQuery meta(database); meta.prepare(QStringLiteral("INSERT INTO local_meta(key,value) VALUES(?,?)"));
        for (int i = 0; i < 7 && failure.isEmpty(); ++i) {
            meta.bindValue(0, keys[i]); meta.bindValue(1, values[i]);
            if (!meta.exec()) failure = QStringLiteral("初始化安装元数据失败: ") + meta.lastError().text();
        }
    }
    QSqlQuery versions(database);
    if (failure.isEmpty() && (!versions.exec(QStringLiteral("PRAGMA user_version=5"))
                              || !versions.exec(QStringLiteral("PRAGMA application_id=%1").arg(applicationId()))))
        failure = QStringLiteral("写入数据库标识失败: ") + versions.lastError().text();
    QSqlQuery foreign(database);
    if (failure.isEmpty() && (!foreign.exec(QStringLiteral("PRAGMA foreign_key_check")) || foreign.next())) failure = QStringLiteral("新 schema 外键检查失败");
    if (!failure.isEmpty() || !database.commit()) {
        if (failure.isEmpty()) failure = database.lastError().text(); database.rollback(); return fail(failure, error);
    }
    return true;
}

} // namespace kl
