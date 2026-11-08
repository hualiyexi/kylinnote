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
    for (const ChecklistItem &item : entity.checklist) session.checklistTexts.insert(item.id, item.text);
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
    QHash<QString, QString> oldTexts;
    for (const ChecklistItem &item : previous.checklist) oldTexts.insert(item.id, item.text);
    for (const ChecklistItem &item : current.checklist) {
        const auto old = oldTexts.constFind(item.id);
        if (old != oldTexts.constEnd() && old.value() != item.text)
            appendTextEvent(&events, current, QStringLiteral("checklist.%1.text").arg(item.id), item.text, occurredAt);
    }
    return events;
}

QVector<EntityEvent> Journal::eventsForEnd(const QString &sessionId, const Entity &current) const
{
    QVector<EntityEvent> events;
    const auto found = m_sessions.constFind(sessionId);
    if (found == m_sessions.constEnd() || current.id != found->entityId) return events;
    const QDateTime occurredAt = utcNow();
    const auto add = [&](const QString &field, const QString &newValue) {
        EntityEvent event; event.entityId = current.id; event.entityKind = current.kind;
        event.entityTitle = current.title; event.type = QStringLiteral("changed"); event.field = field;
        event.newValue = newValue.isNull() ? QStringLiteral("") : newValue;
        event.occurredAt = occurredAt; events.append(event);
    };
    if (current.title != found->title) add(QStringLiteral("title"), current.title);
    if (current.body != found->body) add(QStringLiteral("body"), current.body);
    for (const ChecklistItem &item : current.checklist) {
        const auto baseline = found->checklistTexts.constFind(item.id);
        if (baseline != found->checklistTexts.constEnd() && baseline.value() != item.text)
            add(QStringLiteral("checklist.%1.text").arg(item.id), item.text);
    }
    return events;
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
