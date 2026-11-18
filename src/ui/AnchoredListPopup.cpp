#include "AnchoredListPopup.h"

#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>

#include "PopupShadow.h"
#include "platform/SystemShadow.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr qreal kCardRadius = 12.0;

void activateContentLayouts(QWidget* content)
{
    const auto widgets = content->findChildren<QWidget*>();
    for (auto it = widgets.crbegin(); it != widgets.crend(); ++it) {
        if (QLayout* layout = (*it)->layout()) {
            layout->invalidate();
            layout->activate();
        }
    }
    if (QLayout* layout = content->layout()) {
        layout->invalidate();
        layout->activate();
    }
}
} // namespace

AnchoredListPopup::AnchoredListPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("AnchoredListPopup"));
    setAttribute(Qt::WA_TranslucentBackground);
    disableNativeWindowShadows();
    setModal(false);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(PopupShadowMargin, PopupShadowMargin, PopupShadowMargin, PopupShadowMargin);
    outer->setSpacing(0);
    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("AnchoredListContent"));
    outer->addWidget(m_content);
    // 父浮层被程序化关闭（accept/reject/hide）时，Popup 链不会自动带走子层窗口。
    if (parent) parent->installEventFilter(this);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        m_shadow = QImage();
        m_shadowColor = QColor();
        update();
    });
}

int AnchoredListPopup::shadowMargin()
{
    return PopupShadowMargin;
}

void AnchoredListPopup::setCardWidth(int width)
{
    m_cardWidth = qMax(0, width);
}

void AnchoredListPopup::setAnchorGap(int gap)
{
    m_anchorGap = qMax(0, gap);
}

void AnchoredListPopup::setFlexibleWidget(QWidget* widget, int preferredHeight, int minimumHeight)
{
    m_flexibleWidget = widget;
    m_flexiblePreferred = qMax(0, preferredHeight);
    m_flexibleMinimum = qBound(0, minimumHeight, m_flexiblePreferred);
}

void AnchoredListPopup::popupBelow(QWidget* anchor)
{
    if (!anchor) return;
    popupAt(QRect(anchor->mapToGlobal(QPoint()), anchor->size()));
}

void AnchoredListPopup::popupAt(const QRect& globalAnchor)
{
    m_anchorRect = globalAnchor;
    applyAnchorGeometry();
    show();
    raise();
    if (m_initialFocusWidget) m_initialFocusWidget->setFocus();
}

void AnchoredListPopup::relayout()
{
    if (!isVisible() || !m_anchorRect.isValid()) return;
    applyAnchorGeometry();
}

void AnchoredListPopup::done(int result)
{
    const auto combos = findChildren<QComboBox*>();
    for (QComboBox* combo : combos) combo->hidePopup();
    QDialog::done(result);
}

QRect AnchoredListPopup::cardRect() const
{
    return rect().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin);
}

QRect AnchoredListPopup::cardGeometryGlobal() const
{
    return QRect(cardRect().topLeft() + mapToGlobal(QPoint()), cardRect().size());
}

int AnchoredListPopup::fitCardHeight(int naturalHeight, int spaceHeight)
{
    if (spaceHeight >= naturalHeight) return naturalHeight;
    if (!m_flexibleWidget || m_flexiblePreferred <= 0) return -1;
    const int fixedPart = naturalHeight - m_flexiblePreferred;
    const int allowed = spaceHeight - fixedPart;
    if (allowed < m_flexibleMinimum) return -1;
    m_flexibleWidget->setFixedHeight(allowed);
    activateContentLayouts(m_content);
    return fixedPart + allowed;
}

void AnchoredListPopup::placeCard(int cardTop, int cardHeight, const QRect& area)
{
    const int cardWidth = m_cardWidth > 0 ? m_cardWidth
                                          : qMax(m_content->sizeHint().width(), 1);
    int cardLeft = m_alignRight ? m_anchorRect.right() - cardWidth + 1
                                : m_anchorRect.left();
    cardLeft = qBound(area.left(), cardLeft, qMax(area.left(), area.right() - cardWidth + 1));
    setFixedSize(cardWidth + 2 * PopupShadowMargin, cardHeight + 2 * PopupShadowMargin);
    move(cardLeft - PopupShadowMargin, cardTop - PopupShadowMargin);
}

