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
    // 取消会话只补记本会话自己真正写入的字段终值（recordSessionCommit 记录），
    // 不与当前实体比较：其他窗口的修改由它们自己的会话/直存记录，取消绝不重复记账。
    QVector<EntityEvent> eventsForCancel(const QString &sessionId) const;
    // commitTextDraft 成功后登记该会话实际写入的字段与提交后快照；未知会话或不匹配实体为 no-op。
    void recordSessionCommit(const QString &sessionId, bool wroteTitle, bool wroteBody, const Entity &after);
    void finishEditSession(const QString &sessionId);
    QStringList activeEditSessions() const;

private:
    struct EditSession {
        QString entityId;
        QString title;              // 会话开始基线（事件日志口径）
        QString body;
        // 本会话自身写入归因：只记通过 commitTextDraft 真正写库的字段终值。
        bool wroteTitle = false;
        bool wroteBody = false;
        QString committedTitle;
        QString committedBody;
        EntityKind snapshotKind = EntityKind::Task;   // 最后一次写入后的元数据，供取消事件使用
        QString snapshotTitle;
    };
    QHash<QString, EditSession> m_sessions;
};

} // namespace kl

#endif // KYLINLIST_JOURNAL_H
