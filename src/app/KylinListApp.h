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
class UpdateDialog;
namespace updates {
class UpdateService;
}
// 装配业务服务与各类展示窗口；平台能力通过独立适配器接入。
class KylinListApp : public QObject {
    Q_OBJECT
public:
    explicit KylinListApp(QObject* parent = nullptr);
    ~KylinListApp() override;
    bool init(QString* error = nullptr, bool startHidden = false);
    bool importBackup(const QString& path);
    // 检查全部界面内容已保存、编辑会话已结束；不退出应用。
    // 普通退出与更新安装共用此准入条件。
    bool prepareForExit();
public slots:
    void shutdown();

private:
    void requestQuit();
    void showReminder(QString entityId, QString reminderId);
    void setupUpdates();
    UpdateDialog* ensureUpdateDialog();
    void openUpdateCheck();
    void openUpdateSettings();
    void handleCheckFinished(bool updateAvailable, const QString& message);
    void requestUpdateInstall(const QString& filePath);
    void evaluatePendingInstall();
    void pruneUpdateBackups();
    TaskStore* m_store = nullptr;
    ReminderScheduler* m_reminders = nullptr;
    NoteManager* m_notes = nullptr;
    TaskPanel* m_panel = nullptr;
    TrayController* m_tray = nullptr;
    InstanceChannel* m_channel = nullptr;
    DbusAdapter* m_dbus = nullptr;
    updates::UpdateService* m_updateService = nullptr;
    QPointer<UpdateDialog> m_updateDialog;
    QMap<QString, QPointer<ReminderPopup>> m_popups;
    bool m_shutdown = false;
    bool m_restoring = false;
    bool m_updateBusy = false;
};
}  // namespace kl
