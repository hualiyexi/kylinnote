#ifndef KYLINLIST_ANCHOREDLISTPOPUP_H
#define KYLINLIST_ANCHOREDLISTPOPUP_H

#include <QColor>
#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QRect>

class QWidget;

namespace kl {

// 共享“锚定有界子弹层”：给日期选择器的时间 / 提醒 / 重复等内容较多的二级面板使用，
// 取代在固定尺寸父卡片里整页切换的结构。
//
// 职责边界：
//   - 窗口模型：Qt::Popup | Frameless | NoDropShadow + WA_TranslucentBackground；
//     阴影边距内自绘“圆角 alpha 图整体可分离高斯模糊”投影（不 setMask，
//     规则见 AGENTS.md 浮层硬性规则 4/5）。
//   - 锚定：默认从入口下方 4px 展开；可用 setAnchorGap/setAlignRight 覆盖个别层，
//     其余调用保持不变。下方空间不足先压缩“伸缩区”（通常是滚动列表），仍不足才整体上翻；
//     上翻时可设全局上界（例如父月历首行之下的约束），保证不会盖住整个父月历。
//     水平方向默认贴入口左缘，也可贴右缘，并夹入屏幕。
//   - 键盘：Escape / 外部点击只关闭本层（QDialog::reject），父层不受影响；
//     列表 Enter 走 itemActivated，输入框 Enter 走 returnPressed，由调用方连接。
//
// 内容由调用方塞进 content()；卡片宽度 setCardWidth；滚动区伸缩范围
// setFlexibleWidget(preferred, minimum)。父浮层隐藏时子层自动关闭。
class AnchoredListPopup : public QDialog {
    Q_OBJECT
public:
    explicit AnchoredListPopup(QWidget* parent = nullptr);

    // 内容宿主：调用方在上面挂布局（列表 + 固定输入/确认区）。
    QWidget* content() const { return m_content; }

    void setCardWidth(int width);
    void setAnchorGap(int gap);
    void setAlignRight(bool alignRight) { m_alignRight = alignRight; }
    // 空间不足时优先在 [minimum, preferred] 范围内压缩高度的控件（一般是滚动列表）。
    void setFlexibleWidget(QWidget* widget, int preferredHeight, int minimumHeight);
    // 上翻后卡片顶边不得高于该全局 Y（配合“不盖住整个父月历”）；<0 表示只用屏幕边界。
    void setUpperBoundGlobalY(int globalY) { m_upperBoundGlobalY = globalY; }
    void setInitialFocusWidget(QWidget* widget) { m_initialFocusWidget = widget; }

    void popupBelow(QWidget* anchor);
    void popupAt(const QRect& globalAnchor);
    // 页面切换或行内错误改变自然高度后，按原锚点重新限高和定位。
    void relayout();
    void done(int result) override;

    // 窗口坐标系内的卡片区域（不含阴影边距），供测试核对锚定与限高。
    QRect cardRect() const;
    QRect cardGeometryGlobal() const;
    bool flippedAbove() const { return m_flippedAbove; }

    static int shadowMargin();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    int fitCardHeight(int naturalHeight, int spaceHeight);
    void placeCard(int cardTop, int cardHeight, const QRect& area);
    void applyAnchorGeometry();
    void rebuildShadow(const QColor& color);

    QWidget* m_content = nullptr;
    QPointer<QWidget> m_flexibleWidget;
    QPointer<QWidget> m_initialFocusWidget;
    QRect m_anchorRect;
    int m_flexiblePreferred = 0;
    int m_flexibleMinimum = 0;
    int m_cardWidth = 0;
    int m_anchorGap = 4;
    bool m_alignRight = false;
    int m_upperBoundGlobalY = -1;
    bool m_flippedAbove = false;
    QImage m_shadow;
    QColor m_shadowColor;
};

} // namespace kl

#endif // KYLINLIST_ANCHOREDLISTPOPUP_H
