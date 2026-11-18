#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
class QAction;
namespace kl {
class PopupMenu;
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(QObject* parent = nullptr);
    ~TrayController() override;
    void show();
    void notify(const QString& text);
    // 信息级通知：不改变提醒点击目标，点击后打开主面板（用于更新提示等非告警消息）。
    void notifyInfo(const QString& text);
    void notifyReminder(QString entityId, const QString& title);
    static QIcon trayIcon();

    void setTopmostChecked(bool checked);
    void setTaskWidgetLocked(bool locked);
    void setLockShortcutAvailable(bool available);

signals:
    void quitRequested();
    void panelRequested();
    void taskRequested(QString id);
    void addTaskWidgetRequested();
    void topmostRequested(bool enabled);
    void taskWidgetLockRequested(bool locked);

private:
    void tryShow();
    void updateMenuWidth();
    QSystemTrayIcon* m_tray = nullptr;
    PopupMenu* m_menu = nullptr;
    QAction* m_topmostAction = nullptr;
    QAction* m_lockAction = nullptr;
    bool m_lockShortcutAvailable = false;
    QTimer m_retryTimer;
    QString m_lastReminderId = QString();
};
}  // namespace kl
