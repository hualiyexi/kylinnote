#include "WindowChrome.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include "AppIcons.h"
#include "design/ThemeManager.h"

namespace kl {

WindowDragArea::WindowDragArea(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("WindowDragArea"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::ArrowCursor);
}

void WindowDragArea::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || window()->isMaximized()) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressGlobal = event->globalPos();
    m_windowPosition = window()->frameGeometry().topLeft();
    m_dragging = true;
    event->accept();
}

void WindowDragArea::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    window()->move(m_windowPosition + event->globalPos() - m_pressGlobal);
    event->accept();
}

void WindowDragArea::mouseReleaseEvent(QMouseEvent* event) {
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

void WindowDragArea::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        window()->setWindowState(window()->isMaximized() ? Qt::WindowNoState : Qt::WindowMaximized);
        event->accept();
    }
}

WindowControls::WindowControls(QWidget* parent) : QWidget(parent), m_window(parent) {
    setObjectName(QStringLiteral("WindowControls"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedSize(138, 36);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    const auto button = [this, layout](QToolButton*& target, const QString& name,
                                      const QString& tooltip, const QString& accessible) {
        target = new QToolButton(this);
        target->setObjectName(name);
        target->setFixedSize(46, 36);
        target->setAutoRaise(true);
        target->setToolTip(tooltip);
        target->setAccessibleName(accessible);
        target->setCursor(Qt::ArrowCursor);
        layout->addWidget(target);
    };
    button(m_minimize, QStringLiteral("WindowMinimize"), QStringLiteral("最小化"), QStringLiteral("最小化窗口"));
    button(m_maximize, QStringLiteral("WindowMaximize"), QStringLiteral("最大化"), QStringLiteral("最大化或还原窗口"));
    button(m_close, QStringLiteral("WindowClose"), QStringLiteral("关闭"), QStringLiteral("关闭窗口"));
    connect(m_minimize, &QToolButton::clicked, this, [this]() {
        if (m_window) m_window->showMinimized();
    });
    connect(m_maximize, &QToolButton::clicked, this, [this]() {
        if (!m_window) return;
        m_window->setWindowState(m_window->isMaximized() ? Qt::WindowNoState : Qt::WindowMaximized);
    });
    connect(m_close, &QToolButton::clicked, this, [this]() {
        if (m_window) m_window->close();
    });
    refreshIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &WindowControls::refreshIcons);
}

void WindowControls::refreshIcons() {
    m_minimize->setIcon(AppIcons::icon(IconId::WindowMinimize, 16));
    m_maximize->setIcon(AppIcons::icon(IconId::WindowMaximize, 16));
    m_close->setIcon(AppIcons::icon(IconId::WindowClose, 16));
}

}  // namespace kl
