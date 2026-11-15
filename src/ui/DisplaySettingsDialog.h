#pragma once

#include "ModalDialog.h"

class QComboBox;
class QRadioButton;

namespace kl {

// 对应网页“显示设置”：日期显示方式与窄窗口详情样式；不包含账号同步选项。
class DisplaySettingsDialog : public ModalDialog {
    Q_OBJECT
public:
    explicit DisplaySettingsDialog(bool countdownDates, bool popupDetail,
                                   QWidget* parent = nullptr);
    bool countdownDates() const;
    bool popupDetail() const;
signals:
    void optionsChanged(bool countdownDates, bool popupDetail);

protected:
    void applyTheme() override;

private:
    QComboBox* m_dateDisplay = nullptr;
    QRadioButton* m_sidebar = nullptr;
    QRadioButton* m_popup = nullptr;
};

}  // namespace kl
