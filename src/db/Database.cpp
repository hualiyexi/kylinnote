#include "Database.h"

#include "../Common.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTextDocument>
#include <QVariant>

namespace kn {
namespace {
const int kSchemaVersion = 2;

const char *const kNotesSchema =
    "CREATE TABLE IF NOT EXISTS notes ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL DEFAULT '',"
    "content TEXT NOT NULL DEFAULT '', color INTEGER NOT NULL DEFAULT 0,"
    "geometry TEXT NOT NULL DEFAULT '', z_order INTEGER NOT NULL DEFAULT 0,"
    "created_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT '',"
    "deleted INTEGER NOT NULL DEFAULT 0)";

// 保留旧 notes，避免改变旧便签的任何原始字节。task_groups/tasks 的外键仅用于新模型。
const char *const kV2Schema[] = {
    "CREATE TABLE task_groups (id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL CHECK(length(trim(name)) > 0))",
    "CREATE TABLE tasks ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL DEFAULT '',"
    "description TEXT NOT NULL DEFAULT '', content_format INTEGER NOT NULL DEFAULT 0 CHECK(content_format IN (0,1)),"
    "group_id INTEGER REFERENCES task_groups(id) ON DELETE SET NULL,"
    "priority INTEGER NOT NULL DEFAULT 0 CHECK(priority BETWEEN 0 AND 2),"
    "completed INTEGER NOT NULL DEFAULT 0 CHECK(completed IN (0,1)),"
    "favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN (0,1)),"
    "pinned INTEGER NOT NULL DEFAULT 0 CHECK(pinned IN (0,1)),"
    "deleted INTEGER NOT NULL DEFAULT 0 CHECK(deleted IN (0,1)),"
    "start_at TEXT, due_at TEXT, reminder_at TEXT, notified_at TEXT,"
    "created_at TEXT, updated_at TEXT, completed_at TEXT,"
    "recurrence TEXT NOT NULL DEFAULT 'none' CHECK(recurrence IN ('none','daily','weekly','monthly')),"
    "tags TEXT NOT NULL DEFAULT '[]', checklist TEXT NOT NULL DEFAULT '[]',"
    "color INTEGER NOT NULL DEFAULT 0 CHECK(color BETWEEN 0 AND 5),"
    "geometry TEXT NOT NULL DEFAULT '', z_order INTEGER NOT NULL DEFAULT 0,"
    "layer INTEGER NOT NULL DEFAULT 1 CHECK(layer BETWEEN 0 AND 2),"
    "revision INTEGER NOT NULL DEFAULT 1,"
    "recurrence_anchor_day INTEGER NOT NULL DEFAULT 0 CHECK(recurrence_anchor_day BETWEEN 0 AND 31))",
    // 持久生成凭证。next_id 为 NULL 表示实例已永久删除，仍阻止同一来源再次生成。
    "CREATE TABLE task_recurrence_links ("
    "source_id INTEGER PRIMARY KEY REFERENCES tasks(id) ON DELETE CASCADE,"
    "next_id INTEGER UNIQUE REFERENCES tasks(id) ON DELETE SET NULL,"
    "next_revision INTEGER NOT NULL)",
    "CREATE INDEX idx_tasks_group ON tasks(group_id)",
    "CREATE INDEX idx_tasks_reminder ON tasks(deleted,completed,reminder_at)",
    "CREATE INDEX idx_tasks_deleted ON tasks(deleted)"
};
}

const char *Database::kConnectionName = "kylinnote_main";
Database::Database() = default;

Database &Database::instance()
{
    static Database database;
    return database;
}

bool Database::fail(const QString &message, QString *error)
{
    m_lastError = message;
    if (error) {
        *error = message;
    }
    return false;
}

bool Database::open(QString *error)
{
    if (isOpen()) {
        if (error) error->clear();
        return true;
    }
    if (ensureDataDir().isEmpty()) {
        return fail(QStringLiteral("无法创建数据目录: ") + dataDirPath(), error);
    }
    return openAt(databasePath(), error);
}

bool Database::openAt(const QString &path, QString *error)
{
    if (isOpen() && m_path == path) {
        if (error) error->clear();
        return true;
    }
    close();
    if (path.trimmed().isEmpty()) {
        return fail(QStringLiteral("数据库路径不能为空"), error);
    }

    QString failure;
    {
        QSqlDatabase conn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                      QLatin1String(kConnectionName));
        conn.setDatabaseName(path);
        if (!conn.open()) {
            failure = QStringLiteral("打开数据库失败: ") + conn.lastError().text();
        } else {
            // 先拒绝未来版本，再修改 journal_mode 或任何 schema。
            QSqlQuery query(conn);
            if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
                failure = QStringLiteral("读取 user_version 失败: ") + query.lastError().text();
            } else {
                const int version = query.value(0).toInt();
                query.finish();
                if (version > kSchemaVersion || version < 0) {
                    failure = QStringLiteral("数据库版本 %1 高于当前支持版本 %2，请使用较新版本程序")
                                  .arg(version).arg(kSchemaVersion);
                } else if (!applyPragmas(&failure) || !migrate(version, kSchemaVersion, &failure)) {
                    // 具体错误由被调用者提供；连接在本作用域退出后统一关闭/移除。
                }
            }
        }
    }
    if (!failure.isEmpty()) {
        close();
        return fail(failure, error);
    }
    m_path = path;
    m_ready = true;
    m_lastError.clear();
    if (error) error->clear();
    return true;
}

