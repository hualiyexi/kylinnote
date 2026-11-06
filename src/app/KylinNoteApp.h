#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
namespace kn {
class TaskStore;
class ReminderScheduler;
class NoteManager;
class TaskPanel;
class TrayController;
class InstanceChannel;
class ReminderPopup;
class DbusAdapter;
// 装配业务服务与各类展示窗口；平台能力通过独立适配器接入。
class KylinNoteApp : public QObject {
    Q_OBJECT
public:
    explicit KylinNoteApp(QObject* parent = nullptr);
    ~KylinNoteApp() override;
    bool init(QString* error = nullptr, bool startHidden = false);
public slots:
    void shutdown();

private:
    void requestQuit();
    void showReminder(qint64 id);
    TaskStore* m_store = nullptr;
    ReminderScheduler* m_reminders = nullptr;
    NoteManager* m_notes = nullptr;
    TaskPanel* m_panel = nullptr;
    TrayController* m_tray = nullptr;
    InstanceChannel* m_channel = nullptr;
    DbusAdapter* m_dbus = nullptr;
    QMap<qint64, QPointer<ReminderPopup>> m_popups;
    bool m_shutdown = false;
};
}  // namespace kn
