#ifndef KYLINLIST_DATEPICKERRECURRENCEEDITOR_H
#define KYLINLIST_DATEPICKERRECURRENCEEDITOR_H

#include <QObject>
#include <QVector>

#include "../domain/RecurrenceRule.h"

class QCheckBox;
class QLabel;
class QScrollArea;
class QSpinBox;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace kl {

class AnchoredListPopup;
class DateCalendarPanel;
class DatePickerMenuRow;
class DatePickerPopup;
class MenuComboBox;

// 重复预设与自定义工作区。只编辑 RecurrenceSpec 草稿，不直接写任务库。
class DatePickerRecurrenceEditor final : public QObject {
    Q_OBJECT
public:
    explicit DatePickerRecurrenceEditor(DatePickerPopup* owner);

    AnchoredListPopup* popup() const { return m_popup; }
    void openPresets();
    void openCustom();

private:
    QWidget* buildPresetPage();
    QWidget* buildCustomPage();
    void rebuildPresetRows();
    void addPresetRow(QVBoxLayout* layout, const QString& text, int preset);
    RecurrenceSpec presetSpec(int preset) const;
    void loadCustomControls();
    void updateFormVisibility();
    void updatePageGeometry();
    bool gather(RecurrenceSpec* spec, QString* error);
    void confirmCustom();

    DatePickerPopup* m_owner = nullptr;
    AnchoredListPopup* m_popup = nullptr;
    QWidget* m_pages = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_presetPage = nullptr;
    QWidget* m_customPage = nullptr;
    QVBoxLayout* m_presetRows = nullptr;
    QVector<DatePickerMenuRow*> m_rows;

    MenuComboBox* m_basisCombo = nullptr;
    QSpinBox* m_intervalSpin = nullptr;
    MenuComboBox* m_freqCombo = nullptr;
    QWidget* m_frequencyRow = nullptr;
    QWidget* m_weekdayRow = nullptr;
    QVector<QToolButton*> m_weekdays;
    QWidget* m_monthModeRow = nullptr;
    MenuComboBox* m_monthModeCombo = nullptr;
    QWidget* m_monthDaysRow = nullptr;
    QVector<QToolButton*> m_monthDays;
    QToolButton* m_lastDay = nullptr;
    QWidget* m_ordinalRow = nullptr;
    MenuComboBox* m_ordinalCombo = nullptr;
    MenuComboBox* m_ordinalWeekdayCombo = nullptr;
    QWidget* m_workdayRow = nullptr;
    MenuComboBox* m_workdayCombo = nullptr;
    QWidget* m_yearModeRow = nullptr;
    MenuComboBox* m_yearModeCombo = nullptr;
    QWidget* m_yearDateRow = nullptr;
    MenuComboBox* m_yearMonthCombo = nullptr;
    QSpinBox* m_yearDaySpin = nullptr;
    QWidget* m_lunarRow = nullptr;
    QLabel* m_lunarAnchor = nullptr;
    QCheckBox* m_lunarLeap = nullptr;
    DateCalendarPanel* m_explicitCalendar = nullptr;
    QLabel* m_error = nullptr;

    RecurrenceSpec m_loadedSpec;
    bool m_loading = false;
    bool m_customEdited = false;
    RecurrenceSpec m_snapshot;
    bool m_snapshotRepeating = false;
    bool m_snapshotValid = false;
};

} // namespace kl

#endif // KYLINLIST_DATEPICKERRECURRENCEEDITOR_H
