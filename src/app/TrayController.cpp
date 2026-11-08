#include "TrayController.h"
#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include "NoteManager.h"
namespace kl {
TrayController::TrayController(NoteManager* manager, QObject* parent) : QObject(parent), m_manager(manager) {
    m_tray = new QSystemTrayIcon(trayIcon(), this);
    m_tray->setToolTip(QStringLiteral("麒麟清单 · 我的待办"));
    m_menu = new QMenu;
    m_menu->setWindowFlags(m_menu->windowFlags() | Qt::WindowStaysOnTopHint);
    connect(m_menu->addAction(QStringLiteral("打开待办面板")),
            &QAction::triggered,
            this,
            &TrayController::panelRequested);
    connect(m_menu->addAction(QStringLiteral("新建任务")),
            &QAction::triggered,
            this,
            &TrayController::newTaskRequested);
    connect(m_menu->addAction(QStringLiteral("新建桌面便签")), &QAction::triggered, this, [this]() {
        m_manager->createNote();
    });
    m_menu->addSeparator();
    connect(m_menu->addAction(QStringLiteral("显示桌面便签")),
            &QAction::triggered,
            m_manager,
            &NoteManager::showAll);
    connect(m_menu->addAction(QStringLiteral("隐藏桌面便签")),
            &QAction::triggered,
            m_manager,
            &NoteManager::hideAll);
    m_menu->addSeparator();
    connect(
        m_menu->addAction(QStringLiteral("退出")), &QAction::triggered, this, &TrayController::quitRequested);
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
        m_tray->showMessage(QStringLiteral("麒麟清单"), text, QSystemTrayIcon::Warning, 7000);
}
void TrayController::notifyReminder(QString entityId, const QString& title) {
    m_lastReminderId = entityId;
    if (m_tray->isVisible() && QSystemTrayIcon::supportsMessages())
        m_tray->showMessage(QStringLiteral("待办提醒"), title, QSystemTrayIcon::Information, 15000);
}
QIcon TrayController::trayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#315BA6"));
    p.drawRoundedRect(5, 4, 54, 56, 12, 12);
    p.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(18, 31, 28, 41);
    p.drawLine(28, 41, 47, 22);
    return QIcon(pixmap);
}
}  // namespace kl
