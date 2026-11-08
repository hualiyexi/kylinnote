#ifndef KYLINLIST_REMINDERSCHEDULER_H
#define KYLINLIST_REMINDERSCHEDULER_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace kl {

class TaskStore;

// 只有 reminder_deliveries 持久 claim 成功才发信号；投递按 installation_id 去重。
class ReminderScheduler : public QObject
{
    Q_OBJECT
public:
    explicit ReminderScheduler(TaskStore *store, QObject *parent = nullptr);
    void start();
    void stop();
    void reset();
    void checkDue(const QDateTime &now = QDateTime::currentDateTimeUtc());
    bool snooze(const QString &entityId, int minutes = 10); // 旧 UI：snooze 第一条提醒
    bool snoozeReminder(const QString &reminderId, int minutes = 10);

signals:
    void reminderDue(QString entityId);
    void reminderTriggered(QString entityId, QString reminderId);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void checkAfterWake();
    QPointer<TaskStore> m_store;
    QTimer m_timer;
    QElapsedTimer m_elapsed;
    QDateTime m_lastCheck;
    bool m_checking = false;
    bool m_running = false;
    QMetaObject::Connection m_awakeConnection;
};

} // namespace kl

#endif // KYLINLIST_REMINDERSCHEDULER_H