void AnchoredListPopup::applyAnchorGeometry()
{
    // 先恢复伸缩区首选高度，再取内容自然尺寸，得到“未受限”的卡片高度。
    if (m_flexibleWidget && m_flexiblePreferred > 0)
        m_flexibleWidget->setFixedHeight(m_flexiblePreferred);
    activateContentLayouts(m_content);
    int naturalHeight = 2 * PopupShadowMargin;
    if (QLayout* contentLayout = m_content->layout()) {
        contentLayout->activate();
        naturalHeight = contentLayout->sizeHint().height();
    }

    QScreen* screen = QGuiApplication::screenAt(m_anchorRect.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect area = screen
        ? screen->availableGeometry()
        : QRect(QPoint(0, 0), QSize(m_cardWidth + 2 * PopupShadowMargin, naturalHeight + 2 * PopupShadowMargin));

    // 默认从入口下方 4px 展开；QRect::bottom() 是 top+height-1，先 +1 回到下缘外。
    const int belowTop = m_anchorRect.bottom() + 1 + m_anchorGap;
    int cardHeight = fitCardHeight(naturalHeight, area.bottom() - belowTop + 1);
    m_flippedAbove = false;
    if (cardHeight > 0) {
        placeCard(belowTop, cardHeight, area);
        return;
    }

    // 下方放不下（列表已压到下限仍不够）才上翻；上翻受全局上界约束，
    // 父级用它保证子弹层不会盖住整个父月历。
    int topLimit = area.top();
    if (m_upperBoundGlobalY > topLimit) topLimit = m_upperBoundGlobalY;
    const int aboveBottom = m_anchorRect.top() - 1 - m_anchorGap;
    cardHeight = fitCardHeight(naturalHeight, aboveBottom - topLimit + 1);
    m_flippedAbove = true;
    if (cardHeight > 0) {
        placeCard(aboveBottom - cardHeight + 1, cardHeight, area);
        return;
    }
    // 上界与最小伸缩高度都放不下时，先让卡片完整落在屏幕内。
    // 月历遮挡约束在这个兜底中让位于可操作性，避免页脚被挤出屏幕。
    cardHeight = fitCardHeight(naturalHeight, area.height());
    if (cardHeight < 1) cardHeight = qMin(naturalHeight, area.height());
    const int cardTop = qMax(area.top(), qMin(topLimit, area.bottom() - cardHeight + 1));
    placeCard(cardTop, qMax(1, cardHeight), area);
}

void AnchoredListPopup::rebuildShadow(const QColor& color)
{
    // 阴影 = 卡片形状向下偏移后的圆角 alpha 图整体高斯模糊，以 ds::Shadow 着色；
    // 按 (尺寸, 影色) 缓存，paint 只做 drawImage。
    m_shadow = popupShadow(size(), QRectF(cardRect()), kCardRadius, color);
    m_shadowColor = color;
}

void AnchoredListPopup::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const QColor shadowColor = ThemeManager::instance().color(ds::ColorToken::Shadow);
    if (m_shadow.size() != size() || m_shadowColor != shadowColor) rebuildShadow(shadowColor);
    if (!m_shadow.isNull()) {
        QPainter painter(this);
        painter.drawImage(0, 0, m_shadow);
    }
    {
        // 卡片底与边框画在阴影之上、子控件之下；颜色只来自 ds 语义令牌。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath card;
        card.addRoundedRect(QRectF(cardRect()), kCardRadius, kCardRadius);
        painter.fillPath(card, ThemeManager::instance().color(ds::ColorToken::Surface));
        QPen borderPen(ThemeManager::instance().color(ds::ColorToken::Border));
        borderPen.setWidthF(1.0);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(card);
    }
}

void AnchoredListPopup::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    m_shadow = QImage();
    m_shadowColor = QColor();
    update();
}

bool AnchoredListPopup::eventFilter(QObject* watched, QEvent* event)
{
    // 父浮层隐藏时只关本层：拒绝而不是直接 hide，保证 finished/焦点归还路径一致。
    if (watched == parent() && event->type() == QEvent::Hide && isVisible())
        reject();
    return QDialog::eventFilter(watched, event);
}

} // namespace kl
