#ifndef KYLINLIST_COLORPICKERPOPUP_H
#define KYLINLIST_COLORPICKERPOPUP_H

#include <QDialog>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QWidget;

namespace kl {

// 内部自绘控件（实现见 .cpp）。
class SatValuePlane;   // 饱和度/明度平面
class HueBar;          // 横向色相条
class ColorPreviewDot; // 圆形颜色预览

// 清单颜色选择弹层（对齐滴答“选择颜色”浮层）：
//   第一页：五组预设色（马卡龙/莫兰迪/洛可可/经典/孟菲斯，每组 8 色）+
//           “自定义颜色”入口；
//   第二页：饱和度/明度平面 + 横向 hue 彩虹条 + 圆形预览 + #RRGGBB 输入。
//
// 弹层为透明 Popup 窗口（Frameless + NoDropShadow + WA_TranslucentBackground），
// 阴影由卡片上的 QGraphicsDropShadowEffect 自绘；颜色固定不透明 #RRGGBB，
// 不引入 alpha 通道。点击预设或自定义页“保存”时 emit colorSelected 并关闭；
// “取消”仅返回第一页；Escape / 点击外部关闭整个弹层（reject）。
class ColorPickerPopup : public QDialog {
    Q_OBJECT
public:
    explicit ColorPickerPopup(QWidget* parent = nullptr);
    ~ColorPickerPopup() override;

    // 初始颜色：进入自定义页时的光标位置与预设页的选中态。可为无效色。
    void setInitialColor(const QColor& color);
    // 自定义页当前颜色（无透明通道）。
    QColor currentColor() const;

    // 在锚点控件上/下方弹出（above=true 时向上），并约束在屏幕可用区域内。
    void popupAt(QWidget* anchor, bool above);

signals:
    void colorSelected(QColor color);

private:
    void buildPresetPage();
    void buildCustomPage();
    void applyTheme();
    void showPresetPage();
    void showCustomPage();
    void syncCustomFromColor(const QColor& color);
    void refreshCustomWidgets();
    bool commitCustomColor();
    void selectColor(const QColor& color);

    QWidget* m_card = nullptr;
    QStackedWidget* m_pages = nullptr;
    QPushButton* m_customEntry = nullptr;
    SatValuePlane* m_plane = nullptr;
    HueBar* m_hueBar = nullptr;
    ColorPreviewDot* m_previewDot = nullptr;
    QToolButton* m_backButton = nullptr;
    QLineEdit* m_hexEdit = nullptr;
    QLabel* m_hexError = nullptr;
    QVector<QToolButton*> m_presetSwatches;
    int m_hue = 0;     // 0..359
    int m_sat = 255;   // 0..255
    int m_val = 255;   // 0..255
    QColor m_initial;            // 精确初始色：未改动前原样回传，避免 HSV 往返舍入漂移
    bool m_customDirty = false;  // 用户是否动过自定义页（平面/色相条/HEX）
    bool m_updatingHex = false;
};

} // namespace kl

#endif // KYLINLIST_COLORPICKERPOPUP_H
