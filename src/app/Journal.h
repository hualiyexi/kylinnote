#ifndef KYLINLIST_JOURNAL_H
#define KYLINLIST_JOURNAL_H

#include "../domain/Entity.h"

#include <QHash>
#include <QStringList>

namespace kl {

// 编辑会话只在内存保存基线；TaskStore 负责把结束时的差异提交到事件仓储。
class Journal
{
public:
    QString beginEditSession(const Entity &entity);
    QString entityId(const QString &sessionId) const;
    bool hasActiveSessionForEntity(const QString &entityId) const;
    QVector<EntityEvent> eventsForDirectSave(const Entity &previous, const Entity &current, const QDateTime &occurredAt) const;
    QVector<EntityEvent> eventsForEnd(const QString &sessionId, const Entity &current) const;
    void finishEditSession(const QString &sessionId);
    QStringList activeEditSessions() const;

private:
    struct EditSession {
        QString entityId;
        QString title;
        QString body;
        QHash<QString, QString> checklistTexts;
    };
    QHash<QString, EditSession> m_sessions;
};

} // namespace kl

#endif // KYLINLIST_JOURNAL_H
