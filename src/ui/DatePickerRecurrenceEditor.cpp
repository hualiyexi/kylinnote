#include "DatePickerRecurrenceEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include "../domain/Entity.h"
#include "../domain/LunarCalendar.h"
#include "AnchoredListPopup.h"
#include "DateCalendarPanel.h"
#include "DatePickerControls.h"
#include "DatePickerPopup.h"
#include "MenuComboBox.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
constexpr int kNone = 0;
constexpr int kDaily = 1;
constexpr int kWeekly = 2;
constexpr int kMonthly = 3;
constexpr int kYearly = 4;
constexpr int kWorkdays = 5;
constexpr int kCustom = 6;
constexpr int kLunarMonthly = 4;
constexpr int kLunarYearly = 5;

int comboIndex(QComboBox* combo, int data)
{
    for (int i = 0; i < combo->count(); ++i)
        if (combo->itemData(i).toInt() == data) return i;
    return -1;
}
} // namespace

DatePickerRecurrenceEditor::DatePickerRecurrenceEditor(DatePickerPopup* owner)
    : QObject(owner), m_owner(owner)
{
    m_popup = new AnchoredListPopup(owner);
    m_popup->setObjectName(QStringLiteral("DatePickerRecurrenceLayer"));
    m_popup->setWindowTitle(QStringLiteral("重复"));
    m_popup->setFont(ds::qfont(ds::FontScene::Menu));
    m_popup->content()->setObjectName(QStringLiteral("DatePickerRecurrencePage"));
    auto* layout = new QVBoxLayout(m_popup->content());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_pages = new QWidget(m_popup->content());
    auto* pagesLayout = new QVBoxLayout(m_pages);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    pagesLayout->setSpacing(0);
    m_presetPage = buildPresetPage();
    m_customPage = buildCustomPage();
    pagesLayout->addWidget(m_presetPage);
    pagesLayout->addWidget(m_customPage);
    m_customPage->setVisible(false);
    m_scroll = new QScrollArea(m_popup->content());
    m_scroll->setObjectName(QStringLiteral("DatePickerScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidget(m_pages);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_pages->setAutoFillBackground(false);
    layout->addWidget(m_scroll);
    m_popup->setCardWidth(DatePickerUi::kSubCardWidth);
    connect(m_popup, &QDialog::finished, this, [this](int result) {
        if (m_snapshotValid) {
            if (result != QDialog::Accepted) {
                m_owner->m_schedule.repeating = m_snapshotRepeating;
                m_owner->m_schedule.recurrence = m_snapshot;
            }
            m_snapshotValid = false;
            m_owner->updateEntryCaptions();
        }
    });
}

QWidget* DatePickerRecurrenceEditor::buildPresetPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("DatePickerRecurrenceMenu"));
    m_presetRows = new QVBoxLayout(page);
    m_presetRows->setContentsMargins(4, 4, 4, 4);
    m_presetRows->setSpacing(0);
    return page;
}

