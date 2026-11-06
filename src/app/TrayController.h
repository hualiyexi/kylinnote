#ifndef KYLINNOTE_TRAYCONTROLLER_H
#define KYLINNOTE_TRAYCONTROLLER_H

#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

class QActionGroup;
class QMenu;

namespace kn {

class NoteManager;

// 系统托盘：右键菜单 新建 / 显示全部 / 隐藏全部 / 退出
class TrayController : public QObject
{
    Q_OBJECT
public:
    TrayController(NoteManager *manager, QObject *parent = nullptr);

    void show();
    void notify(const QString &text);

signals:
    void quitRequested();

public:
    ~TrayController() override;

private:
    void buildMenu();
    void tryShow();
    static QIcon trayIcon();

    NoteManager *m_manager = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QActionGroup *m_layerGroup = nullptr;
    QTimer m_retryTimer;
    int    m_retries = 0;
};

} // namespace kn

#endif // KYLINNOTE_TRAYCONTROLLER_H
