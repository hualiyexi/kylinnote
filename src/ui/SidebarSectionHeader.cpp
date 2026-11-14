#include "SidebarSectionHeader.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {

SidebarSectionHeader::SidebarSectionHeader(const QString& title, const QString& objectName,
                                           QWidget* parent)
    : QFrame(parent) {
    setObjectName(objectName);
    setFixedHeight(36);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 3, 6, 3);
    outer->setSpacing(0);
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("SidebarSectionCard"));
    auto* layout = new QHBoxLayout(m_card);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(4);
    m_title = new QLabel(title, m_card);
    m_title->setFont(ds::qfont(ds::FontScene::Caption));
    layout->addWidget(m_title, 1);
    outer->addWidget(m_card);

    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(16);
    shadow->setOffset(0, 2);
    m_card->setGraphicsEffect(shadow);
    updateHover(false);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SidebarSectionHeader::applyTheme);
    applyTheme();
}

void SidebarSectionHeader::setAction(IconId icon, const QString& toolTip,
                                     const QString& objectName, bool collapseAction) {
    if (m_action) return;
    m_actionIcon = icon;
    m_collapseAction = collapseAction;
    m_action = new QToolButton(m_card);
    m_action->setObjectName(objectName);
    m_action->setFixedSize(22, 22);
    m_action->setIconSize(QSize(14, 14));
    m_action->setFocusPolicy(Qt::StrongFocus);
    m_action->installEventFilter(this);
    m_action->setAutoRaise(true);
    m_action->setCursor(Qt::PointingHandCursor);
    m_action->setToolTip(toolTip);
    m_action->setAccessibleName(toolTip);
    m_action->setStyleSheet(QStringLiteral("QToolButton{padding:0;border:0;}"));
    m_card->layout()->addWidget(m_action);
    connect(m_action, &QToolButton::clicked, this, &SidebarSectionHeader::actionTriggered);
    updateHover(m_hovered);
    applyTheme();
}

void SidebarSectionHeader::setExpanded(bool expanded) {
    if (m_expanded == expanded) return;
    m_expanded = expanded;
    applyTheme();
}

void SidebarSectionHeader::enterEvent(QEvent* event) {
    updateHover(true);
    QFrame::enterEvent(event);
}

void SidebarSectionHeader::leaveEvent(QEvent* event) {
    updateHover(false);
    QFrame::leaveEvent(event);
}

void SidebarSectionHeader::mouseReleaseEvent(QMouseEvent* event) {
    if (m_collapseAction && event->button() == Qt::LeftButton
        && m_card->rect().contains(m_card->mapFromParent(event->pos()))) {
        emit actionTriggered();
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void SidebarSectionHeader::applyTheme() {
    auto& theme = ThemeManager::instance();
    m_title->setStyleSheet(QStringLiteral("color:%1;font-weight:600;")
        .arg(theme.color(ds::ColorToken::TextSecondary).name()));
    m_card->setStyleSheet(QStringLiteral(
        "QFrame#SidebarSectionCard{background:transparent;border:0;border-radius:6px;}"
        "QFrame#SidebarSectionCard[hovered=\"true\"]{background:%1;}")
        .arg(theme.color(ds::ColorToken::Surface).name()));
    if (auto* shadow = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor color = theme.color(ds::ColorToken::Shadow);
        color.setAlpha(30);
        shadow->setColor(color);
    }
    if (m_action) {
        m_action->setIcon(AppIcons::icon(m_actionIcon, ds::ColorToken::TextSecondary, 14));
        if (m_collapseAction && !m_expanded) {
            const QPixmap down = m_action->icon().pixmap(14, 14);
            m_action->setIcon(QIcon(down.transformed(QTransform().rotate(-90))));
        }
    }
    if (m_action && !m_collapseAction && !m_hovered && !m_action->hasFocus())
        m_action->setIcon(QIcon());
}

void SidebarSectionHeader::updateHover(bool hovered) {
    m_hovered = hovered;
    m_card->setProperty("hovered", hovered);
    m_card->style()->unpolish(m_card);
    m_card->style()->polish(m_card);
    if (m_card->graphicsEffect()) m_card->graphicsEffect()->setEnabled(hovered);
    applyTheme();
}

bool SidebarSectionHeader::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_action
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
        applyTheme();
    return QFrame::eventFilter(watched, event);
}

}  // namespace kl
