#ifndef KYLINLIST_DATEPICKERREMINDEREDITOR_H
#define KYLINLIST_DATEPICKERREMINDEREDITOR_H

#include <QObject>
#include <QStringList>
#include <QVector>

#include "../domain/ScheduleValue.h"

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QSpinBox;
class QWidget;

namespace kl {

class AnchoredListPopup;
class DatePickerMenuRow;
class DatePickerPopup;
class MenuComboBox;
struct ReminderSpec;

// 日期选择器内部的提醒编辑器：根菜单与自定义页共享一个锚定卡片。
// 调度草稿仍由 DatePickerPopup 所有，本类只负责网页式交互和本地最近项。
class DatePickerReminderEditor final : public QObject {
    Q_OBJECT
public:
    explicit DatePickerReminderEditor(DatePickerPopup* owner);

    AnchoredListPopup* popup() const { return m_popup; }
    void open(bool allDay, bool range);
    void rebuildMenu();

private:
    QWidget* buildRootPage();
    QWidget* buildCustomPage();
    QWidget* buildTimedForm();
    QWidget* buildAllDayForm();
    QHBoxLayout* buildFooter(QWidget* parent, bool custom);
    void showCustomPage();
    void addMenuRow(const QString& ruleJson, const QString& text,
                    const QString& secondary = QString(), qint64 legacyMs = 0);
    void refreshMenuChecks();
    bool buildCustomSpec(ReminderSpec* spec, QString* error) const;
    void updateCustomPreview();
    void confirmCustom();
    void recordRecentRules(bool accepted);

    DatePickerPopup* m_owner = nullptr;
    AnchoredListPopup* m_popup = nullptr;
    QWidget* m_pages = nullptr;
    QWidget* m_rootPage = nullptr;
    QWidget* m_customPage = nullptr;
    QScrollArea* m_menuScroll = nullptr;
    QVector<DatePickerMenuRow*> m_rows;
    QStringList m_presetRules;
    QStringList m_pendingRecentRules;

    QWidget* m_customForms = nullptr;
    QWidget* m_timedForm = nullptr;
    QWidget* m_allDayForm = nullptr;
    QSpinBox* m_offsetSpin = nullptr;
    MenuComboBox* m_unitCombo = nullptr;
    MenuComboBox* m_allDayModeCombo = nullptr;
    QSpinBox* m_allDayOffsetSpin = nullptr;
    QLineEdit* m_allDayTimeEdit = nullptr;
    QLabel* m_customSummary = nullptr;
    QLabel* m_customTrigger = nullptr;
    QLabel* m_customError = nullptr;

    bool m_allDay = false;
    bool m_range = false;
};

} // namespace kl

#endif // KYLINLIST_DATEPICKERREMINDEREDITOR_H