QWidget* DatePickerRecurrenceEditor::buildCustomPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("DatePickerCustomRecurrencePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 13, 16, 12);
    layout->setSpacing(8);

    m_basisCombo = DatePickerUi::comboBox(QStringLiteral("重复基准"), page);
    m_basisCombo->addItem(QStringLiteral("按到期日期"));
    m_basisCombo->addItem(QStringLiteral("按完成日期"));
    m_basisCombo->addItem(QStringLiteral("按自选日期"));
    layout->addWidget(m_basisCombo);

    m_frequencyRow = new QWidget(page);
    auto* frequency = new QHBoxLayout(m_frequencyRow);
    frequency->setContentsMargins(0, 0, 0, 0);
    frequency->setSpacing(8);
    m_intervalSpin = DatePickerUi::spinBox(1, 999, QStringLiteral("重复间隔"), m_frequencyRow);
    m_intervalSpin->setPrefix(QStringLiteral("每 "));
    m_freqCombo = DatePickerUi::comboBox(QStringLiteral("重复频率"), m_frequencyRow);
    m_freqCombo->addItem(QStringLiteral("天"), static_cast<int>(RecurrenceSpec::Frequency::Daily));
    m_freqCombo->addItem(QStringLiteral("周"), static_cast<int>(RecurrenceSpec::Frequency::Weekly));
    m_freqCombo->addItem(QStringLiteral("月"), static_cast<int>(RecurrenceSpec::Frequency::Monthly));
    m_freqCombo->addItem(QStringLiteral("年"), static_cast<int>(RecurrenceSpec::Frequency::Yearly));
    m_freqCombo->addItem(QStringLiteral("农历月"), kLunarMonthly);
    m_freqCombo->addItem(QStringLiteral("农历年"), kLunarYearly);
    frequency->addWidget(m_intervalSpin, 1);
    frequency->addWidget(m_freqCombo, 1);
    layout->addWidget(m_frequencyRow);

    m_weekdayRow = new QWidget(page);
    auto* weekdays = new QHBoxLayout(m_weekdayRow);
    weekdays->setContentsMargins(0, 0, 0, 0);
    weekdays->setSpacing(4);
    static const char* const names[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int day = 1; day <= 7; ++day) {
        auto* button = DatePickerUi::toggleButton(QString::fromUtf8(names[day - 1]),
                                                  m_weekdayRow, QSize(22, 22));
        button->setProperty("weekdayValue", day);
        button->setProperty("round", true);
        weekdays->addWidget(button);
        m_weekdays.append(button);
    }
    layout->addWidget(m_weekdayRow);

    m_monthModeRow = new QWidget(page);
    auto* monthMode = new QHBoxLayout(m_monthModeRow);
    monthMode->setContentsMargins(0, 0, 0, 0);
    m_monthModeCombo = DatePickerUi::comboBox(QStringLiteral("每月重复方式"), m_monthModeRow);
    m_monthModeCombo->addItem(QStringLiteral("按日期"));
    m_monthModeCombo->addItem(QStringLiteral("按星期"));
    m_monthModeCombo->addItem(QStringLiteral("按工作日"));
    monthMode->addWidget(m_monthModeCombo, 1);
    layout->addWidget(m_monthModeRow);

    m_monthDaysRow = new QWidget(page);
    auto* monthDays = new QGridLayout(m_monthDaysRow);
    monthDays->setContentsMargins(0, 0, 0, 0);
    monthDays->setHorizontalSpacing(4);
    monthDays->setVerticalSpacing(4);
    for (int day = 1; day <= 31; ++day) {
        auto* button = DatePickerUi::toggleButton(QString::number(day), m_monthDaysRow,
                                                  QSize(22, 22));
        button->setProperty("dayValue", day);
        button->setProperty("round", true);
        monthDays->addWidget(button, (day - 1) / 7, (day - 1) % 7);
        m_monthDays.append(button);
    }
    m_lastDay = DatePickerUi::toggleButton(QStringLiteral("最后一天"), m_monthDaysRow,
                                           QSize(52, 22));
    m_lastDay->setProperty("round", true);
    monthDays->addWidget(m_lastDay, 4, 3, 1, 2);
    layout->addWidget(m_monthDaysRow);

    m_ordinalRow = new QWidget(page);
    auto* ordinal = new QHBoxLayout(m_ordinalRow);
    ordinal->setContentsMargins(0, 0, 0, 0);
    ordinal->setSpacing(8);
    m_ordinalCombo = DatePickerUi::comboBox(QStringLiteral("第几个"), m_ordinalRow);
    m_ordinalCombo->addItem(QStringLiteral("第1个"), 1);
    m_ordinalCombo->addItem(QStringLiteral("第2个"), 2);
    m_ordinalCombo->addItem(QStringLiteral("第3个"), 3);
    m_ordinalCombo->addItem(QStringLiteral("第4个"), 4);
    m_ordinalCombo->addItem(QStringLiteral("第5个"), 5);
    m_ordinalCombo->addItem(QStringLiteral("最后一个"), -1);
    m_ordinalWeekdayCombo = DatePickerUi::comboBox(QStringLiteral("星期几"), m_ordinalRow);
    for (int day = 1; day <= 7; ++day)
        m_ordinalWeekdayCombo->addItem(QStringLiteral("周") + QString::fromUtf8(names[day - 1]), day);
    ordinal->addWidget(m_ordinalCombo, 1);
    ordinal->addWidget(m_ordinalWeekdayCombo, 1);
    layout->addWidget(m_ordinalRow);

    m_workdayRow = new QWidget(page);
    auto* workday = new QHBoxLayout(m_workdayRow);
    workday->setContentsMargins(0, 0, 0, 0);
    m_workdayCombo = DatePickerUi::comboBox(QStringLiteral("每月工作日"), m_workdayRow);
    m_workdayCombo->addItem(QStringLiteral("第一个工作日"), true);
    m_workdayCombo->addItem(QStringLiteral("最后一个工作日"), false);
    workday->addWidget(m_workdayCombo, 1);
    layout->addWidget(m_workdayRow);

    m_yearModeRow = new QWidget(page);
    auto* yearMode = new QHBoxLayout(m_yearModeRow);
    yearMode->setContentsMargins(0, 0, 0, 0);
    m_yearModeCombo = DatePickerUi::comboBox(QStringLiteral("每年重复方式"), m_yearModeRow);
    m_yearModeCombo->addItem(QStringLiteral("按日期"));
    m_yearModeCombo->addItem(QStringLiteral("按星期"));
    yearMode->addWidget(m_yearModeCombo, 1);
    layout->addWidget(m_yearModeRow);

    m_yearDateRow = new QWidget(page);
    auto* yearDate = new QHBoxLayout(m_yearDateRow);
    yearDate->setContentsMargins(0, 0, 0, 0);
    yearDate->setSpacing(8);
    m_yearMonthCombo = DatePickerUi::comboBox(QStringLiteral("月份"), m_yearDateRow);
    for (int month = 1; month <= 12; ++month)
        m_yearMonthCombo->addItem(QStringLiteral("%1月").arg(month), month);
    m_yearDaySpin = DatePickerUi::spinBox(0, 31, QStringLiteral("日期"), m_yearDateRow);
    m_yearDaySpin->setSpecialValueText(QStringLiteral("最后一天"));
    yearDate->addWidget(m_yearMonthCombo, 1);
    yearDate->addWidget(m_yearDaySpin, 1);
    layout->addWidget(m_yearDateRow);

    m_lunarRow = new QWidget(page);
    auto* lunar = new QVBoxLayout(m_lunarRow);
    lunar->setContentsMargins(0, 0, 0, 0);
    lunar->setSpacing(8);
    m_lunarAnchor = new QLabel(m_lunarRow);
    m_lunarAnchor->setObjectName(QStringLiteral("DatePickerHint"));
    m_lunarAnchor->setFont(ds::qfont(ds::FontScene::Caption));
    m_lunarAnchor->setProperty("muted", true);
    m_lunarAnchor->setWordWrap(true);
    lunar->addWidget(m_lunarAnchor);
    m_lunarLeap = new QCheckBox(QStringLiteral("闰月重复"), m_lunarRow);
    m_lunarLeap->setObjectName(QStringLiteral("DatePickerCheck"));
    m_lunarLeap->setAccessibleName(QStringLiteral("闰月重复"));
    lunar->addWidget(m_lunarLeap);
    layout->addWidget(m_lunarRow);

    m_explicitCalendar = new DateCalendarPanel(page);
    m_explicitCalendar->setObjectName(QStringLiteral("DatePickerExplicitDatesCalendar"));
    m_explicitCalendar->setCompact(true);
    m_explicitCalendar->setMultiSelectEnabled(true);
    layout->addWidget(m_explicitCalendar, 0, Qt::AlignHCenter);

    m_error = new QLabel(page);
    m_error->setObjectName(QStringLiteral("DatePickerError"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    layout->addWidget(m_error);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 4, 0, 0);
    footer->setSpacing(12);
    auto* cancel = DatePickerUi::actionButton(QStringLiteral("取消"), "DatePickerSubClear", page);
    auto* done = DatePickerUi::actionButton(QStringLiteral("确定"), "DatePickerSubConfirm", page);
    footer->addWidget(cancel, 1);
    footer->addWidget(done, 1);
    layout->addLayout(footer);
    connect(cancel, &QPushButton::clicked, m_popup, &QDialog::reject);
    connect(done, &QPushButton::clicked, this, [this]() { confirmCustom(); });

    const auto markEdited = [this]() {
        if (!m_loading) m_customEdited = true;
    };
    const auto combos = page->findChildren<QComboBox*>();
    for (QComboBox* combo : combos) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, markEdited](int) {
            if (m_loading) return;
            markEdited();
            m_error->setVisible(false);
            updateFormVisibility();
            updatePageGeometry();
        });
    }
    const auto spins = page->findChildren<QSpinBox*>();
    for (QSpinBox* spin : spins)
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [markEdited](int) { markEdited(); });
    const auto toggles = page->findChildren<QToolButton*>();
    for (QToolButton* toggle : toggles)
        connect(toggle, &QToolButton::toggled, this, [markEdited](bool) { markEdited(); });
    connect(m_lunarLeap, &QCheckBox::toggled, this, [markEdited](bool) { markEdited(); });
    connect(m_explicitCalendar, &DateCalendarPanel::selectedDatesChanged, this,
            [markEdited](const QVector<QDate>&) { markEdited(); });
    return page;
}

