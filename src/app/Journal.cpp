#include "Journal.h"

namespace kl {
namespace {
void appendTextEvent(QVector<EntityEvent> *events, const Entity &current, const QString &field,
                     const QString &newValue, const QDateTime &occurredAt)
{
    EntityEvent event; event.entityId = current.id; event.entityKind = current.kind;
    event.entityTitle = current.title; event.type = QStringLiteral("changed"); event.field = field;
    event.newValue = newValue.isNull() ? QStringLiteral("") : newValue;
    event.occurredAt = occurredAt; events->append(event);
}
}

QString Journal::beginEditSession(const Entity &entity)
{
    if (!isUuid(entity.id)) return QString();
    EditSession session;
    session.entityId = entity.id; session.title = entity.title; session.body = entity.body;
    const QString id = newUuid(); m_sessions.insert(id, session); return id;
}

QString Journal::entityId(const QString &sessionId) const
{
    return m_sessions.value(sessionId).entityId;
}

bool Journal::hasActiveSessionForEntity(const QString &entityId) const
{
    for (const EditSession &session : m_sessions) if (session.entityId == entityId) return true;
    return false;
}

QVector<EntityEvent> Journal::eventsForDirectSave(const Entity &previous, const Entity &current,
                                                const QDateTime &occurredAt) const
{
    QVector<EntityEvent> events;
    if (previous.id != current.id) return events;
    if (current.title != previous.title) appendTextEvent(&events, current, QStringLiteral("title"), current.title, occurredAt);
    if (current.body != previous.body) appendTextEvent(&events, current, QStringLiteral("body"), current.body, occurredAt);
    return events;
}

QVector<EntityEvent> Journal::eventsForEnd(const QString &sessionId, const Entity &current) const
{
    QVector<EntityEvent> events;
    const auto found = m_sessions.constFind(sessionId);
    if (found == m_sessions.constEnd()) return events;
    // 结束会话只补记本会话自己真正写入的字段终值（recordSessionCommit 记录），不与当前实体
    // 比较：其他编辑器的修改由它们自己的会话/直存记账，结束绝不重复记账或把别人的写入算给自己。
    // 目标实体可能已被永久删除：元数据优先用传入的当前实体（id 匹配时），否则退回最后一次
    // 写入后的快照，entity_events 无外键仍可记账。
    Entity snapshot;
    snapshot.id = found->entityId;
    snapshot.kind = current.id == found->entityId ? current.kind : found->snapshotKind;
    snapshot.title = current.id == found->entityId ? current.title : found->snapshotTitle;
    const QDateTime occurredAt = utcNow();
    if (found->wroteTitle && found->committedTitle != found->title)
        appendTextEvent(&events, snapshot, QStringLiteral("title"), found->committedTitle, occurredAt);
    if (found->wroteBody && found->committedBody != found->body)
        appendTextEvent(&events, snapshot, QStringLiteral("body"), found->committedBody, occurredAt);
    return events;
}

QVector<EntityEvent> Journal::eventsForCancel(const QString &sessionId) const
{
    QVector<EntityEvent> events;
    const auto found = m_sessions.constFind(sessionId);
    if (found == m_sessions.constEnd()) return events;
    // 目标实体可能已被永久删除；事件元数据使用最后一次写入时的快照，entity_events 无外键仍可记账。
    Entity snapshot;
    snapshot.id = found->entityId; snapshot.kind = found->snapshotKind; snapshot.title = found->snapshotTitle;
    const QDateTime occurredAt = utcNow();
    if (found->wroteTitle && found->committedTitle != found->title)
        appendTextEvent(&events, snapshot, QStringLiteral("title"), found->committedTitle, occurredAt);
    if (found->wroteBody && found->committedBody != found->body)
        appendTextEvent(&events, snapshot, QStringLiteral("body"), found->committedBody, occurredAt);
    return events;
}

void Journal::recordSessionCommit(const QString &sessionId, bool wroteTitle, bool wroteBody, const Entity &after)
{
    const auto found = m_sessions.find(sessionId);
    if (found == m_sessions.end() || found->entityId != after.id) return;
    // 只更新本会话真正写入的字段；合并保留下来的远端值属于其他编辑器，不能算作本会话历史。
    if (wroteTitle) { found->wroteTitle = true; found->committedTitle = after.title; }
    if (wroteBody) { found->wroteBody = true; found->committedBody = after.body; }
    found->snapshotKind = after.kind; found->snapshotTitle = after.title;
}

void Journal::finishEditSession(const QString &sessionId)
{
    m_sessions.remove(sessionId);
}

QStringList Journal::activeEditSessions() const
{
    return m_sessions.keys();
}

} // namespace kl
