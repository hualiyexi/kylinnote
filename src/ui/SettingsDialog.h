#pragma once

#include "ModalDialog.h"
#include <QPointer>

class QCheckBox;
class QLabel;
class QListWidget;
class QStackedWidget;
class QComboBox;
namespace kl {
class AutostartSettings;
class SmartListSettingsWidget;

class SettingsDialog : public ModalDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(AutostartSettings* autostart, QWidget* parent = nullptr);

signals:
    void smartListVisibilityChanged();
    void taskWidgetRequested();

protected:
    void applyTheme() override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void syncChromeGeometry();
    void syncAutostart();

private:
    QPointer<AutostartSettings> m_autostart;
    QListWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
    QCheckBox* m_autostartSwitch = nullptr;
    QLabel* m_autostartHint = nullptr;
    QLabel* m_autostartError = nullptr;
    QWidget* m_appearance = nullptr;
    QWidget* m_leftSurface = nullptr;
    QComboBox* m_theme = nullptr;
    QComboBox* m_font = nullptr;
    SmartListSettingsWidget* m_smartLists = nullptr;
};

}  // namespace kl
