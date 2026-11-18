#ifndef KYLINLIST_MODALDIALOG_H
#define KYLINLIST_MODALDIALOG_H

#include <QDialog>

class QHBoxLayout;
class QLabel;
class QToolButton;
class QVBoxLayout;

namespace kl {

// 无边框模态对话框基类：透明窗口 + 阴影边距 + 内部圆角卡片。
//
// 绘制模型说明（见 AGENTS.md）：Qt Widgets 没有 box-shadow，窗口级阴影必须
// 由应用自己实现。这里沿用 DetailPopup / DatePickerPopup 的成熟做法：
//   - Qt::Dialog | FramelessWindowHint | NoDropShadowWindowHint，窗口本体透明；
//   - 外层布局留出阴影边距，卡片用 QGraphicsDropShadowEffect 自绘投影
//     （颜色由 Text 令牌派生，主题切换时刷新）；
//   - 禁止 setMask（1bit 遮罩必然锯齿），圆角全部走 QSS border-radius。
//
// 子类通过 contentLayout() 填充正文、buttonLayout() 填充底部按钮。
// 标题栏右侧 Close 与 Escape 都等价于 reject()。
class ModalDialog : public QDialog {
    Q_OBJECT
public:
    explicit ModalDialog(QWidget* parent = nullptr);
    ~ModalDialog() override;

    void setDialogTitle(const QString& title);
    QString dialogTitle() const;

    // 供子类填充的正文布局与底部按钮布局（按钮布局已含前导弹簧，按钮默认右对齐）。
    QVBoxLayout* contentLayout() const { return m_contentLayout; }
    QHBoxLayout* buttonLayout() const { return m_buttonLayout; }
    // 圆角卡片本体；子类需要挂局部 QSS 时挂这里，不要挂到透明窗口上。
    QWidget* card() const { return m_card; }
    QWidget* header() const { return m_header; }

protected:
    // 重建卡片局部 QSS 与阴影颜色。子类可重载追加自己的规则，但需先调基类实现。
    virtual void applyTheme();
    void showEvent(QShowEvent* event) override;

private:
    QWidget* m_card = nullptr;
    QWidget* m_header = nullptr;
    QLabel* m_titleLabel = nullptr;
    QToolButton* m_closeButton = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    QHBoxLayout* m_buttonLayout = nullptr;
};

// 轻量确认框：标题 + 正文 + 确认/取消。取消为默认按钮，Escape、右上角关闭
// 一律取消（reject）。危险操作传 danger=true：确认按钮带 danger 动态属性，
// 外观由 ModalDialog 的局部 QSS 渲染为 Danger 令牌色。
class ConfirmDialog : public ModalDialog {
    Q_OBJECT
public:
    explicit ConfirmDialog(const QString& title, const QString& text,
                           const QString& confirmText, bool danger = false,
                           QWidget* parent = nullptr);

    // 便捷入口：exec 后返回用户是否点击了确认。
    static bool prompt(const QString& title, const QString& text,
                       const QString& confirmText, bool danger = false,
                       QWidget* parent = nullptr);

private:
    QLabel* m_textLabel = nullptr;
};

} // namespace kl

#endif // KYLINLIST_MODALDIALOG_H
