#include "TrayController.h"
#include <QAction>
#include <QGuiApplication>
#include "Branding.h"
#include "../ui/AppIcons.h"
#include "../ui/BrandingIcons.h"
#include "../ui/PopupMenu.h"
#include "../ui/TaskWidget.h"
#include "../ui/design/FontManager.h"
#include "../ui/design/ThemeManager.h"
namespace kl {
namespace {
const QString kLockLabel = QStringLiteral("锁定小部件");
const QString kUnlockLabel = QStringLiteral("解除小部件锁定");
const QString kLockShortcut = QStringLiteral("Ctrl+Shift+L");
}  // namespace
TrayController::TrayController(QObject* parent) : QObject(parent) {
    m_tray = new QSystemTrayIcon(trayIcon(), this);
    m_tray->setToolTip(Branding::displayName() + QStringLiteral(" · 我的待办"));
    m_menu = new PopupMenu;
    m_menu->setObjectName(QStringLiteral("TrayMenu"));
    m_menu->setProperty("entityMenu", true);
    m_menu->setWindowFlag(Qt::WindowStaysOnTopHint);
    auto* widgets = m_menu->addMenu(QStringLiteral("添加桌面小部件"));
    widgets->setObjectName(QStringLiteral("TrayWidgetsMenu"));
    widgets->setProperty("entityMenu", true);
    QAction* taskWidget = widgets->addAction(QStringLiteral("任务"));
    taskWidget->setObjectName(QStringLiteral("TrayAddTaskWidget"));
    connect(taskWidget, &QAction::triggered, this, &TrayController::addTaskWidgetRequested);

    m_topmostAction = m_menu->addAction(QStringLiteral("主窗口置顶"));
    m_topmostAction->setObjectName(QStringLiteral("TrayTopmost"));
    m_topmostAction->setCheckable(true);
    connect(m_topmostAction, &QAction::triggered, this, &TrayController::topmostRequested);

    m_lockAction = m_menu->addAction(kLockLabel);
    m_lockAction->setObjectName(QStringLiteral("TrayLockWidgets"));
    m_lockAction->setCheckable(true);
    m_lockAction->setVisible(TaskWidget::windowLockEnabled());
    connect(m_lockAction, &QAction::triggered, this, &TrayController::taskWidgetLockRequested);

    m_menu->addSeparator();
    QAction* quit = m_menu->addAction(QStringLiteral("退出"));
    quit->setObjectName(QStringLiteral("TrayQuit"));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);
    const auto refreshIcons = [this, widgets, taskWidget, quit]() {
        widgets->setIcon(AppIcons::icon(IconId::DesktopWidget, ds::ColorToken::Primary, 18));
        taskWidget->setIcon(AppIcons::icon(IconId::TaskList, 18));
        m_topmostAction->setIcon(AppIcons::icon(IconId::Pin, 18));
        m_lockAction->setIcon(AppIcons::icon(IconId::Lock, 18));
        quit->setIcon(AppIcons::icon(IconId::Exit, 18));
    };
    refreshIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, refreshIcons);
    connect(&FontManager::instance(), &FontManager::fontChanged, this, &TrayController::updateMenuWidth);
    updateMenuWidth();
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            emit panelRequested();
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (!m_lastReminderId.isEmpty())
            emit taskRequested(m_lastReminderId);
        else
            emit panelRequested();
    });
    m_retryTimer.setInterval(3000);
    connect(&m_retryTimer, &QTimer::timeout, this, &TrayController::tryShow);
}
TrayController::~TrayController() {
    delete m_menu;
}
void TrayController::setTopmostChecked(bool checked) {
    if (m_topmostAction) m_topmostAction->setChecked(checked);
}
void TrayController::setTaskWidgetLocked(bool locked) {
    m_lockAction->setChecked(locked);
    m_lockAction->setText((locked ? kUnlockLabel : kLockLabel)
        + (m_lockShortcutAvailable ? QLatin1Char('\t') + kLockShortcut : QString()));
}
void TrayController::setLockShortcutAvailable(bool available) {
    m_lockShortcutAvailable = available;
    setTaskWidgetLocked(m_lockAction->isChecked());
    updateMenuWidth();
}
void TrayController::updateMenuWidth() {
    // Qt 的 sizeHint 未计入自绘勾选与快捷键之间的空白，给最长状态预留宽度。
    const QFontMetrics metrics(m_menu->font());
    const int shortcutWidth = m_lockShortcutAvailable ? 12 + metrics.horizontalAdvance(kLockShortcut) : 0;
    const int lockWidth = metrics.horizontalAdvance(kUnlockLabel) + shortcutWidth + 84;
    m_menu->setMinimumWidth((TaskWidget::windowLockEnabled() ? qMax(224, lockWidth) : 224)
                            + 2 * PopupMenu::shadowMargin());
}
void TrayController::show() {
    tryShow();
}
void TrayController::tryShow() {
    if (!QGuiApplication::screens().isEmpty() && QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray->show();
        m_retryTimer.stop();
    } else if (!m_retryTimer.isActive())
        m_retryTimer.start();
}
void TrayController::notify(const QString& text) {
    m_lastReminderId.clear();
    if (m_tray->isVisible() && QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(Branding::displayName(), text, QSystemTrayIcon::Warning, 7000);
}
void TrayController::notifyInfo(const QString& text) {
    m_lastReminderId.clear();
    if (m_tray->isVisible() && QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(Branding::displayName(), text, QSystemTrayIcon::Information, 7000);
}
void TrayController::notifyReminder(QString entityId, const QString& title) {
    m_lastReminderId = entityId;
    if (m_tray->isVisible() && QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(QStringLiteral("待办提醒"), title, QSystemTrayIcon::Information, 15000);
}
QIcon TrayController::trayIcon() {
    // 软件级图标（窗口/任务栏/托盘）统一使用品牌 Logo 的白底版。
    // 默认品牌标绘制与品牌回退逻辑都在 BrandingIcons 中，此处只做委托，
    // 全程序不出现第二份默认 Logo 实现。
    return applicationBrandIcon();
}
}  // namespace kl
