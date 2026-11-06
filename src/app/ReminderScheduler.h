#ifndef KYLINNOTE_REMINDERSCHEDULER_H
#define KYLINNOTE_REMINDERSCHEDULER_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace kn {

class TaskStore;

// 只有数据库去重标记提交成功才发 reminderDue；具体提示方式由 UI 选择。
// 15s 轮询在系统恢复/时钟跳变后补发逾期提醒，应用重新激活时也会立即检查。
class ReminderScheduler : public QObject
{
    Q_OBJECT
public:
    explicit ReminderScheduler(TaskStore *store, QObject *parent = nullptr);
    void start();
    void stop();
    void checkDue(const QDateTime &now = QDateTime::currentDateTime());
    bool snooze(qint64 id, int minutes = 10);

signals:
    void reminderDue(qint64 id);

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

} // namespace kn

#endif // KYLINNOTE_REMINDERSCHEDULER_H
