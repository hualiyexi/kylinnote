#pragma once
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>
class QMenu;
namespace kn {
class NoteManager;
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(NoteManager* manager, QObject* parent = nullptr);
    ~TrayController() override;
    void show();
    void notify(const QString& text);
    void notifyReminder(qint64 taskId, const QString& title);
    static QIcon trayIcon();
signals:
    void quitRequested();
    void panelRequested();
    void newTaskRequested();
    void taskRequested(qint64 id);

private:
    void tryShow();
    NoteManager* m_manager;
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QTimer m_retryTimer;
    qint64 m_lastReminderId = 0;
};
}  // namespace kn
