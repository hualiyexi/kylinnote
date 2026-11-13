#include "KylinListApp.h"
#include "../db/Database.h"
#include "../platform/InstanceChannel.h"
#include "../ui/ReminderPopup.h"
#include "../ui/TaskPanel.h"
#include "../ui/UpdateDialog.h"
#include "NoteManager.h"
#include "ReminderScheduler.h"
#include "Settings.h"
#include "TaskStore.h"
#include "TrayController.h"
#include "updates/InstallIdentity.h"
#include "updates/UpdateService.h"
#include "../platform/UpdateInstaller.h"
#ifdef KYLINLIST_HAVE_DBUS
#include "DbusAdapter.h"
#endif
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    setupUpdates();
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
bool KylinListApp::prepareForExit() {
    if (m_restoring || m_shutdown) return false;
    if ((m_panel && !m_panel->flush()) || (m_notes && !m_notes->flushAll())
        || (m_store && !m_store->endAllSessions())) {
        if (m_panel)
            m_panel->showTask();
        return false;
    }
    return true;
}
void KylinListApp::requestQuit() {
    if (prepareForExit())
        QApplication::quit();
}
void KylinListApp::setupUpdates() {
    QString identityError;
    const updates::InstallIdentity identity = updates::readInstallIdentity(&identityError);
    if (!identity.isValid())
        qWarning() << "Update identity:" << identityError;
    m_updateService = new updates::UpdateService(identity, this);
    m_updateService->setBaseUrl(Settings::instance().updateBaseUrl());
    connect(m_updateService, &updates::UpdateService::checkFinished,
            this, &KylinListApp::handleCheckFinished);
    connect(m_panel, &TaskPanel::updateCheckRequested, this, &KylinListApp::openUpdateCheck);
    connect(m_panel, &TaskPanel::updateSettingsRequested, this, &KylinListApp::openUpdateSettings);
    if (m_updateService->enabled() && m_updateService->identityValid()
        && Settings::instance().updateAutoCheckEnabled()) {
        m_updateService->startAutomaticChecks();
    }
    QTimer::singleShot(300, this, &KylinListApp::evaluatePendingInstall);
}
UpdateDialog* KylinListApp::ensureUpdateDialog() {
    if (m_updateDialog) return m_updateDialog.data();
    m_updateDialog = new UpdateDialog(m_updateService, m_panel);
    connect(m_updateDialog, &UpdateDialog::installRequested,
            this, &KylinListApp::requestUpdateInstall);
    connect(m_updateDialog, &UpdateDialog::settingsRequested,
            this, &KylinListApp::openUpdateSettings);
    return m_updateDialog.data();
}
void KylinListApp::openUpdateCheck() {
    if (!m_updateService) return;
    UpdateDialog* dialog = ensureUpdateDialog();
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    m_updateService->checkNow();
}
void KylinListApp::openUpdateSettings() {
    if (!m_updateService) return;
    Settings& settings = Settings::instance();
    UpdateSettingsDialog dialog(settings.updateBaseUrl(), settings.updateAutoCheckEnabled(), m_panel);
    if (dialog.exec() != QDialog::Accepted) return;
    settings.setUpdateBaseUrl(dialog.baseUrl());
    settings.setUpdateAutoCheckEnabled(dialog.autoCheckEnabled());
    settings.sync();
    m_updateService->setBaseUrl(dialog.baseUrl());
    if (m_updateService->enabled() && m_updateService->identityValid()
        && dialog.autoCheckEnabled()) {
        m_updateService->startAutomaticChecks();
    } else {
        m_updateService->stopAutomaticChecks();
    }
    if (m_updateDialog) m_updateDialog->refresh();
    if (m_panel)
        m_panel->showUpdateNotice(m_updateService->enabled()
                                      ? QStringLiteral("更新服务器设置已保存。")
                                      : QStringLiteral("已停用更新检查。"));
}
void KylinListApp::handleCheckFinished(bool updateAvailable, const QString& message) {
    Q_UNUSED(message);
    Settings& settings = Settings::instance();
    settings.setLastUpdateCheckAt(appNow());
    if (!updateAvailable) return;
    // 自动检查发现新版：每个版本每天至多提醒一次；手动检查由更新对话框展示。
    const QString version = m_updateService->manifest().version;
    const QDate today = toAppTime(appNow()).date();
    if (settings.lastUpdateNoticeVersion() == version && settings.lastUpdateNoticeDate() == today)
        return;
    settings.setLastUpdateNoticeVersion(version);
    settings.setLastUpdateNoticeDate(today);
    const QString text = QStringLiteral("发现新版本 %1，可在「备份与设置 ▸ 检查更新」安装。").arg(version);
    if (m_panel && m_panel->isVisible())
        m_panel->showUpdateNotice(text);
    else if (m_tray)
        m_tray->notifyInfo(text);
}
void KylinListApp::requestUpdateInstall(const QString& filePath) {
    if (m_updateBusy || m_shutdown || m_restoring || !m_updateService
        || m_updateService->state() != updates::UpdateService::State::Ready)
        return;
    const updates::UpdateArtifact* artifact = m_updateService->targetArtifact();
    if (!artifact || m_updateService->verifiedFilePath() != filePath) return;
    m_updateBusy = true;
    const auto bail = [this](const QString& message) {
        if (m_panel) m_panel->showUpdateNotice(message);
        m_updateBusy = false;
    };
    // 1) 快速添加、检查项等未提交草稿不属于 flush() 保存范围，先要求用户处理。
    QString draftWhat;
    if (m_panel && m_panel->hasUncommittedDrafts(&draftWhat)) {
        bail(draftWhat + QStringLiteral("，请先提交或清空后再安装更新。"));
        return;
    }
    // 2) 与普通退出相同的保存准入：详情页、便签、编辑会话全部落盘。
    if (!prepareForExit()) {
        bail(QStringLiteral("部分内容尚未保存成功，本次暂不安装更新。"));
        return;
    }
    // 3) 更新前内容备份（.klb：任务、笔记与附件；不含桌面布局等本机状态）。
    const QString backupDir = dataDirPath() + QStringLiteral("/updates/backups");
    QDir().mkpath(backupDir);
    const QString backupPath = backupDir + QStringLiteral("/kylinlist-update-%1.klb")
                                   .arg(toAppTime(appNow()).toString(QStringLiteral("yyyyMMdd-HHmmss")));
    if (!m_store->exportBackup(backupPath)) {
        bail(QStringLiteral("更新前备份失败，已取消安装：%1").arg(m_store->lastError()));
        return;
    }
    pruneUpdateBackups();
    // 4) 安装前再次核对已下载的安装包。
    QString verifyError;
    if (!updates::verifyInstallerFile(filePath, artifact->size, artifact->sha256, &verifyError)) {
        bail(verifyError);
        return;
    }
    // 5) 记录待安装信息：下次启动按实际版本确认安装结果。
    Settings::PendingInstall pending;
    pending.valid = true;
    pending.version = m_updateService->manifest().version;
    pending.sha256 = artifact->sha256;
    pending.platform = artifact->platform;
    pending.arch = artifact->arch;
    pending.variant = artifact->variant;
    pending.fileName = QFileInfo(filePath).fileName();
    Settings::instance().setPendingInstall(pending);
    Settings::instance().sync();
    // 6) 交给系统安装器；交接成功即退出应用。
    QString handoffError;
    if (!updates::handOffToInstaller(filePath, &handoffError)) {
        bail(handoffError);
        return;
    }
    QApplication::quit();
}
void KylinListApp::evaluatePendingInstall() {
    Settings& settings = Settings::instance();
    const Settings::PendingInstall pending = settings.pendingInstall();
    if (!pending.valid) return;
    const QString appVersion = QStringLiteral(KYLINLIST_VERSION);
    switch (updates::evaluatePendingInstall(appVersion, pending.version)) {
    case updates::PendingOutcome::Completed: {
        settings.clearPendingInstall();
        settings.sync();
        const QDir cacheDir(dataDirPath() + QStringLiteral("/updates/cache"));
        const QString cached = cacheDir.filePath(pending.fileName);
        if (QFileInfo::exists(cached)
            && updates::verifyInstallerFile(cached, QFileInfo(cached).size(), pending.sha256,
                                            nullptr)) {
            QFile::remove(cached);
        }
        const QString text = QStringLiteral("已成功更新到 %1。").arg(pending.version);
        if (m_panel && m_panel->isVisible()) m_panel->showUpdateNotice(text);
        else if (m_tray) m_tray->notifyInfo(text);
        break;
    }
    case updates::PendingOutcome::Superseded:
        settings.clearPendingInstall();
        settings.sync();
        break;
    case updates::PendingOutcome::StillPending: {
        const QString text = QStringLiteral("上次的 %1 更新尚未完成，可从「备份与设置 ▸ 检查更新」重试。")
                                 .arg(pending.version);
        if (m_panel && m_panel->isVisible()) m_panel->showUpdateNotice(text);
        else if (m_tray) m_tray->notifyInfo(text);
        break;
    }
    case updates::PendingOutcome::None:
        break;
    }
}
void KylinListApp::pruneUpdateBackups() {
    const QDir dir(dataDirPath() + QStringLiteral("/updates/backups"));
    const QFileInfoList entries = dir.entryInfoList({QStringLiteral("kylinlist-update-*.klb")},
                                                    QDir::Files, QDir::Name);
    const int keep = 3;
    for (int i = 0; i + keep < entries.size(); ++i)
        QFile::remove(entries.at(i).absoluteFilePath());
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
    delete m_updateService;
    m_updateService = nullptr;
    Settings::instance().sync();
    Database::instance().close();
}
}  // namespace kl