RecurrenceSpec DatePickerRecurrenceEditor::presetSpec(int preset) const
{
    RecurrenceSpec spec;
    spec.weekStart = m_owner->m_firstDay;
    if (m_owner->m_schedule.repeating) {
        const auto& current = m_owner->m_schedule.recurrence;
        spec.weekStart = current.weekStart;
        spec.endMode = current.endMode;
        spec.endDate = current.endDate;
        spec.endCount = current.endCount;
    }
    const QDate anchor = m_owner->anchorDate().isValid() ? m_owner->anchorDate()
                                                        : m_owner->today();
    switch (preset) {
    case kDaily:
        spec.frequency = RecurrenceSpec::Frequency::Daily;
        break;
    case kMonthly:
        spec.frequency = RecurrenceSpec::Frequency::Monthly;
        spec.monthDays.append(anchor.day());
        break;
    case kYearly:
        spec.frequency = RecurrenceSpec::Frequency::Yearly;
        spec.month = anchor.month();
        spec.monthDays.append(anchor.day());
        break;
    case kWorkdays:
        spec.frequency = RecurrenceSpec::Frequency::Weekly;
        spec.weekdays = {1, 2, 3, 4, 5};
        break;
    case kWeekly:
    default:
        spec.frequency = RecurrenceSpec::Frequency::Weekly;
        spec.weekdays.append(anchor.dayOfWeek());
        break;
    }
    return spec;
}

