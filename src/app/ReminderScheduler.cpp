#include "ReminderScheduler.h"

#include "TaskStore.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QEvent>
#include <QScopedValueRollback>

namespace kn {
namespace {
const int kPollIntervalMs = 15000;
}

ReminderScheduler::ReminderScheduler(TaskStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
    m_timer.setInterval(kPollIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this]() { checkDue(); });
}

void ReminderScheduler::start()
{
    if (m_running) return;
    m_running = true;
    if (QCoreApplication::instance()) QCoreApplication::instance()->installEventFilter(this);
    QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance(thread());
    if (dispatcher) {
        m_awakeConnection = connect(dispatcher, &QAbstractEventDispatcher::awake,
                                    this, &ReminderScheduler::checkAfterWake);
    }
    m_timer.start();
    checkDue();
}

void ReminderScheduler::stop()
{
    m_running = false;
    m_timer.stop();
    if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
    disconnect(m_awakeConnection);
    m_awakeConnection = QMetaObject::Connection();
}

void ReminderScheduler::checkAfterWake()
{
    if (!m_running || m_checking) return;
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 wallElapsed = m_lastCheck.isValid() ? m_lastCheck.msecsTo(now) : kPollIntervalMs;
    // 持续 UI 事件也可能让定时器延迟；单调时钟 + 墙钟联合检查，覆盖前后跳变和休眠恢复。
    if (!m_elapsed.isValid() || m_elapsed.elapsed() >= kPollIntervalMs
        || wallElapsed >= kPollIntervalMs || wallElapsed < 0) checkDue(now);
}

bool ReminderScheduler::eventFilter(QObject *watched, QEvent *event)
{
    if (m_running && (event->type() == QEvent::ApplicationActivate || event->type() == QEvent::ApplicationStateChange))
        checkDue();
    return QObject::eventFilter(watched, event);
}

void ReminderScheduler::checkDue(const QDateTime &now)
{
    if (!m_store || m_checking || !now.isValid()) return;
    QScopedValueRollback<bool> guard(m_checking, true);
    m_lastCheck = now;
    m_elapsed.restart();
    const QVector<Task> tasks = m_store->tasks();
    for (const Task &task : tasks) {
        if (!m_store) break;
        if (task.completed || task.deleted || !task.reminderAt.isValid() || task.reminderAt > now
            || (task.notifiedAt.isValid() && task.notifiedAt >= task.reminderAt)) continue;
        // 先持久 claim，再提示。失败保留未通知状态，下轮可重试。
        if (m_store->markReminderNotified(task.id, task.reminderAt, now)) {
            // Store 的变化信号可能同步触发 UI 修改任务，再确认一次当前资格。
            if (!m_store) break;
            const Task current = m_store->task(task.id);
            if (current.id && !current.completed && !current.deleted && current.reminderAt == task.reminderAt)
                emit reminderDue(task.id);
        }
    }
}

bool ReminderScheduler::snooze(qint64 id, int minutes)
{
    if (!m_store || minutes <= 0) return false;
    Task task = m_store->task(id);
    if (!task.id || task.completed || task.deleted || !task.reminderAt.isValid()) return false;
    const QDateTime next = QDateTime::currentDateTime().addSecs(qint64(minutes) * 60);
    if (!next.isValid() || next.date().year() > 9999) return false;
    task.reminderAt = next;
    task.notifiedAt = QDateTime();
    return m_store->saveTask(task);
}

} // namespace kn
