#include "KylinNoteApp.h"
#include "../db/Database.h"
#include "../platform/InstanceChannel.h"
#include "../ui/ReminderPopup.h"
#include "../ui/TaskPanel.h"
#include "NoteManager.h"
#include "ReminderScheduler.h"
#include "Settings.h"
#include "TaskStore.h"
#include "TrayController.h"
#ifdef KYLINNOTE_HAVE_DBUS
#include "DbusAdapter.h"
#endif
#include <QApplication>
#include <QDebug>
#include <QTimer>
namespace kn {
KylinNoteApp::KylinNoteApp(QObject* parent) : QObject(parent) {
}
KylinNoteApp::~KylinNoteApp() {
    shutdown();
}
bool KylinNoteApp::init(QString* error, bool startHidden) {
    if (!Database::instance().open(error))
        return false;
    m_store = new TaskStore(Database::instance().connection(), this);
    if (!m_store->load(error))
        return false;
    m_notes = new NoteManager(m_store, this);
    m_panel = new TaskPanel(m_store);
    m_panel->setWindowIcon(TrayController::trayIcon());
    m_tray = new TrayController(m_notes, this);
    connect(m_tray, &TrayController::panelRequested, this, [this]() { m_panel->showTask(); });
    connect(m_tray, &TrayController::newTaskRequested, m_panel, &TaskPanel::focusQuickAdd);
    connect(m_tray, &TrayController::taskRequested, m_panel, &TaskPanel::showTask);
    connect(m_tray, &TrayController::quitRequested, this, &KylinNoteApp::requestQuit);
    connect(m_panel, &TaskPanel::quitRequested, this, &KylinNoteApp::requestQuit);
    connect(m_notes, &NoteManager::taskOpenRequested, m_panel, &TaskPanel::showTask);
    connect(m_store, &TaskStore::errorOccurred, m_tray, &TrayController::notify);
    m_tray->show();
    m_notes->restoreAll();
    m_channel = new InstanceChannel(this);
    QString ipcError;
    if (!m_channel->listen(&ipcError))
        qWarning() << "Instance channel:" << ipcError;
    connect(m_channel, &InstanceChannel::showAllRequested, this, [this]() { m_panel->showTask(); });
    connect(m_channel, &InstanceChannel::newNoteRequested, this, [this]() { m_notes->createNote(); });
#ifdef KYLINNOTE_HAVE_DBUS
    m_dbus = new DbusAdapter(m_notes, this);
    QString dbusError;
    if (!m_dbus->registerService(&dbusError))
        qWarning() << "D-Bus:" << dbusError;
#endif
    m_reminders = new ReminderScheduler(m_store, this);
    connect(m_reminders, &ReminderScheduler::reminderDue, this, &KylinNoteApp::showReminder);
    connect(m_store, &TaskStore::taskChanged, this, [this](qint64 id) {
        const Task task = m_store->task(id);
        if (m_popups.value(id) && (!task.id || task.deleted || task.completed || !task.reminderAt.isValid() ||
                                   task.reminderAt > QDateTime::currentDateTime()))
            m_popups.value(id)->close();
    });
    QTimer::singleShot(500, m_reminders, &ReminderScheduler::start);
    if (!startHidden || !QSystemTrayIcon::isSystemTrayAvailable())
        m_panel->showTask();
    connect(qApp, &QCoreApplication::aboutToQuit, this, &KylinNoteApp::shutdown);
    return true;
}
void KylinNoteApp::showReminder(qint64 id) {
    const Task task = m_store->task(id);
    if (!task.id || task.deleted || task.completed)
        return;
    if (m_popups.value(id)) {
        m_popups[id]->raise();
        return;
    }
    m_tray->notifyReminder(id, task.title);
    auto* popup = new ReminderPopup(task);
    popup->setWindowIcon(TrayController::trayIcon());
    m_popups.insert(id, popup);
    connect(popup, &QObject::destroyed, this, [this, id]() { m_popups.remove(id); });
    connect(popup, &ReminderPopup::openRequested, m_panel, &TaskPanel::showTask);
    connect(popup, &ReminderPopup::snoozeRequested, this, [this, popup](qint64 taskId, int minutes) {
        if (m_reminders->snooze(taskId, minutes))
            popup->close();
    });
    connect(popup, &ReminderPopup::completeRequested, this, [this, popup](qint64 taskId) {
        if (m_panel->flush() && m_store->setCompleted(taskId, true))
            popup->close();
    });
    popup->show();
    popup->raise();
    QApplication::alert(popup, 10000);
}
void KylinNoteApp::requestQuit() {
    if ((m_panel && !m_panel->flush()) || (m_notes && !m_notes->flushAll())) {
        if (m_panel)
            m_panel->showTask();
        return;
    }
    QApplication::quit();
}
void KylinNoteApp::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    if (m_reminders)
        m_reminders->stop();
    if (m_panel)
        m_panel->flush();
    if (m_notes)
        m_notes->flushAll();
    if (m_channel)
        m_channel->close();
    const auto popups = m_popups;
    for (auto popup : popups)
        if (popup)
            delete popup.data();
    m_popups.clear();
    delete m_panel;
    m_panel = nullptr;
    delete m_tray;
    m_tray = nullptr;
    delete m_notes;
    m_notes = nullptr;
    delete m_reminders;
    m_reminders = nullptr;
    delete m_store;
    m_store = nullptr;
    Settings::instance().sync();
    Database::instance().close();
}
}  // namespace kn
