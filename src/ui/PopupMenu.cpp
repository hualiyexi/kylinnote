#include "PopupMenu.h"

#include <QEvent>
#include <QAction>
#include <QPainter>
#include <QWidgetAction>
#include <QPainterPath>
#include <QKeySequence>
#include <QStyle>
#include "AppIcons.h"
#include "PopupShadow.h"
#include "TextGeometry.h"
#include "platform/SystemShadow.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr qreal kCardRadius = 12.0;

void drawTextAtInkCenter(QPainter& painter, const QRectF& rect, int flags, const QString& text) {
    if (rect.isEmpty() || text.isEmpty()) return;
    const QFontMetricsF metrics(painter.fontMetrics());
    const qreal baseline = textBaselineForCenter(metrics, rect.center().y(), text);
    const qreal alignedBaseline = rect.top() + (rect.height() - metrics.height()) / 2.0
        + metrics.ascent();
    // QPainter 的矩形路径保留 TextShowMnemonic、RTL 与 leading/trailing，
    // 只把它的默认基线平移到当前实际文字墨迹的中线。
    painter.drawText(rect.translated(0, baseline - alignedBaseline), flags, text);
}
}  // namespace

PopupMenu::PopupMenu(QWidget* parent) : QMenu(parent) {
    // 浮层规则（AGENTS.md 硬性规则 5）：Popup + Frameless + NoDropShadow +
    // WA_TranslucentBackground，阴影边距留给自绘；不 setMask，无锯齿遮罩。
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    disableNativeWindowShadows();
    // 局部 QSS：原生 menu panel 透明，padding 预留阴影边距。QStyleSheetStyle 会
    // 把 padding 映射进 PM_MenuHMargin/PM_MenuVMargin（Qt 5.12 已核实），QMenu
    // 的 action 布局随之内缩；border:none + margin:0 令 PM_MenuPanelWidth 为 0。
    // 属性选择器仅作用于本组件，保留挂入的普通 QMenu 的原有样式。
    setProperty("popupMenu", true);
    setCardPadding(4, 4);
    // 全局 QSS 由 ThemeManager 重铺；这里只需失效阴影缓存，下次 paint 按当前
    // 主题的 ds::Shadow 重新生成（影色是缓存键的一部分）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        m_shadow = QImage();
        m_shadowColor = QColor();
        update();
    });
}

PopupMenu::PopupMenu(const QString& title, QWidget* parent) : PopupMenu(parent) {
    setTitle(title);
}

void PopupMenu::setThemeOverride(ds::Theme theme) {
    m_themeOverride = theme;
    m_themeOverrideActive = true;
    // 影色是阴影缓存键的一部分；主题口径变化后失效，下次 paint 按所选主题重建。
    m_shadow = QImage();
    m_shadowColor = QColor();
    update();
}

void PopupMenu::clearThemeOverride() {
    m_themeOverrideActive = false;
    m_shadow = QImage();
    m_shadowColor = QColor();
    update();
}

bool PopupMenu::hasThemeOverride() const {
    return m_themeOverrideActive;
}

QColor PopupMenu::tokenColor(ds::ColorToken token) const {
    return m_themeOverrideActive ? ds::color(token, m_themeOverride)
                                 : ThemeManager::instance().color(token);
}

PopupMenu* PopupMenu::addMenu(const QString& title) {
    auto* menu = new PopupMenu(title, this);
    // 子菜单视觉上属于同一浮层，跟随父菜单的主题口径取色。
    if (m_themeOverrideActive) menu->setThemeOverride(m_themeOverride);
    QMenu::addMenu(menu);
    return menu;
}

PopupMenu* PopupMenu::addMenu(const QIcon& icon, const QString& title) {
    auto* menu = new PopupMenu(title, this);
    if (m_themeOverrideActive) menu->setThemeOverride(m_themeOverride);
    menu->setIcon(icon);
    QMenu::addMenu(menu);
    return menu;
}

QAction* PopupMenu::addMenu(QMenu* menu) {
    return QMenu::addMenu(menu);
}

int PopupMenu::shadowMargin() {
    return PopupShadowMargin;
}

void PopupMenu::setCardPadding(int horizontal, int vertical) {
    setStyleSheet(QStringLiteral(
        "QMenu[popupMenu=\"true\"]{background:transparent;border:0;margin:0;padding:%1px %2px;}")
        .arg(PopupShadowMargin + vertical).arg(PopupShadowMargin + horizontal));
}

bool PopupMenu::event(QEvent* event) {
    // Qt 5.12 的 QMenu::popup 不是 virtual，exec()/子菜单内部弹出都绕不开它，
    // 所以统一在 QEvent::Show（透明窗口首次合成之前）把窗口平移一个阴影边距：
    // 卡片左上角落在 exec/popup 请求的全局点，子菜单卡片贴住父项右缘并对齐
    // 父项所在行。RTL 时水平方向镜像（卡片右缘对齐请求点）。
    // popup() 每次都会先 setGeometry 再 show，此平移不会累积。
    if (event->type() == QEvent::Show && isWindow()) {
        const int dx = isRightToLeft() ? PopupShadowMargin : -PopupShadowMargin;
        move(pos() + QPoint(dx, -PopupShadowMargin));
    }
    return QMenu::event(event);
}

