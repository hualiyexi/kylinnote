#pragma once

#include "ModalDialog.h"

class QCheckBox;
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
    void setCompletedVisible(bool visible);
signals:
    void optionsChanged(bool countdownDates, bool popupDetail);
    void completedVisibilityChanged(bool visible);

protected:
    void applyTheme() override;

private:
    QCheckBox* m_showCompleted = nullptr;
    QComboBox* m_dateDisplay = nullptr;
    QRadioButton* m_sidebar = nullptr;
    QRadioButton* m_popup = nullptr;
};

}  // namespace kl
