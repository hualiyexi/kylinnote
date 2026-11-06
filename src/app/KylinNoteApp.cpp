#include "KylinNoteApp.h"

#include "../db/Database.h"
#include "../platform/Autostart.h"
#include "../platform/GlobalHotkey.h"
#include "../platform/ScreenFit.h"
#include "DbusAdapter.h"
#include "NoteManager.h"
#include "Settings.h"
#include "TrayController.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QWidget>

namespace kn {

KylinNoteApp::KylinNoteApp(QObject *parent)
    : QObject(parent)
{
}

KylinNoteApp::~KylinNoteApp()
{
    shutdown();
}

bool KylinNoteApp::init(QString *error)
{
    // 1. 数据库：打不开也允许继续运行，全部改动留在内存并给出状态提示
    QString dbError;
    if (!Database::instance().open(&dbError)) {
        m_dbBroken = true;
        qWarning() << "[kylinnote] 打开数据库失败:" << dbError;
    }

    // 2. 开机自启
    QString autostartError;
    if (!Autostart::ensureInstalled(&autostartError)) {
        qWarning() << "[kylinnote] 写入开机自启失败:" << autostartError;
    }

    // 3. 调度层与托盘
    m_manager = new NoteManager(this);
    m_tray    = new TrayController(m_manager, this);
    connect(m_tray, &TrayController::quitRequested,
            this, &KylinNoteApp::onQuitRequested);
    m_tray->show();

    setupGlobalShortcuts();

    // 4. 还原便签，窗管未就绪时延迟重试
    if (ScreenFit::screensReady()) {
        doRestore();
    } else {
        m_readyTimer.setInterval(WindowReadyRetryMs);
        m_readyTimer.setSingleShot(false);
        connect(&m_readyTimer, &QTimer::timeout, this, &KylinNoteApp::onScreenReadyCheck);
        m_readyTimer.start();
    }

    // 5. D-Bus 服务（失败则退化为"仅锁文件"的单实例模式）
    m_dbus = new DbusAdapter(m_manager, this);
    QString dbusError;
    if (!m_dbus->registerService(&dbusError)) {
        qWarning() << "[kylinnote] 注册 D-Bus 服务失败:" << dbusError;
    }

    if (m_dbBroken && m_tray) {
        m_tray->notify(QString::fromUtf8(
            "\u65e0\u6cd5\u6253\u5f00\u7b14\u7b3a\u6570\u636e\u5e93\uff0c\u4fee\u6539\u4ec5\u4fdd\u5b58\u5728\u5185\u5b58\u4e2d"));
    }

    connect(qApp, &QCoreApplication::aboutToQuit, this, &KylinNoteApp::shutdown);

    if (error) {
        error->clear();
    }
    return true;
}

void KylinNoteApp::onScreenReadyCheck()
{
    if (m_restored) {
        m_readyTimer.stop();
        return;
    }
    if (ScreenFit::screensReady()) {
        doRestore();
        return;
    }
    if (++m_readyTries >= WindowReadyMaxRetry) {
        // 窗管迟迟不就绪，按当前（可能为空）的屏幕信息兜底还原
        qWarning() << "[kylinnote] 窗管未就绪，按默认几何还原便签";
        doRestore();
    }
}

void KylinNoteApp::doRestore()
{
    if (m_restored) {
        return;
    }
    m_restored = true;
    m_readyTimer.stop();

    m_manager->restoreAll();

    Settings &settings = Settings::instance();
    if (m_manager->isEmpty() && !settings.hintShown()) {
        settings.setHintShown(true);
        settings.sync();
        if (m_tray) {
            m_tray->notify(QString::fromUtf8(
                "\u8bf7\u5728\u6258\u76d8\u56fe\u6807\u4e0a\u53f3\u952e\u9009\u62e9\u201c\u65b0\u5efa\u4fbf\u7b3a\u201d\uff0c\u6216\u6309 Ctrl+N"));
        }
    }
}

void KylinNoteApp::setupGlobalShortcuts()
{
#ifdef KYLINNOTE_HAVE_X11
    // X11 下用 XGrabKey 做真正的全局热键：焦点在别的软件里也能响应。
    // grab 失败（被占用）时退回下面的 QAction 方式。
    m_hotkeys = new GlobalHotkey(this);
    if (m_hotkeys->isAvailable()) {
        struct { QKeySequence seq; int id; } wanted[] = {
            { QKeySequence(QStringLiteral("Ctrl+N")), HotkeyNewNote },
            { QKeySequence(QStringLiteral("Ctrl+D")), HotkeyDeleteNote },
            { QKeySequence(Qt::Key_F12),              HotkeyCycleLayer }
        };
        bool allGrabbed = true;
        for (const auto &w : wanted) {
            if (!m_hotkeys->grab(w.seq, w.id)) {
                allGrabbed = false;
            }
        }
        if (allGrabbed) {
            connect(m_hotkeys, &GlobalHotkey::activated,
                    this, &KylinNoteApp::onHotkeyActivated);
            return;
        }
        delete m_hotkeys;
        m_hotkeys = nullptr;
    }
#endif

    // 降级路径：QAction + ApplicationShortcut，只在便签窗口处于激活态时生效
    m_hotkeyHost = new QWidget();
    m_hotkeyHost->setWindowTitle(QStringLiteral("kylinnote-hotkey-host"));

    QAction *newNote = new QAction(m_hotkeyHost);
    newNote->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    newNote->setShortcutContext(Qt::ApplicationShortcut);
    m_hotkeyHost->addAction(newNote);
    connect(newNote, &QAction::triggered, this, [this]() {
        m_manager->createNote();
    });

    QAction *deleteNote = new QAction(m_hotkeyHost);
    deleteNote->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    deleteNote->setShortcutContext(Qt::ApplicationShortcut);
    m_hotkeyHost->addAction(deleteNote);
    connect(deleteNote, &QAction::triggered, this, [this]() {
        m_manager->deleteActive();
    });

    QAction *toggleTop = new QAction(m_hotkeyHost);
    toggleTop->setShortcut(QKeySequence(Qt::Key_F12));
    toggleTop->setShortcutContext(Qt::ApplicationShortcut);
    m_hotkeyHost->addAction(toggleTop);
    connect(toggleTop, &QAction::triggered, this, [this]() {
        applyLayerChange();
    });
}

void KylinNoteApp::onHotkeyActivated(int id)
{
    switch (id) {
    case HotkeyNewNote:
        m_manager->createNote();
        break;
    case HotkeyDeleteNote:
        m_manager->deleteActive();
        break;
    case HotkeyCycleLayer:
        applyLayerChange();
        break;
    default:
        break;
    }
}

void KylinNoteApp::applyLayerChange()
{
    const NoteLayer layer = m_manager->cycleLayerForActive();
    m_tray->notify(layerLabel(layer));
}

void KylinNoteApp::onQuitRequested()
{
    shutdown();
    QApplication::quit();
}

void KylinNoteApp::shutdown()
{
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    if (m_manager) {
        m_manager->flushAll();
    }
    Settings::instance().sync();
    Database::instance().close();
}

} // namespace kn
