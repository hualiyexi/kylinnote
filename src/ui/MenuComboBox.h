#pragma once

#include <QComboBox>
#include <QPointer>

namespace kl {
class PopupMenu;

// 简短的单选菜单使用统一 PopupMenu；保留 QComboBox 的模型、信号与键盘选择接口。
class MenuComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit MenuComboBox(QWidget* parent = nullptr);
    void showPopup() override;
    void hidePopup() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPointer<PopupMenu> m_popup;
};
}  // namespace kl
