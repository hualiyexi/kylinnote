#ifndef KYLINNOTE_KYLINNOTEAPP_H
#define KYLINNOTE_KYLINNOTEAPP_H

#include <QObject>
#include <QTimer>

class QWidget;

namespace kn {

class NoteManager;
class TrayController;
class DbusAdapter;
class GlobalHotkey;

// 应用装配：数据库、自启、还原、托盘、D-Bus、全局快捷键
class KylinNoteApp : public QObject
{
    Q_OBJECT
public:
    explicit KylinNoteApp(QObject *parent = nullptr);
    ~KylinNoteApp() override;

    bool init(QString *error = nullptr);

public slots:
    void shutdown();

private slots:
    void onScreenReadyCheck();
    void onQuitRequested();
    void onHotkeyActivated(int id);

private:
    void setupGlobalShortcuts();
    void doRestore();
    void applyLayerChange();

    // 全局热键 id
    enum { HotkeyNewNote = 1, HotkeyDeleteNote = 2, HotkeyCycleLayer = 3 };

    NoteManager    *m_manager = nullptr;
    TrayController *m_tray    = nullptr;
    DbusAdapter    *m_dbus    = nullptr;
    QWidget        *m_hotkeyHost = nullptr;
    GlobalHotkey   *m_hotkeys    = nullptr;

    QTimer m_readyTimer;
    int    m_readyTries = 0;
    bool   m_restored   = false;
    bool   m_shutdown   = false;
    bool   m_dbBroken   = false;
};

} // namespace kn

#endif // KYLINNOTE_KYLINNOTEAPP_H
