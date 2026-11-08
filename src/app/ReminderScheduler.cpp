#include "ReminderScheduler.h"

#include "TaskStore.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QEvent>
#include <QScopedValueRollback>

namespace kl {
namespace { const int kPollIntervalMs = 15000; }

ReminderScheduler::ReminderScheduler(TaskStore *store, QObject *parent) : QObject(parent), m_store(store)
{
    m_timer.setInterval(kPollIntervalMs); m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this]() { checkDue(); });
}
void ReminderScheduler::start()
{
    if (m_running) return; m_running = true;
    if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance(thread());
    if (dispatcher) m_awakeConnection = connect(dispatcher, &QAbstractEventDispatcher::awake, this, &ReminderScheduler::checkAfterWake);
    m_timer.start(); checkDue();
}
void ReminderScheduler::stop()
{
    m_running = false; m_timer.stop();
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
    disconnect(m_awakeConnection); m_awakeConnection = QMetaObject::Connection();
}
void ReminderScheduler::reset()
{
    stop(); m_lastCheck = QDateTime(); m_elapsed.invalidate(); m_checking = false;
}
void ReminderScheduler::checkAfterWake()
{
    if (!m_running || m_checking) return;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 wallElapsed = m_lastCheck.isValid() ? m_lastCheck.msecsTo(now) : kPollIntervalMs;
    if (!m_elapsed.isValid() || m_elapsed.elapsed() >= kPollIntervalMs || wallElapsed >= kPollIntervalMs || wallElapsed < 0) checkDue(now);
}
bool ReminderScheduler::eventFilter(QObject *watched, QEvent *event)
{
    if (m_running && (event->type() == QEvent::ApplicationActivate || event->type() == QEvent::ApplicationStateChange)) checkDue();
    return QObject::eventFilter(watched, event);
}
void ReminderScheduler::checkDue(const QDateTime &now)
{
    if (!m_store || m_checking || !now.isValid()) return;
    QScopedValueRollback<bool> guard(m_checking, true);
    const QDateTime utc = now.toUTC(); m_lastCheck = utc; m_elapsed.restart();
    const QVector<Entity> tasks = m_store->entities();
    for (const Entity &task : tasks) {
        if (!m_store) break;
        if (task.kind != EntityKind::Task || task.status != TaskStatus::Active || task.deleted) continue;
        for (const TaskReminder &reminder : task.reminders) {
            if (!m_store) break;
            if (reminder.triggerAt > utc || m_store->isReminderDelivered(reminder.id)) continue;
            if (!m_store->markReminderDelivered(reminder.id, reminder.triggerAt, utc)) continue;
            if (!m_store) break;
            const Entity current = m_store->entity(task.id);
            bool stillCurrent = false;
            for (const TaskReminder &row : current.reminders)
                if (row.id == reminder.id && row.triggerAt == reminder.triggerAt) stillCurrent = true;
            if (current.id.isEmpty() || current.kind != EntityKind::Task || current.status != TaskStatus::Active || current.deleted || !stillCurrent) continue;
            emit reminderDue(task.id); emit reminderTriggered(task.id, reminder.id);
        }
    }
}
bool ReminderScheduler::snoozeReminder(const QString &reminderId, int minutes)
{
    if (!m_store || minutes <= 0) return false;
    Entity value;
    for (const Entity &candidate : m_store->entities()) for (const TaskReminder &row : candidate.reminders)
        if (row.id == reminderId) value = candidate;
    if (value.id.isEmpty() || value.kind != EntityKind::Task || value.status != TaskStatus::Active || value.deleted) return false;
    const QDateTime next = QDateTime::currentDateTimeUtc().addSecs(qint64(minutes) * 60);
    if (!next.isValid() || next.date().year() > 9999) return false;
    bool found = false;
    for (TaskReminder &row : value.reminders) if (row.id == reminderId) { row.triggerAt = next; found = true; }
    return found && m_store->saveTask(value);
}
bool ReminderScheduler::snooze(const QString &entityId, int minutes)
{
    if (!m_store) return false; const Entity value = m_store->entity(entityId);
    if (value.id.isEmpty() || value.kind != EntityKind::Task || value.status != TaskStatus::Active || value.deleted) return false;
    const int primary = primaryReminderIndex(value);
    return primary >= 0 && snoozeReminder(value.reminders.at(primary).id, minutes);
}

} // namespace kl
