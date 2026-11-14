#pragma once

#include "ModalDialog.h"

class QListWidget;
class QStackedWidget;
namespace kl {
class SmartListSettingsWidget;

class SettingsDialog : public ModalDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

signals:
    void smartListVisibilityChanged();

protected:
    void applyTheme() override;

private:
    QListWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    SmartListSettingsWidget* m_smartLists = nullptr;
};

}  // namespace kl
