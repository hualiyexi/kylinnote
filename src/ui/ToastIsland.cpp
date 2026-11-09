#include "ToastIsland.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include "AppIcons.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

// 截图指定的固定配色，不属于应用主题令牌，也不随浅色/深色切换。
const QColor kBackground(QStringLiteral("#343434"));
const QColor kText(QStringLiteral("#F5F5F5"));
const QColor kAction(QStringLiteral("#D6B36A"));

QPixmap undoPixmap() {
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(kAction, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawArc(QRectF(5, 5, 22, 22), 35 * 16, 265 * 16);
    painter.drawPolyline(QPolygonF() << QPointF(5, 5) << QPointF(5, 12) << QPointF(12, 11));
    return pixmap;
}

}  // namespace

ToastIsland::ToastIsland(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("ToastIsland"));
    setAttribute(Qt::WA_StyledBackground, false);
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setFixedHeight(40);
    setCursor(Qt::ArrowCursor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 0, 10, 0);
    layout->setSpacing(12);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("ToastText"));
    m_text->setFont(ds::qfont(ds::FontScene::Body));
    QPalette textPalette = m_text->palette();
    textPalette.setColor(QPalette::WindowText, kText);
    m_text->setPalette(textPalette);
    layout->addWidget(m_text);
    m_action = new QToolButton(this);
    m_action->setObjectName(QStringLiteral("ToastAction"));
    m_action->setFixedSize(30, 30);
    m_action->setIconSize(QSize(20, 20));
    m_action->setIcon(QIcon(undoPixmap()));
    m_action->setAutoRaise(true);
    m_action->setCursor(Qt::PointingHandCursor);
    m_action->setToolTip(QStringLiteral("撤销"));
    m_action->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:0;border-radius:6px;padding:0;}"
        "QToolButton:hover{background:#484034;}"
        "QToolButton:pressed{background:#514632;}"));
    layout->addWidget(m_action);
    m_action->hide();
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ToastIsland::expire);
    connect(m_action, &QToolButton::clicked, this, [this]() {
        const auto callback = m_callback;
        m_timer.stop();
        m_callback = nullptr;
        hide();
        const bool succeeded = callback ? callback() : false;
        emit actionFinished(succeeded);
    });
    hide();
}

void ToastIsland::showMessage(const QString& text, int timeoutMs) {
    m_callback = nullptr;
    m_action->hide();
    present(text, timeoutMs);
}

void ToastIsland::showAction(const QString& text, const QString& accessibleAction,
                             std::function<bool()> action, int timeoutMs) {
    m_callback = std::move(action);
    m_action->setAccessibleName(accessibleAction);
    m_action->setToolTip(accessibleAction);
    m_action->show();
    present(text, timeoutMs);
}

void ToastIsland::present(const QString& text, int timeoutMs) {
    m_text->setText(text);
    m_text->setAccessibleName(text);
    m_remaining = qMax(0, timeoutMs);
    m_hovered = false;
    adjustSize();
    show();
    raise();
    if (m_remaining > 0) {
        m_clock.start();
        m_timer.start(m_remaining);
    }
}

int ToastIsland::remainingTime() const {
    if (!m_timer.isActive()) return m_remaining;
    return qMax(0, m_remaining - int(m_clock.elapsed()));
}

void ToastIsland::enterEvent(QEvent* event) {
    if (m_timer.isActive()) {
        m_remaining = remainingTime();
        m_timer.stop();
    }
    m_hovered = true;
    QFrame::enterEvent(event);
}

void ToastIsland::leaveEvent(QEvent* event) {
    m_hovered = false;
    if (isVisible() && m_remaining > 0) {
        m_clock.start();
        m_timer.start(m_remaining);
    }
    QFrame::leaveEvent(event);
}

void ToastIsland::expire() {
    // 原生鼠标进入事件在少数平台上可能晚于计时器，超时点再检查一次。
    if (underMouse()) {
        m_remaining = 0;
        m_hovered = true;
        return;
    }
    m_callback = nullptr;
    hide();
}

void ToastIsland::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kBackground);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 7, 7);
}

}  // namespace kl
