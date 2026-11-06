#include "TrayController.h"

#include "NoteManager.h"

#include "../Common.h"
#include "Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QScreen>

namespace kn {

TrayController::TrayController(NoteManager *manager, QObject *parent)
    : QObject(parent),
      m_manager(manager)
{
    m_tray = new QSystemTrayIcon(trayIcon(), this);
    m_tray->setToolTip(QStringLiteral("KylinNote"));

    buildMenu();
    m_tray->setContextMenu(m_menu);

    connect(m_tray, &QSystemTrayIcon::activated,
            this, [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    m_manager->showAll();
                }
            });

    m_retryTimer.setInterval(2000);
    m_retryTimer.setSingleShot(false);
    connect(&m_retryTimer, &QTimer::timeout, this, &TrayController::tryShow);
}

TrayController::~TrayController()
{
    delete m_menu;
    m_menu = nullptr;
}

void TrayController::buildMenu()
{
    m_menu = new QMenu();
    m_menu->setTitle(QStringLiteral("KylinNote"));
    // 便签带 WindowStaysOnTopHint，菜单需要同样置顶才不会被压住
    m_menu->setWindowFlags(m_menu->windowFlags() | Qt::WindowStaysOnTopHint);

    QAction *newNote = m_menu->addAction(QString::fromUtf8("\u65b0\u5efa\u4fbf\u7b3a\tCtrl+N"));
    connect(newNote, &QAction::triggered, this, [this]() {
        m_manager->createNote();
    });

    QAction *showAll = m_menu->addAction(QString::fromUtf8("\u663e\u793a\u5168\u90e8"));
    connect(showAll, &QAction::triggered, this, [this]() {
        m_manager->showAll();
    });

    QAction *hideAll = m_menu->addAction(QString::fromUtf8("\u9690\u85cf\u5168\u90e8"));
    connect(hideAll, &QAction::triggered, this, [this]() {
        m_manager->hideAll();
    });

    // 层级：贴在桌面（不遮挡其它软件）/ 普通 / 始终置顶
    QMenu *layerMenu = m_menu->addMenu(QString::fromUtf8("\u4fbf\u7b3a\u5c42\u7ea7"));  // 便签层位
    m_layerGroup = new QActionGroup(layerMenu);

    const NoteLayer layers[] = { NoteLayer::OnDesktop, NoteLayer::Normal, NoteLayer::OnTop };
    const char *labels[] = {
        "\u8d34\u5728\u684c\u9762\uff08\u4e0d\u906e\u6321\u5176\u5b83\u8f6f\u4ef6\uff09",  // 贴在桌面（不遮挡其它软件）
        "\u666e\u901a\u7a97\u53e3",                                                        // 普通窗口
        "\u59cb\u7ec8\u7f6e\u9876"                                                          // 始终置顶
    };

    for (int i = 0; i < LayerCount; ++i) {
        const NoteLayer layer = layers[i];
        QAction *item = layerMenu->addAction(QString::fromUtf8(labels[i]));
        item->setCheckable(true);
        item->setActionGroup(m_layerGroup);
        item->setData(layerToInt(layer));
        if (layer == Settings::instance().defaultLayer()) {
            item->setChecked(true);
        }
        connect(item, &QAction::triggered, this, [this, layer]() {
            Settings::instance().setDefaultLayer(layer);
            Settings::instance().sync();
            m_manager->applyLayerToAll(layer);
            notify(QString::fromUtf8("\u4fbf\u7b3a\u5c42\u7ea7\uff1a") + layerLabel(layer));
        });
    }

    m_menu->addSeparator();

    QAction *quit = m_menu->addAction(QString::fromUtf8("\u9000\u51fa"));
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);
}

void TrayController::show()
{
    m_retries = 0;
    tryShow();
}

void TrayController::tryShow()
{
    // 屏幕列表为空（窗管未就绪、无 X11）或托盘不可用时，
    // QSystemTrayIcon::show() 在部分 Qt/桌面补丁下会崩溃，必须先判断，
    // 不可用则等待重试，避免整个进程挂掉。
    if (QGuiApplication::screens().isEmpty() || !QSystemTrayIcon::isSystemTrayAvailable()) {
        if (m_retries >= 30) {
            m_retryTimer.stop();
            qWarning() << "[kylinnote] 系统托盘长时间不可用，已放弃等待";
            return;
        }
        ++m_retries;
        if (!m_retryTimer.isActive()) {
            m_retryTimer.start();
        }
        return;
    }

    m_retryTimer.stop();
    m_tray->show();
}

void TrayController::notify(const QString &text)
{
    if (m_tray->isVisible() && QSystemTrayIcon::supportsMessages()) {
        m_tray->showMessage(QStringLiteral("KylinNote"), text,
                            QSystemTrayIcon::Warning, 5000);
    }
}

QIcon TrayController::trayIcon()
{
    QIcon themed = QIcon::fromTheme(QStringLiteral("kylinnote"));
    if (!themed.isNull()) {
        return themed;
    }

    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0xC9, 0xB4, 0x58), 2));
    painter.setBrush(QColor(0xFE, 0xF7, 0xB0));
    painter.drawRect(8, 6, 48, 48);

    QPen linePen(QColor(0x8A, 0x7A, 0x2A), 3);
    linePen.setCapStyle(Qt::RoundCap);
    painter.setPen(linePen);
    for (int i = 0; i < 3; ++i) {
        const int y = 20 + i * 12;
        painter.drawLine(18, y, 46, y);
    }
    painter.end();

    return QIcon(pixmap);
}

} // namespace kn