void Database::close()
{
    m_ready = false;
    m_path.clear();
    if (!QSqlDatabase::contains(QLatin1String(kConnectionName))) {
        return;
    }
    {
        QSqlDatabase conn = QSqlDatabase::database(QLatin1String(kConnectionName), false);
        conn.close();
    }
    // 调用者须在 close/openAt 前释放 repository/store 中的连接副本。
    QSqlDatabase::removeDatabase(QLatin1String(kConnectionName));
}

bool Database::isOpen() const
{
    return m_ready && QSqlDatabase::contains(QLatin1String(kConnectionName))
        && QSqlDatabase::database(QLatin1String(kConnectionName), false).isOpen();
}

QSqlDatabase Database::connection() const
{
    // 迁移期间也要取得连接；对外成功状态只能由 isOpen 判断。
    return QSqlDatabase::database(QLatin1String(kConnectionName), false);
}

bool Database::applyPragmas(QString *error)
{
    const char *const pragmas[] = {
        "PRAGMA busy_timeout = 5000", "PRAGMA foreign_keys = ON",
        "PRAGMA journal_mode = WAL", "PRAGMA synchronous = NORMAL"
    };
    for (const char *sql : pragmas) {
        QSqlQuery query(connection());
        if (!query.exec(QLatin1String(sql))) {
            return fail(QStringLiteral("执行 %1 失败: %2")
                        .arg(QLatin1String(sql), query.lastError().text()), error);
        }
    }
    return true;
}

bool Database::migrate(int from, int to, QString *error)
{
    if (from == to) return true;
    QSqlDatabase conn = connection();
    if (!conn.transaction()) {
        return fail(QStringLiteral("开始数据库迁移失败: ") + conn.lastError().text(), error);
    }
    QString failure;
    const auto execute = [&conn, &failure](const QString &sql) {
        QSqlQuery query(conn);
        if (!query.exec(sql)) {
            failure = QStringLiteral("数据库迁移失败: ") + query.lastError().text();
            return false;
        }
        return true;
    };
    bool ok = true;
    if (from == 0) {
        ok = execute(QLatin1String(kNotesSchema))
          && execute(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_notes_deleted ON notes(deleted)"));
    }
    if (ok && from < 2) {
        for (const char *sql : kV2Schema) {
            if (!(ok = execute(QLatin1String(sql)))) break;
        }
        if (ok) {
            QSqlQuery notes(conn);
            ok = notes.exec(QStringLiteral("SELECT id,title,content,color,geometry,z_order,created_at,updated_at,deleted FROM notes ORDER BY id"));
            if (!ok) failure = QStringLiteral("读取旧便签失败: ") + notes.lastError().text();
            QSqlQuery insert(conn);
            if (ok) {
                ok = insert.prepare(QStringLiteral(
                    "INSERT INTO tasks(id,title,description,content_format,pinned,deleted,color,geometry,z_order,created_at,updated_at)"
                    " VALUES(?,?,?,0,?,?,?,?,?,?,?)"));
                if (!ok) failure = insert.lastError().text();
            }
            while (ok && notes.next()) {
                QTextDocument document;
                document.setHtml(notes.value(2).toString());
                QString title = notes.value(1).toString();
                if (title.trimmed().isEmpty()) {
                    title = document.toPlainText().section(QLatin1Char('\n'), 0, 0).trimmed().left(60);
                    if (title.isEmpty()) title = QStringLiteral("未命名便签");
                }
                insert.bindValue(0, notes.value(0));
                insert.bindValue(1, title);
                insert.bindValue(2, document.toPlainText().isNull() ? QStringLiteral("") : document.toPlainText());
                insert.bindValue(3, notes.value(8).toInt() == 0 ? 1 : 0);
                insert.bindValue(4, notes.value(8).toInt() == 0 ? 0 : 1);
                insert.bindValue(5, notes.value(3));
                insert.bindValue(6, notes.value(4));
                insert.bindValue(7, notes.value(5));
                insert.bindValue(8, notes.value(6));
                insert.bindValue(9, notes.value(7));
                if (!insert.exec()) {
                    ok = false;
                    failure = QStringLiteral("导入旧便签失败: ") + insert.lastError().text();
                }
            }
            if (ok && notes.lastError().isValid()) {
                ok = false;
                failure = notes.lastError().text();
            }
        }
    }
    if (ok) ok = execute(QStringLiteral("PRAGMA user_version = %1").arg(to));
    if (ok && !conn.commit()) {
        ok = false;
        failure = QStringLiteral("提交迁移失败: ") + conn.lastError().text();
    }
    if (!ok) {
        conn.rollback();
        return fail(failure, error);
    }
    return true;
}

} // namespace kn
