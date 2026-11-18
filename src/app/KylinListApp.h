#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
class QSessionManager;
namespace kl {
class AutostartSettings;
class TaskStore;
class ReminderScheduler;
class NoteManager;
class TaskPanel;
class TaskWidget;
class TrayController;
class GlobalHotkey;
class InstanceChannel;
class ReminderPopup;
class DbusAdapter;
class UpdateDialog;
class StatsDialog;
class StatsService;
namespace updates {
class UpdateService;
}
// 装配业务服务与各类展示窗口；平台能力通过独立适配器接入。
class KylinListApp : public QObject {
    Q_OBJECT
public:
    explicit KylinListApp(QObject* parent = nullptr);
    ~KylinListApp() override;
    // 隔离运行使用独立数据与配置。显式预览模式才提供独立的手动登录启动项。
    bool init(QString* error = nullptr, bool startHidden = false, bool isolatedRun = false,
              bool previewAutostart = false);
    bool importBackup(const QString& path);
    // 检查全部界面内容已保存、编辑会话已结束；不退出应用。
    // 普通退出与更新安装共用此准入条件。
    bool prepareForExit();
    TaskStore* store() const;
    TaskPanel* panel() const { return m_panel; }
    TaskWidget* taskWidget() const { return m_taskWidget; }
    TrayController* trayController() const { return m_tray; }
    // 关闭顶层窗口的系统会话通知：只保存，不模拟用户点击便签的关闭。
    bool commitSession(QSessionManager& sessionManager);
    bool handleSessionEnd();
public slots:
    void shutdown();

private slots:
    // UpdateDialog::postponeRequested()：暂缓自动提醒 7 天并关闭对话框。
    void handleUpdatePostpone();

private:
    void requestQuit();
    void showReminder(QString entityId, QString reminderId);
    void setupUpdates();
    void setupStats();
    UpdateDialog* ensureUpdateDialog();
    void openStats();
    void openUpdateCheck();
    void openUpdateSettings();
    void handleCheckFinished(bool updateAvailable, const QString& message, bool automatic);
    void requestUpdateInstall(const QString& filePath);
    void evaluatePendingInstall();
    void pruneUpdateBackups();
    void initializeDefaultAutostart();
    AutostartSettings* m_autostart = nullptr;
    TaskStore* m_store = nullptr;
    ReminderScheduler* m_reminders = nullptr;
    NoteManager* m_notes = nullptr;
    TaskPanel* m_panel = nullptr;
    TaskWidget* m_taskWidget = nullptr;
    TrayController* m_tray = nullptr;
    GlobalHotkey* m_hotkey = nullptr;
    InstanceChannel* m_channel = nullptr;
    DbusAdapter* m_dbus = nullptr;
    updates::UpdateService* m_updateService = nullptr;
    StatsService* m_statsService = nullptr;
    QPointer<UpdateDialog> m_updateDialog;
    QPointer<StatsDialog> m_statsDialog;
    QMap<QString, QPointer<ReminderPopup>> m_popups;
    QSessionManager* m_sessionManager = nullptr;
    bool m_shutdown = false;
    bool m_sessionEnding = false;
    bool m_restoring = false;
    bool m_updateBusy = false;
    bool m_isolatedRun = false;
};
}  // namespace kl
