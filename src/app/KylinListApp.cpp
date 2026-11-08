#include "KylinListApp.h"
#include "../db/Database.h"
#include "../platform/InstanceChannel.h"
#include "../ui/ReminderPopup.h"
#include "../ui/TaskPanel.h"
#include "NoteManager.h"
#include "ReminderScheduler.h"
#include "Settings.h"
#include "TaskStore.h"
#include "TrayController.h"
#ifdef KYLINLIST_HAVE_DBUS
#include "DbusAdapter.h"
#endif
#include <QApplication>
#include <QDebug>
#include <QTimer>
namespace kl {
namespace {
QString reminderPopupKey(const QString &entityId, const QString &reminderId)
{
    return entityId + QLatin1Char('/') + reminderId;
}
}
KylinListApp::KylinListApp(QObject* parent) : QObject(parent) {
}
KylinListApp::~KylinListApp() {
    shutdown();
}
bool KylinListApp::init(QString* error, bool startHidden) {
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
    connect(m_tray, &TrayController::quitRequested, this, &KylinListApp::requestQuit);
    connect(m_panel, &TaskPanel::quitRequested, this, &KylinListApp::requestQuit);
    connect(m_panel, &TaskPanel::importBackupRequested, this, &KylinListApp::importBackup);
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
#ifdef KYLINLIST_HAVE_DBUS
    m_dbus = new DbusAdapter(m_notes, this);
    QString dbusError;
    if (!m_dbus->registerService(&dbusError))
        qWarning() << "D-Bus:" << dbusError;
#endif
    m_reminders = new ReminderScheduler(m_store, this);
    connect(m_reminders, &ReminderScheduler::reminderTriggered, this, &KylinListApp::showReminder);
    connect(m_store, &TaskStore::taskChanged, this, [this](QString id) {
        const Entity task = m_store->entity(id);
        const auto popups = m_popups;
        for (const auto& popup : popups) {
            if (!popup || popup->entityId() != id)
                continue;
            bool close = task.id.isEmpty() || task.kind != EntityKind::Task || task.deleted || task.status != TaskStatus::Active;
            bool found = false;
            if (!close) {
                for (const TaskReminder& reminder : task.reminders) {
                    if (reminder.id != popup->reminderId())
                        continue;
                    found = true;
                    close = !reminder.triggerAt.isValid() || reminder.triggerAt > appNow();
                    break;
                }
            }
            if (close || !found)
                popup->close();
        }
    });
    QTimer::singleShot(500, m_reminders, &ReminderScheduler::start);
    if (!startHidden || !QSystemTrayIcon::isSystemTrayAvailable())
        m_panel->showTask();
    connect(qApp, &QCoreApplication::aboutToQuit, this, &KylinListApp::shutdown);
    return true;
}
void KylinListApp::showReminder(QString entityId, QString reminderId) {
    const Entity task = m_store->entity(entityId);
    if (task.id.isEmpty() || task.kind != EntityKind::Task || task.deleted || task.status != TaskStatus::Active || reminderId.isEmpty())
        return;
    bool found = false;
    for (const TaskReminder& reminder : task.reminders)
        if (reminder.id == reminderId)
            found = true;
    if (!found)
        return;
    const QString key = reminderPopupKey(entityId, reminderId);
    if (m_popups.value(key)) {
        m_popups[key]->raise();
        return;
    }
    m_tray->notifyReminder(entityId, task.title);
    auto* popup = new ReminderPopup(task, reminderId);
    popup->setWindowIcon(TrayController::trayIcon());
    m_popups.insert(key, popup);
    connect(popup, &QObject::destroyed, this, [this, key, popup]() {
        if (m_popups.value(key) == popup) m_popups.remove(key);
    });
    connect(popup, &ReminderPopup::openRequested, m_panel, &TaskPanel::showTask);
    connect(popup, &ReminderPopup::snoozeRequested, this,
            [this, popup](QString, QString reminderId, int minutes) {
                if (m_reminders->snoozeReminder(reminderId, minutes))
                    popup->close();
            });
    connect(popup, &ReminderPopup::completeRequested, this, [this, popup](QString entityId) {
        if (m_panel->flush() && m_store->setCompleted(entityId, true))
            popup->close();
    });
    popup->show();
    popup->raise();
    QApplication::alert(popup, 10000);
}
bool KylinListApp::importBackup(const QString& path) {
    if (m_restoring || m_shutdown || !m_store || !m_panel || !m_notes || !m_reminders) return false;
    if (!m_panel->flush() || !m_notes->flushAll() || !m_store->endAllSessions()) return false;
    m_restoring = true; m_reminders->stop();
    const auto popups = m_popups;
    for (const auto& popup : popups) if (popup) popup->close();
    m_popups.clear();
    m_panel->setRestoreFrozen(true); m_notes->setRestoreFrozen(true);
    if (!m_store->importBackup(path)) {
        const QString failure = m_store->lastError();
        m_panel->setRestoreFrozen(false); m_notes->setRestoreFrozen(false); m_reminders->start();
        m_tray->notify(QStringLiteral("恢复失败，本机数据与附件未改变：") + failure);
        m_restoring = false; return false;
    }
    const int restoredCount = m_store->entities().size();
    m_notes->closeAllForRestore(); m_notes->setRestoreFrozen(false);
    m_panel->resetAfterRestore(); m_reminders->reset(); m_reminders->start();
    m_tray->notify(QStringLiteral("覆盖恢复完成：%1 个任务与笔记，旧数据已被替换。").arg(restoredCount));
    m_restoring = false; return true;
}
void KylinListApp::requestQuit() {
    if (m_restoring) return;
    if ((m_panel && !m_panel->flush()) || (m_notes && !m_notes->flushAll())
        || (m_store && !m_store->endAllSessions())) {
        if (m_panel)
            m_panel->showTask();
        return;
    }
    QApplication::quit();
}
void KylinListApp::shutdown() {
    if (m_shutdown)
        return;
    m_shutdown = true;
    if (m_reminders)
        m_reminders->stop();
    if (m_panel)
        m_panel->flush();
    if (m_notes)
        m_notes->flushAll();
    if (m_store)
        m_store->endAllSessions();
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
}  // namespace kl
