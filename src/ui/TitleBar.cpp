#include "TitleBar.h"
#include "AppIcons.h"
#include "design/ThemeManager.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>

namespace kl {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TitleBar"));
    setAccessibleName(QStringLiteral("拖动桌面便签"));
    setFixedHeight(20);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::ArrowCursor);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 1, 8, 1);
    layout->setSpacing(4);
    layout->addStretch(1);
    const auto button = [this, layout](const QString& name, const QString& label) {
        auto* control = new QToolButton(this);
        control->setObjectName(name);
        control->setFixedSize(18, 18);
        control->setIconSize(QSize(12, 12));
        control->setAutoRaise(true);
        control->setFocusPolicy(Qt::NoFocus);
        control->setToolTip(label);
        control->setAccessibleName(label);
        control->setCursor(Qt::PointingHandCursor);
        QSizePolicy policy = control->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        control->setSizePolicy(policy);
        layout->addWidget(control);
        return control;
    };
    m_colorButton = button(QStringLiteral("ColorButton"), QStringLiteral("便签颜色"));
    m_pinButton = button(QStringLiteral("NotePinButton"), QStringLiteral("置顶便签"));
    m_pinButton->setCheckable(true);
    m_closeButton = button(QStringLiteral("CloseButton"), QStringLiteral("关闭便签，保留任务"));
    const auto icons = [this]() {
        m_colorButton->setIcon(AppIcons::icon(IconId::Theme, ds::ColorToken::TextTertiary, 12));
        m_pinButton->setIcon(AppIcons::icon(IconId::Pin,
            m_pinned ? ds::ColorToken::Primary : ds::ColorToken::TextTertiary, 12));
        m_closeButton->setIcon(AppIcons::icon(IconId::Close, ds::ColorToken::TextTertiary, 12));
    };
    icons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [icons](ds::Theme) { icons(); });
    connect(m_colorButton, &QToolButton::clicked, this, &TitleBar::colorClicked);
    connect(m_pinButton, &QToolButton::clicked, this, &TitleBar::pinClicked);
    connect(m_closeButton, &QToolButton::clicked, this, &TitleBar::closeClicked);
    setHovered(false);
}

void TitleBar::setHovered(bool hovered) {
    m_colorButton->setVisible(hovered);
    m_pinButton->setVisible(hovered);
    m_closeButton->setVisible(hovered);
}

void TitleBar::setColorIndicator(NoteColor) {
    m_colorButton->setIcon(AppIcons::icon(IconId::Theme, ds::ColorToken::TextTertiary, 12));
}

void TitleBar::setPinned(bool pinned) {
    if (m_pinned == pinned) return;
    m_pinned = pinned;
    m_pinButton->setChecked(pinned);
    const auto token = pinned ? ds::ColorToken::Primary : ds::ColorToken::TextTertiary;
    m_pinButton->setIcon(AppIcons::icon(IconId::Pin, token, 12));
    m_pinButton->setToolTip(pinned ? QStringLiteral("取消便签置顶") : QStringLiteral("置顶便签"));
}

void TitleBar::setMoveConstraint(std::function<QPoint(const QPoint&)> constraint) {
    m_moveConstraint = std::move(constraint);
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    m_dragging = true;
    m_dragStartGlobal = event->globalPos();
    m_dragStartWindowPos = window()->pos();
    event->accept();
}

void TitleBar::mouseMoveEvent(QMouseEvent* event) {
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) return QWidget::mouseMoveEvent(event);
    const QPoint raw = m_dragStartWindowPos + event->globalPos() - m_dragStartGlobal;
    window()->move(m_moveConstraint ? m_moveConstraint(raw) : raw);
    event->accept();
}

void TitleBar::mouseReleaseEvent(QMouseEvent* event) {
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

} // namespace kl
