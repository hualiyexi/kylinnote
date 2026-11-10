#include "WindowChrome.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>
#include "AppIcons.h"
#include "design/ThemeManager.h"

namespace kl {

WindowResizeArea::WindowResizeArea(QWidget* window, Qt::Edges edges, const QString& name)
    : QWidget(window), m_edges(edges) {
    setObjectName(name);
    setAttribute(Qt::WA_StyledBackground, false);
    setMouseTracking(true);
    if ((edges == (Qt::LeftEdge | Qt::TopEdge)) || (edges == (Qt::RightEdge | Qt::BottomEdge)))
        setCursor(Qt::SizeFDiagCursor);
    else if ((edges == (Qt::RightEdge | Qt::TopEdge)) || (edges == (Qt::LeftEdge | Qt::BottomEdge)))
        setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
    else setCursor(Qt::SizeVerCursor);
}

void WindowResizeArea::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    m_pressGlobal = event->globalPos();
    m_startGeometry = window()->geometry();
    m_dragging = true;
    event->accept();
}

void WindowResizeArea::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->globalPos() - m_pressGlobal;
    QRect next = m_startGeometry;
    if (m_edges & Qt::LeftEdge) next.setLeft(next.left() + delta.x());
    if (m_edges & Qt::RightEdge) next.setRight(next.right() + delta.x());
    if (m_edges & Qt::TopEdge) next.setTop(next.top() + delta.y());
    if (m_edges & Qt::BottomEdge) next.setBottom(next.bottom() + delta.y());
    const QSize minimum = window()->minimumSize();
    if (next.width() < minimum.width()) {
        if (m_edges & Qt::LeftEdge) next.setLeft(next.right() - minimum.width() + 1);
        else next.setWidth(minimum.width());
    }
    if (next.height() < minimum.height()) {
        if (m_edges & Qt::TopEdge) next.setTop(next.bottom() - minimum.height() + 1);
        else next.setHeight(minimum.height());
    }
    window()->setGeometry(next);
    event->accept();
}

void WindowResizeArea::mouseReleaseEvent(QMouseEvent* event) {
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}

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
    setAttribute(Qt::WA_StyledBackground, false);
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
        if (m_window && m_window->window()) m_window->window()->showMinimized();
        emit windowOperationFinished();
    });
    connect(m_maximize, &QToolButton::clicked, this, [this]() {
        QWidget* window = m_window ? m_window->window() : nullptr;
        if (!window) return;
        if (window->isMaximized()) window->showNormal();
        else window->showMaximized();
        emit windowOperationFinished();
    });
    connect(m_close, &QToolButton::clicked, this, [this]() {
        if (m_window && m_window->window()) m_window->window()->close();
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
