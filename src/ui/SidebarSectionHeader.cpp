#include "SidebarSectionHeader.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>
#include "NavigationGeometry.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {

SidebarSectionHeader::SidebarSectionHeader(const QString& title, const QString& objectName,
                                           QWidget* parent)
    : QFrame(parent) {
    setObjectName(objectName);
    setFixedHeight(36);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 3, 0, 3);
    outer->setSpacing(0);
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("SidebarSectionCard"));
    auto* layout = new QHBoxLayout(m_card);
    layout->setContentsMargins(NavigationGeometry::TitleLeft, 0, 4, 0);
    layout->setSpacing(4);
    m_title = new QLabel(title, m_card);
    m_title->setFont(ds::qfont(ds::FontScene::Caption));
    layout->addWidget(m_title, 1);
    outer->addWidget(m_card);

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

void SidebarSectionHeader::setLeadingIcon(IconId icon) {
    m_leadingIconId = icon;
    if (!m_leadingIcon) {
        m_leadingIcon = new QLabel(m_card);
        m_leadingIcon->setFixedSize(20, 20);
        m_leadingIcon->setAlignment(Qt::AlignCenter);
        if (auto* layout = qobject_cast<QHBoxLayout*>(m_card->layout()))
            layout->insertWidget(0, m_leadingIcon);
    }
    applyTheme();
}

void SidebarSectionHeader::setDisclosureEnabled(bool enabled) {
    if (m_disclosureEnabled == enabled) return;
    m_disclosureEnabled = enabled;
    if (enabled && !m_disclosure) {
        m_disclosure = new QToolButton(this);
        m_disclosure->setObjectName(objectName() + QStringLiteral("Disclosure"));
        m_disclosure->setFixedSize(12, 20);
        m_disclosure->setIconSize(QSize(NavigationGeometry::DisclosureGlyphSize,
                                       NavigationGeometry::DisclosureGlyphSize));
        m_disclosure->setFocusPolicy(Qt::StrongFocus);
        m_disclosure->setAutoRaise(true);
        m_disclosure->setCursor(Qt::PointingHandCursor);
        m_disclosure->setToolTip(QStringLiteral("展开或收起清单区域"));
        m_disclosure->setAccessibleName(m_disclosure->toolTip());
        m_disclosure->setStyleSheet(QStringLiteral(
            "QToolButton{padding:0;border:0;background:transparent;}"));
        m_disclosure->move(NavigationGeometry::DisclosureLeft,
                           (height() - m_disclosure->height()) / 2);
        m_disclosure->installEventFilter(this);
        connect(m_disclosure, &QToolButton::clicked,
                this, &SidebarSectionHeader::disclosureToggled);
    }
    if (m_disclosure) {
        m_disclosure->setVisible(enabled);
        if (enabled) m_disclosure->raise();
    }
    updateHover(m_hovered);
    applyTheme();
}

void SidebarSectionHeader::setFolderPresentation(bool enabled) {
    m_folderPresentation = enabled;
    if (auto* layout = qobject_cast<QHBoxLayout*>(m_card->layout()))
        layout->setSpacing(enabled ? NavigationGeometry::IconLabelGap : 4);
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
    if (m_disclosureEnabled && event->button() == Qt::LeftButton) {
        emit disclosureToggled();
        event->accept();
        return;
    }
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
    m_title->setFont(ds::qfont(m_folderPresentation ? ds::FontScene::Body : ds::FontScene::Caption));
    m_title->setStyleSheet(QStringLiteral("color:%1;font-weight:%2;")
        .arg(theme.color(m_folderPresentation ? ds::ColorToken::Text
                                              : ds::ColorToken::TextSecondary).name())
        .arg(m_folderPresentation ? 400 : 600));
    m_card->setStyleSheet(QStringLiteral(
        "QFrame#SidebarSectionCard{background:transparent;border:0;border-radius:6px;}"
        "QFrame#SidebarSectionCard[hovered=\"true\"]{background:%1;}")
        .arg(theme.color(ds::ColorToken::RowHover).name()));
    if (m_leadingIcon) {
        m_leadingIcon->setPixmap(
            AppIcons::icon(m_leadingIconId, ds::ColorToken::TextSecondary, 20).pixmap(20, 20));
    }
    if (m_action) {
        m_action->setIcon(AppIcons::icon(m_actionIcon, ds::ColorToken::TextSecondary, 14));
        if (m_collapseAction && !m_expanded) {
            const QPixmap down = m_action->icon().pixmap(14, 14);
            m_action->setIcon(QIcon(down.transformed(QTransform().rotate(-90))));
        }
    }
    if (m_disclosure) {
        m_disclosure->setIcon(AppIcons::icon(IconId::ChevronDown,
            ds::ColorToken::TextTertiary, NavigationGeometry::DisclosureGlyphSize));
        if (!m_expanded) {
            const QPixmap down = m_disclosure->icon().pixmap(
                NavigationGeometry::DisclosureGlyphSize, NavigationGeometry::DisclosureGlyphSize);
            m_disclosure->setIcon(QIcon(down.transformed(QTransform().rotate(-90))));
        }
        m_disclosure->setVisible(m_disclosureEnabled
            && (m_folderPresentation || m_hovered || m_disclosure->hasFocus()));
        if (m_disclosureEnabled) m_disclosure->raise();
    }
    if (m_action && !m_collapseAction && !m_hovered && !m_action->hasFocus())
        m_action->setIcon(QIcon());
}

void SidebarSectionHeader::updateHover(bool hovered) {
    m_hovered = hovered;
    if (m_disclosureEnabled) {
        m_card->setProperty("hovered", hovered);
        m_card->style()->unpolish(m_card);
        m_card->style()->polish(m_card);
    }
    applyTheme();
}

bool SidebarSectionHeader::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_action || watched == m_disclosure)
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
        applyTheme();
    return QFrame::eventFilter(watched, event);
}

}  // namespace kl
