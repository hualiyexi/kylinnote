#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
class QMenu;
namespace kl {
class NoteManager;
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(NoteManager* manager, QObject* parent = nullptr);
    ~TrayController() override;
    void show();
    void notify(const QString& text);
    void notifyReminder(QString entityId, const QString& title);
    static QIcon trayIcon();
signals:
    void quitRequested();
    void panelRequested();
    void newTaskRequested();
    void taskRequested(QString id);

private:
    void tryShow();
    NoteManager* m_manager;
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QTimer m_retryTimer;
    QString m_lastReminderId = QString();
};
}  // namespace kl
