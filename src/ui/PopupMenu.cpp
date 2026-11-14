#include "PopupMenu.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVector>
#include <cmath>
#include "design/ThemeManager.h"

namespace kl {
namespace {
// 阴影与卡片几何（对齐滴答清单菜单观感；单位为窗口逻辑像素）。
constexpr int kShadowMargin = 20;         // 透明边距 = QSS padding = 卡片内缩量
constexpr int kShadowOffsetY = 4;         // 阴影向下偏移
constexpr qreal kCardRadius = 12.0;       // 卡片圆角
constexpr qreal kShadowPeakAlpha = 0.18;  // 高斯场中心峰值（需求 0.15~0.2 的中值）
// 可分离高斯参数，与 WorkspaceSurface 的窗口外阴影一致：sigma 6、核半径 18
// （约等于 18px 的柔和模糊半径）。
constexpr qreal kSigma = 6.0;
constexpr int kKernelRadius = 18;

// 对整张圆角 alpha 图做一次可分离高斯模糊。直边与转角共享同一连续场，
// 不存在分区渐变在圆角旁叠加形成的直角接缝（思路同 WorkspaceSurface，
// 本文件独立实现，不含边到边拉伸，直接生成整窗实际尺寸并缓存）。
QImage blurredShadowImage(const QSize& size, const QRectF& cardRect, const QColor& color) {
    if (size.isEmpty()) return QImage();
    QImage source(size, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    {
        QPainter painter(&source);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRoundedRect(cardRect, kCardRadius, kCardRadius);
    }

    const int width = size.width();
    const int height = size.height();
    QVector<double> input(width * height);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        for (int x = 0; x < width; ++x) input[y * width + x] = qAlpha(row[x]) / 255.0;
    }

    QVector<double> kernel(kKernelRadius * 2 + 1);
    double sum = 0.0;
    for (int i = -kKernelRadius; i <= kKernelRadius; ++i) {
        const double value = std::exp(-(i * i) / (2.0 * kSigma * kSigma));
        kernel[i + kKernelRadius] = value;
        sum += value;
    }
    for (double& value : kernel) value /= sum;

    QVector<double> temporary(width * height, 0.0), output(width * height, 0.0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -kKernelRadius; k <= kKernelRadius; ++k)
                temporary[y * width + x] +=
                    input[y * width + qBound(0, x + k, width - 1)] * kernel[k + kKernelRadius];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            for (int k = -kKernelRadius; k <= kKernelRadius; ++k)
                output[y * width + x] +=
                    temporary[qBound(0, y + k, height - 1) * width + x] * kernel[k + kKernelRadius];

    QImage shadow(size, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(shadow.scanLine(y));
        for (int x = 0; x < width; ++x) {
            QColor pixel(color);
            pixel.setAlphaF(qMin(1.0, output[y * width + x] * kShadowPeakAlpha));
            row[x] = pixel.rgba();
        }
    }
    return shadow.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
}  // namespace

PopupMenu::PopupMenu(QWidget* parent) : QMenu(parent) {
    // 浮层规则（AGENTS.md 硬性规则 5）：Popup + Frameless + NoDropShadow +
    // WA_TranslucentBackground，阴影边距留给自绘；不 setMask，无锯齿遮罩。
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    // 局部 QSS：原生 menu panel 透明，padding 预留阴影边距。QStyleSheetStyle 会
    // 把 padding 映射进 PM_MenuHMargin/PM_MenuVMargin（Qt 5.12 已核实），QMenu
    // 的 action 布局随之内缩；border:none + margin:0 令 PM_MenuPanelWidth 为 0。
    // 属性选择器仅作用于本组件，保留挂入的普通 QMenu 的原有样式。
    setProperty("popupMenu", true);
    setStyleSheet(QStringLiteral(
                      "QMenu[popupMenu=\"true\"]{background:transparent;border:0;margin:0;padding:%1px;}")
                      .arg(kShadowMargin + 4));
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

PopupMenu* PopupMenu::addMenu(const QString& title) {
    auto* menu = new PopupMenu(title, this);
    QMenu::addMenu(menu);
    return menu;
}

PopupMenu* PopupMenu::addMenu(const QIcon& icon, const QString& title) {
    auto* menu = new PopupMenu(title, this);
    menu->setIcon(icon);
    QMenu::addMenu(menu);
    return menu;
}

QAction* PopupMenu::addMenu(QMenu* menu) {
    return QMenu::addMenu(menu);
}

int PopupMenu::shadowMargin() {
    return kShadowMargin;
}

bool PopupMenu::event(QEvent* event) {
    // Qt 5.12 的 QMenu::popup 不是 virtual，exec()/子菜单内部弹出都绕不开它，
    // 所以统一在 QEvent::Show（透明窗口首次合成之前）把窗口平移一个阴影边距：
    // 卡片左上角落在 exec/popup 请求的全局点，子菜单卡片贴住父项右缘并对齐
    // 父项所在行。RTL 时水平方向镜像（卡片右缘对齐请求点）。
    // popup() 每次都会先 setGeometry 再 show，此平移不会累积。
    if (event->type() == QEvent::Show && isWindow()) {
        const int dx = isRightToLeft() ? kShadowMargin : -kShadowMargin;
        move(pos() + QPoint(dx, -kShadowMargin));
    }
    return QMenu::event(event);
}

QRectF PopupMenu::cardRect() const {
    return QRectF(rect()).adjusted(kShadowMargin, kShadowMargin,
                                   -kShadowMargin, -kShadowMargin);
}

void PopupMenu::rebuildShadow(const QColor& color) {
    // 阴影 = 卡片形状向下偏移 kShadowOffsetY 后的圆角 alpha 图，整体高斯模糊，
    // 以 ds::Shadow 着色；按 (尺寸, 影色) 缓存，paint 只做 drawImage。
    m_shadow = blurredShadowImage(size(), cardRect().translated(0, kShadowOffsetY), color);
    m_shadowColor = color;
}

void PopupMenu::paintEvent(QPaintEvent* event) {
    const QColor shadowColor = ThemeManager::instance().color(ds::ColorToken::Shadow);
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
        painter.fillPath(card, ThemeManager::instance().color(ds::ColorToken::MenuBg));
        QPen borderPen(ThemeManager::instance().color(ds::ColorToken::Border));
        borderPen.setWidthF(1.0);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(card);
    }
    // item 文字/图标/分隔线/选中高亮仍由 QMenu + 全局 QSS 绘制，压在卡片之上。
    QMenu::paintEvent(event);
}

}  // namespace kl