QRectF PopupMenu::cardRect() const {
    return QRectF(rect()).adjusted(PopupShadowMargin, PopupShadowMargin,
                                   -PopupShadowMargin, -PopupShadowMargin);
}

void PopupMenu::rebuildShadow(const QColor& color) {
    // 统一的整图高斯阴影；保留菜单的尺寸、圆角、偏移和缓存行为。
    m_shadow = popupShadow(size(), cardRect(), kCardRadius, color);
    m_shadowColor = color;
}

void PopupMenu::paintEvent(QPaintEvent* event) {
    const QColor shadowColor = tokenColor(ds::ColorToken::Shadow);
    if (m_shadow.size() != size() || m_shadowColor != shadowColor) rebuildShadow(shadowColor);
    if (!m_shadow.isNull()) {
        QPainter painter(this);
        painter.drawImage(0, 0, m_shadow);
    }
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath card;
        card.addRoundedRect(cardRect(), kCardRadius, kCardRadius);
        painter.fillPath(card, tokenColor(ds::ColorToken::MenuBg));
        QPen borderPen(tokenColor(ds::ColorToken::Border));
        borderPen.setWidthF(1.0);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(card);
    }
    // QMenu 继续负责动作几何、键盘和子菜单；实体菜单内容只绘制一次，
    // 避免 Qt 5.12 的 PM_SmallIconSize=16 缩图及事后覆盖留下的重复墨迹。
    if (property("entityMenu").toBool()) paintActions();
    else QMenu::paintEvent(event);
}

void PopupMenu::paintActions() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font());
    const int left = qRound(cardRect().left()) + 1;
    const int right = qRound(cardRect().right()) - 1;
    for (QAction* action : actions()) {
        if (!action->isVisible()) continue;
        const QRect row = actionGeometry(action);
        if (action->isSeparator()) {
            painter.fillRect(QRect(left, row.center().y(), qMax(0, right - left + 1), 1),
                             tokenColor(ds::ColorToken::Divider));
            continue;
        }
        if (qobject_cast<QWidgetAction*>(action)) continue;
        const bool enabled = action->isEnabled();
        if (enabled && action == activeAction()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(tokenColor(ds::ColorToken::MenuHover));
            painter.drawRoundedRect(QRectF(row), 8, 8);
        }
        const auto visual = [this, &row](const QRect& rect) {
            return QStyle::visualRect(layoutDirection(), row, rect);
        };
        int textLeft = row.left() + 12;
        if (!action->icon().isNull()) {
            const QRect glyph(row.left() + 12, row.top() + (row.height() - 18) / 2, 18, 18);
            action->icon().paint(&painter, visual(glyph), Qt::AlignCenter,
                                 enabled ? QIcon::Normal : QIcon::Disabled);
            textLeft += 18 + 8;
        }
        int textRight = row.right() - 12;
        if (action->menu()) {
            const QRect arrow(textRight - 11, row.top() + (row.height() - 12) / 2, 12, 12);
            painter.save();
            if (isRightToLeft()) {
                painter.translate(visual(arrow).center());
                painter.rotate(180);
                AppIcons::paint(&painter, QStringLiteral("chevron-right"), QRectF(-6, -6, 12, 12),
                                tokenColor(enabled ? ds::ColorToken::TextTertiary : ds::ColorToken::MenuDisabled));
            } else AppIcons::paint(&painter, QStringLiteral("chevron-right"), arrow,
                                    tokenColor(enabled ? ds::ColorToken::TextTertiary : ds::ColorToken::MenuDisabled));
            painter.restore();
            textRight -= 20;
        } else if (action->isChecked()) {
            const QRect check(textRight - 15, row.top() + (row.height() - 16) / 2, 16, 16);
            AppIcons::paint(&painter, IconId::Check, visual(check),
                            tokenColor(enabled ? ds::ColorToken::Primary : ds::ColorToken::MenuDisabled));
            textRight -= 24;
        }
        QString label = action->text();
        QString trailing = action->property("trailingText").toString();
        const int tab = label.indexOf(QLatin1Char('\t'));
        if (tab >= 0) {
            trailing = label.mid(tab + 1);
            label = label.left(tab);
        } else if (trailing.isEmpty() && !action->shortcut().isEmpty()) {
            trailing = action->shortcut().toString(QKeySequence::NativeText);
        }
        if (!trailing.isEmpty()) {
            const int width = painter.fontMetrics().horizontalAdvance(trailing);
            const QRect suffix(textRight - width + 1, row.top(), width, row.height());
            painter.setPen(tokenColor(enabled ? ds::ColorToken::TextTertiary : ds::ColorToken::MenuDisabled));
            drawTextAtInkCenter(painter, QRectF(visual(suffix)),
                                Qt::AlignVCenter | Qt::AlignTrailing, trailing);
            textRight -= width + 12;
        }
        painter.setPen(tokenColor(!enabled ? ds::ColorToken::MenuDisabled
            : action->isChecked() ? ds::ColorToken::Primary : ds::ColorToken::Text));
        const QRect text(textLeft, row.top(), qMax(0, textRight - textLeft + 1), row.height());
        const QString shown = painter.fontMetrics().elidedText(label, Qt::ElideRight, text.width());
        drawTextAtInkCenter(painter, QRectF(visual(text)),
                            Qt::AlignVCenter | Qt::AlignLeading | Qt::TextShowMnemonic, shown);
    }
}

}  // namespace kl