void DatePickerRecurrenceEditor::openPresets()
{
    m_presetPage->setVisible(true);
    m_customPage->setVisible(false);
    rebuildPresetRows();
    updatePageGeometry();
    m_popup->setUpperBoundGlobalY(m_owner->calendarGuardY());
    m_popup->popupBelow(m_owner->m_recurrenceEntry);
}

void DatePickerRecurrenceEditor::rebuildPresetRows()
{
    for (DatePickerMenuRow* row : m_rows) {
        m_presetRows->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    m_rows.clear();
    if (m_owner->m_schedule.repeating) addPresetRow(m_presetRows, QStringLiteral("不重复"), kNone);
    addPresetRow(m_presetRows, QStringLiteral("每天"), kDaily);
    const QDate anchor = m_owner->anchorDate().isValid() ? m_owner->anchorDate() : m_owner->today();
    static const char* const names[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    addPresetRow(m_presetRows, QStringLiteral("每周 (%1)").arg(QString::fromUtf8(names[anchor.dayOfWeek() - 1])), kWeekly);
    addPresetRow(m_presetRows, QStringLiteral("每月 (%1日)").arg(anchor.day()), kMonthly);
    addPresetRow(m_presetRows, QStringLiteral("每年 (%1月%2日)").arg(anchor.month()).arg(anchor.day()), kYearly);
    addPresetRow(m_presetRows, QStringLiteral("工作日"), kWorkdays);
    addPresetRow(m_presetRows, QStringLiteral("自定义"), kCustom);
    if (!m_rows.isEmpty()) m_popup->setInitialFocusWidget(m_rows.first());
}

void DatePickerRecurrenceEditor::addPresetRow(QVBoxLayout* layout, const QString& text, int preset)
{
    auto* row = new DatePickerMenuRow(text, layout->parentWidget());
    row->setProperty("preset", preset);
    bool checked = false;
    if (preset == kCustom) {
        checked = m_owner->m_schedule.repeating;
        for (int candidate = kDaily; candidate <= kWorkdays; ++candidate) {
            if (recurrenceRuleText(presetSpec(candidate))
                == recurrenceRuleText(m_owner->m_schedule.recurrence))
                checked = false;
        }
    } else if (preset == kNone) {
        checked = !m_owner->m_schedule.repeating;
    } else if (m_owner->m_schedule.repeating) {
        checked = recurrenceRuleText(presetSpec(preset))
            == recurrenceRuleText(m_owner->m_schedule.recurrence);
    }
    row->setChecked(checked);
    layout->addWidget(row);
    m_rows.append(row);
    connect(row, &QPushButton::clicked, this, [this, preset]() {
        if (preset == kCustom) {
            openCustom();
            return;
        }
        if (preset == kNone) {
            m_owner->m_schedule.repeating = false;
            m_owner->updateEntryCaptions();
        } else {
            m_owner->applyRecurrencePreset(presetSpec(preset));
        }
        m_popup->reject();
    });
}

void DatePickerRecurrenceEditor::openCustom()
{
    const bool wasVisible = m_popup->isVisible();
    m_snapshot = m_owner->m_schedule.recurrence;
    m_snapshotRepeating = m_owner->m_schedule.repeating;
    m_snapshotValid = true;
    loadCustomControls();
    m_presetPage->setVisible(false);
    m_customPage->setVisible(true);
    updatePageGeometry();
    m_popup->setUpperBoundGlobalY(m_owner->calendarGuardY());
    if (wasVisible) m_popup->relayout();
    else m_popup->popupBelow(m_owner->m_recurrenceEntry);
    m_basisCombo->setFocus();
}

void DatePickerRecurrenceEditor::updatePageGeometry()
{
    QWidget* page = !m_customPage->isHidden() ? static_cast<QWidget*>(m_customPage)
                                              : static_cast<QWidget*>(m_presetPage);
    if (QLayout* layout = page->layout()) {
        layout->invalidate();
        layout->activate();
    }
    const bool custom = page == m_customPage;
    const int natural = page->layout() ? page->layout()->sizeHint().height()
                                       : page->sizeHint().height();
    const int preferred = custom ? qMin(natural, 480) : natural;
    m_scroll->setFixedHeight(qMax(1, preferred));
    m_popup->setFlexibleWidget(m_scroll, qMax(1, preferred),
                               custom ? 200 : 2 * DatePickerUi::kRowHeight);
    m_popup->relayout();
}

void DatePickerRecurrenceEditor::loadCustomControls()
{
    m_loading = true;
    const RecurrenceSpec spec = m_owner->m_schedule.repeating
        ? m_owner->m_schedule.recurrence : presetSpec(kWeekly);
    m_loadedSpec = spec;
    m_customEdited = false;
    const QDate anchor = m_owner->anchorDate().isValid() ? m_owner->anchorDate() : m_owner->today();
    const bool explicitDates = spec.kind == RecurrenceSpec::Kind::ExplicitDates;
    m_basisCombo->setCurrentIndex(explicitDates ? 2
        : spec.basis == RecurrenceSpec::Basis::CompletionDate ? 1 : 0);
    m_intervalSpin->setValue(spec.interval);
    const int frequency = spec.kind == RecurrenceSpec::Kind::LunarMonthly ? kLunarMonthly
        : spec.kind == RecurrenceSpec::Kind::LunarYearly ? kLunarYearly
        : static_cast<int>(spec.frequency);
    m_freqCombo->setCurrentIndex(qMax(0, comboIndex(m_freqCombo, frequency)));
    for (QToolButton* button : m_weekdays) {
        const int day = button->property("weekdayValue").toInt();
        button->setChecked(spec.weekdays.isEmpty() ? day == anchor.dayOfWeek()
                                                   : spec.weekdays.contains(day));
    }
    m_monthModeCombo->setCurrentIndex(static_cast<int>(spec.monthMode));
    for (QToolButton* button : m_monthDays) {
        const int day = button->property("dayValue").toInt();
        button->setChecked(spec.monthDays.isEmpty() ? day == anchor.day()
                                                    : spec.monthDays.contains(day));
    }
    m_lastDay->setChecked(spec.monthDays.contains(-1));
    const bool ordinalRule = spec.kind == RecurrenceSpec::Kind::Gregorian
        && ((spec.frequency == RecurrenceSpec::Frequency::Monthly
             && spec.monthMode == RecurrenceSpec::MonthMode::Weekday)
            || (spec.frequency == RecurrenceSpec::Frequency::Yearly
                && spec.yearMode == RecurrenceSpec::YearMode::Weekday));
    m_ordinalCombo->setCurrentIndex(qMax(0, comboIndex(m_ordinalCombo,
        ordinalRule ? spec.ordinal : (anchor.day() - 1) / 7 + 1)));
    m_ordinalWeekdayCombo->setCurrentIndex(qMax(0, comboIndex(m_ordinalWeekdayCombo,
        ordinalRule ? spec.ordinalWeekday : anchor.dayOfWeek())));
    m_workdayCombo->setCurrentIndex(spec.firstWorkday ? 0 : 1);
    m_yearModeCombo->setCurrentIndex(static_cast<int>(spec.yearMode));
    const bool yearly = spec.kind == RecurrenceSpec::Kind::Gregorian
        && spec.frequency == RecurrenceSpec::Frequency::Yearly;
    m_yearMonthCombo->setCurrentIndex(qBound(1, yearly ? spec.month : anchor.month(), 12) - 1);
    m_yearDaySpin->setValue(spec.monthDays.contains(-1) ? 0
        : spec.monthDays.isEmpty() ? anchor.day() : spec.monthDays.first());
    LunarDate lunar;
    if (LunarCalendar::fromSolar(anchor, &lunar)) {
        m_lunarAnchor->setText(QStringLiteral("按农历%1%2月%3日%4重复")
            .arg(lunar.leapMonth ? QStringLiteral("闰") : QString())
            .arg(lunar.month).arg(lunar.day)
            .arg(lunar.lastDay ? QStringLiteral("（月末）") : QString()));
    } else {
        m_lunarAnchor->setText(QStringLiteral("当前日期不在农历支持范围内"));
    }
    m_lunarLeap->setChecked(spec.lunarLeapMonthRepeat);
    m_explicitCalendar->setFirstDay(m_owner->m_firstDay);
    m_explicitCalendar->setToday(m_owner->today());
    m_explicitCalendar->setSelectedDates(explicitDates ? spec.explicitDates : QVector<QDate>{anchor});
    m_explicitCalendar->setVisibleMonth(!spec.explicitDates.isEmpty() ? spec.explicitDates.first() : anchor);
    m_error->clear();
    m_error->setVisible(false);
    m_loading = false;
    updateFormVisibility();
}

void DatePickerRecurrenceEditor::updateFormVisibility()
{
    const bool explicitDates = m_basisCombo->currentIndex() == 2;
    const bool completion = m_basisCombo->currentIndex() == 1;
    auto* model = qobject_cast<QStandardItemModel*>(m_freqCombo->model());
    for (int row = 0; model && row < m_freqCombo->count(); ++row) {
        if (QStandardItem* item = model->item(row))
            item->setEnabled(!completion || m_freqCombo->itemData(row).toInt()
                <= static_cast<int>(RecurrenceSpec::Frequency::Monthly));
    }
    if (completion && m_freqCombo->currentData().toInt()
        > static_cast<int>(RecurrenceSpec::Frequency::Monthly)) {
        const QSignalBlocker blocker(m_freqCombo);
        m_freqCombo->setCurrentIndex(comboIndex(m_freqCombo, static_cast<int>(RecurrenceSpec::Frequency::Daily)));
    }
    const int frequency = m_freqCombo->currentData().toInt();
    const bool lunar = !explicitDates && (frequency == kLunarMonthly || frequency == kLunarYearly);
    if (lunar) {
        const QSignalBlocker blocker(m_intervalSpin);
        m_intervalSpin->setValue(1);
    }
    m_intervalSpin->setEnabled(!lunar);
    const bool fixedDates = !explicitDates && !completion && !lunar;
    const bool monthly = fixedDates && frequency == static_cast<int>(RecurrenceSpec::Frequency::Monthly);
    const bool yearly = fixedDates && frequency == static_cast<int>(RecurrenceSpec::Frequency::Yearly);
    m_frequencyRow->setVisible(!explicitDates);
    m_weekdayRow->setVisible(fixedDates && frequency == static_cast<int>(RecurrenceSpec::Frequency::Weekly));
    m_monthModeRow->setVisible(monthly);
    m_yearModeRow->setVisible(yearly);
    m_yearDateRow->setVisible(yearly);
    m_explicitCalendar->setVisible(explicitDates);
    m_lunarRow->setVisible(lunar);
    const int monthMode = m_monthModeCombo->currentIndex();
    m_monthDaysRow->setVisible(monthly && monthMode == 0);
    m_workdayRow->setVisible(monthly && monthMode == 2);
    const bool yearlyWeekday = yearly && m_yearModeCombo->currentIndex() == 1;
    m_ordinalRow->setVisible((monthly && monthMode == 1) || yearlyWeekday);
    m_yearDaySpin->setVisible(!yearlyWeekday);
    if (monthly || yearly) {
        auto* layout = qobject_cast<QVBoxLayout*>(m_customPage->layout());
        if (layout) {
            layout->removeWidget(m_ordinalRow);
            QWidget* after = monthly ? m_monthModeRow : m_yearDateRow;
            layout->insertWidget(layout->indexOf(after) + 1, m_ordinalRow);
        }
    }
}

bool DatePickerRecurrenceEditor::gather(RecurrenceSpec* spec, QString* error)
{
    if (!spec) return false;
    if (!m_customEdited) {
        if (!isValidRecurrence(m_loadedSpec, error)) return false;
        *spec = m_loadedSpec;
        return true;
    }
    RecurrenceSpec value;
    value.weekStart = m_loadedSpec.weekStart;
    value.endMode = m_owner->m_schedule.recurrence.endMode;
    value.endDate = m_owner->m_schedule.recurrence.endDate;
    value.endCount = m_owner->m_schedule.recurrence.endCount;
    const bool explicitDates = m_basisCombo->currentIndex() == 2;
    const int frequency = m_freqCombo->currentData().toInt();
    value.kind = explicitDates ? RecurrenceSpec::Kind::ExplicitDates
        : frequency == kLunarMonthly ? RecurrenceSpec::Kind::LunarMonthly
        : frequency == kLunarYearly ? RecurrenceSpec::Kind::LunarYearly
        : RecurrenceSpec::Kind::Gregorian;
    value.basis = m_basisCombo->currentIndex() == 1 ? RecurrenceSpec::Basis::CompletionDate
                                                   : RecurrenceSpec::Basis::DueDate;
    if (explicitDates) {
        value.explicitDates = m_explicitCalendar->selectedDates();
        if (value.explicitDates.isEmpty()) {
            *error = QStringLiteral("请至少选择一个重复日期");
            return false;
        }
        if (value.endMode == RecurrenceSpec::EndMode::Count) {
            *error = QStringLiteral("自选日期不支持按次数结束，请先修改结束重复条件");
            return false;
        }
    } else if (value.kind == RecurrenceSpec::Kind::LunarMonthly
               || value.kind == RecurrenceSpec::Kind::LunarYearly) {
        LunarDate anchor;
        if (!LunarCalendar::fromSolar(m_owner->anchorDate(), &anchor, error)) return false;
        value.frequency = frequency == kLunarMonthly ? RecurrenceSpec::Frequency::Monthly
                                                      : RecurrenceSpec::Frequency::Yearly;
        value.lunarLeapMonthRepeat = m_lunarLeap->isChecked();
        if (value.kind == m_loadedSpec.kind) value.month = m_loadedSpec.month;
    } else {
        value.interval = m_intervalSpin->value();
        value.frequency = static_cast<RecurrenceSpec::Frequency>(frequency);
        if (value.basis == RecurrenceSpec::Basis::DueDate) {
            switch (value.frequency) {
            case RecurrenceSpec::Frequency::Daily:
                break;
            case RecurrenceSpec::Frequency::Weekly:
                for (QToolButton* button : m_weekdays)
                    if (button->isChecked()) value.weekdays.append(button->property("weekdayValue").toInt());
                if (value.weekdays.isEmpty()) {
                    *error = QStringLiteral("请至少选择一个星期");
                    return false;
                }
                break;
            case RecurrenceSpec::Frequency::Monthly:
                value.monthMode = static_cast<RecurrenceSpec::MonthMode>(m_monthModeCombo->currentIndex());
                if (value.monthMode == RecurrenceSpec::MonthMode::Days) {
                    if (m_lastDay->isChecked()) value.monthDays.append(-1);
                    for (QToolButton* button : m_monthDays)
                        if (button->isChecked()) value.monthDays.append(button->property("dayValue").toInt());
                    if (value.monthDays.isEmpty()) {
                        *error = QStringLiteral("请至少选择一个每月日期");
                        return false;
                    }
                } else if (value.monthMode == RecurrenceSpec::MonthMode::Weekday) {
                    value.ordinal = m_ordinalCombo->currentData().toInt();
                    value.ordinalWeekday = m_ordinalWeekdayCombo->currentData().toInt();
                } else {
                    value.firstWorkday = m_workdayCombo->currentData().toBool();
                }
                break;
            case RecurrenceSpec::Frequency::Yearly:
                value.yearMode = static_cast<RecurrenceSpec::YearMode>(m_yearModeCombo->currentIndex());
                value.month = m_yearMonthCombo->currentData().toInt();
                if (value.yearMode == RecurrenceSpec::YearMode::Date) {
                    value.monthDays.append(m_yearDaySpin->value() == 0 ? -1 : m_yearDaySpin->value());
                } else {
                    value.ordinal = m_ordinalCombo->currentData().toInt();
                    value.ordinalWeekday = m_ordinalWeekdayCombo->currentData().toInt();
                }
                break;
            }
        }
    }
    if (!isValidRecurrence(value, error)) return false;
    *spec = value;
    return true;
}

void DatePickerRecurrenceEditor::confirmCustom()
{
    RecurrenceSpec spec;
    QString error;
    if (!gather(&spec, &error)) {
        m_error->setText(error);
        m_error->setVisible(true);
        updatePageGeometry();
        return;
    }
    m_owner->m_schedule.repeating = true;
    m_owner->m_schedule.recurrence = spec;
    m_snapshotValid = false;
    m_error->setVisible(false);
    m_owner->clearCardError();
    m_owner->updateEntryCaptions();
    m_popup->accept();
}

} // namespace kl
