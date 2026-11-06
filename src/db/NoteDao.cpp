#include "NoteDao.h"
#include "Database.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

namespace kn {

static const QString kSelectBase =
    QStringLiteral("SELECT id,title,content,color,geometry,z_order,created_at,updated_at,deleted "
                   "FROM notes");

// QString() 是 null 而非空串，bindValue 会将其绑定为 SQL NULL，
// 撞上 title/content 的 NOT NULL 约束。落库前统一归一化。
static inline QString safeText(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}

static NoteRecord recordFromQueryImpl(QSqlQuery &q)
{
    NoteRecord rec;
    rec.id        = q.value(0).toLongLong();
    rec.title     = q.value(1).toString();
    rec.content   = q.value(2).toString();
    rec.color     = colorFromInt(q.value(3).toInt());
    rec.geometry  = decodeGeometry(q.value(4).toString());
    rec.zOrder    = q.value(5).toInt();
    rec.createdAt = QDateTime::fromString(q.value(6).toString(), Qt::ISODate);
    rec.updatedAt = QDateTime::fromString(q.value(7).toString(), Qt::ISODate);
    rec.deleted   = q.value(8).toInt() != 0;
    return rec;
}

NoteRecord NoteDao::recordFromQuery(QSqlQuery &q)
{
    return recordFromQueryImpl(q);
}

QVector<NoteRecord> NoteDao::fetchAll(bool includeDeleted)
{
    QVector<NoteRecord> list;
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return list;
    }

    QSqlQuery q(conn);
    QString sql = kSelectBase;
    if (!includeDeleted) {
        sql += QStringLiteral(" WHERE deleted = 0");
    }
    sql += QStringLiteral(" ORDER BY z_order ASC, id ASC");

    if (!q.exec(sql)) {
        return list;
    }
    while (q.next()) {
        list.append(recordFromQueryImpl(q));
    }
    return list;
}

bool NoteDao::fetch(qint64 id, NoteRecord &out)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }
    QSqlQuery q(conn);
    q.prepare(kSelectBase + QStringLiteral(" WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), id);
    if (!q.exec() || !q.next()) {
        return false;
    }
    out = recordFromQueryImpl(q);
    return true;
}

bool NoteDao::insert(NoteRecord &rec)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }

    const QString stamp = nowStamp();
    QSqlQuery q(conn);
    q.prepare(QStringLiteral(
        "INSERT INTO notes (title,content,color,geometry,z_order,created_at,updated_at,deleted)"
        " VALUES (:title,:content,:color,:geometry,:z_order,:created_at,:updated_at,0)"));
    q.bindValue(QStringLiteral(":title"), safeText(rec.title));
    q.bindValue(QStringLiteral(":content"), safeText(rec.content));
    q.bindValue(QStringLiteral(":color"), colorToInt(rec.color));
    q.bindValue(QStringLiteral(":geometry"), encodeGeometry(rec.geometry));
    q.bindValue(QStringLiteral(":z_order"), rec.zOrder);
    q.bindValue(QStringLiteral(":created_at"), stamp);
    q.bindValue(QStringLiteral(":updated_at"), stamp);

    if (!q.exec()) {
        qWarning() << "[kylinnote] 新建便签写库失败:" << q.lastError().text();
        return false;
    }
    rec.id        = q.lastInsertId().toLongLong();
    rec.createdAt = QDateTime::fromString(stamp, Qt::ISODate);
    rec.updatedAt = rec.createdAt;
    return true;
}

bool NoteDao::update(const NoteRecord &rec)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }

    QSqlQuery q(conn);
    q.prepare(QStringLiteral(
        "UPDATE notes SET title=:title,content=:content,color=:color,geometry=:geometry,"
        "z_order=:z_order,updated_at=:updated_at WHERE id=:id"));
    q.bindValue(QStringLiteral(":title"), safeText(rec.title));
    q.bindValue(QStringLiteral(":content"), safeText(rec.content));
    q.bindValue(QStringLiteral(":color"), colorToInt(rec.color));
    q.bindValue(QStringLiteral(":geometry"), encodeGeometry(rec.geometry));
    q.bindValue(QStringLiteral(":z_order"), rec.zOrder);
    q.bindValue(QStringLiteral(":updated_at"), nowStamp());
    q.bindValue(QStringLiteral(":id"), rec.id);
    return q.exec();
}

bool NoteDao::updateGeometry(qint64 id, const QRect &geometry)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }
    QSqlQuery q(conn);
    q.prepare(QStringLiteral("UPDATE notes SET geometry=:geometry,updated_at=:updated_at WHERE id=:id"));
    q.bindValue(QStringLiteral(":geometry"), encodeGeometry(geometry));
    q.bindValue(QStringLiteral(":updated_at"), nowStamp());
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec();
}

bool NoteDao::updateZOrder(qint64 id, int zOrder)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }
    QSqlQuery q(conn);
    q.prepare(QStringLiteral("UPDATE notes SET z_order=:z_order WHERE id=:id"));
    q.bindValue(QStringLiteral(":z_order"), zOrder);
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec();
}

bool NoteDao::softDelete(qint64 id)
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return false;
    }
    QSqlQuery q(conn);
    q.prepare(QStringLiteral("UPDATE notes SET deleted=1,updated_at=:updated_at WHERE id=:id"));
    q.bindValue(QStringLiteral(":updated_at"), nowStamp());
    q.bindValue(QStringLiteral(":id"), id);
    return q.exec();
}

int NoteDao::maxZOrder()
{
    QSqlDatabase conn = Database::instance().connection();
    if (!conn.isOpen()) {
        return 0;
    }
    QSqlQuery q(conn);
    if (!q.exec(QStringLiteral("SELECT COALESCE(MAX(z_order),0) FROM notes")) || !q.next()) {
        return 0;
    }
    return q.value(0).toInt();
}

} // namespace kn
