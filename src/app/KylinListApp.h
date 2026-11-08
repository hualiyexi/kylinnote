#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
namespace kl {
class TaskStore;
class ReminderScheduler;
class NoteManager;
class TaskPanel;
class TrayController;
class InstanceChannel;
class ReminderPopup;
class DbusAdapter;
// 装配业务服务与各类展示窗口；平台能力通过独立适配器接入。
class KylinListApp : public QObject {
    Q_OBJECT
public:
    explicit KylinListApp(QObject* parent = nullptr);
    ~KylinListApp() override;
    bool init(QString* error = nullptr, bool startHidden = false);
    bool importBackup(const QString& path);
public slots:
    void shutdown();

private:
    void requestQuit();
    void showReminder(QString entityId, QString reminderId);
    TaskStore* m_store = nullptr;
    ReminderScheduler* m_reminders = nullptr;
    NoteManager* m_notes = nullptr;
    TaskPanel* m_panel = nullptr;
    TrayController* m_tray = nullptr;
    InstanceChannel* m_channel = nullptr;
    DbusAdapter* m_dbus = nullptr;
    QMap<QString, QPointer<ReminderPopup>> m_popups;
    bool m_shutdown = false;
    bool m_restoring = false;
};
}  // namespace kl
