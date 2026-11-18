#include "DateCalendarPanel.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace kl {
namespace {

constexpr int kCalendarCell = 30;

void repolish(QWidget* widget)
{
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QVector<QDate> normalizedDates(const QVector<QDate>& dates)
{
    QVector<QDate> result;
    for (const QDate& date : dates) {
        if (date.isValid()) result.append(date);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace

DateCalendarPanel::DateCalendarPanel(QWidget* parent)
    : QWidget(parent)
    , m_today(QDate::currentDate())
    , m_visibleMonth(QDate::currentDate().year(), QDate::currentDate().month(), 1)
{
    setObjectName(QStringLiteral("DatePickerCalendarPanel"));
    auto* panelLayout = new QVBoxLayout(this);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);

    auto* monthRow = new QHBoxLayout;
    monthRow->setContentsMargins(0, 0, 0, 0);
    monthRow->setSpacing(2);
    m_monthLabel = new QLabel(this);
    m_monthLabel->setFixedHeight(28);
    monthRow->addWidget(m_monthLabel);
    monthRow->addStretch();
    auto* previous = new QToolButton(this);
    previous->setObjectName(QStringLiteral("DatePickerNav"));
    previous->setText(QStringLiteral("‹"));
    previous->setToolTip(QStringLiteral("上一月"));
    previous->setAccessibleName(QStringLiteral("上一月"));
    auto* todayButton = new QToolButton(this);
    todayButton->setObjectName(QStringLiteral("DatePickerNav"));
    todayButton->setText(QStringLiteral("○"));
    todayButton->setToolTip(QStringLiteral("今天"));
    todayButton->setAccessibleName(QStringLiteral("今天"));
    auto* next = new QToolButton(this);
    next->setObjectName(QStringLiteral("DatePickerNav"));
    next->setText(QStringLiteral("›"));
    next->setToolTip(QStringLiteral("下一月"));
    next->setAccessibleName(QStringLiteral("下一月"));
    for (QToolButton* button : {previous, todayButton, next}) {
        button->setFixedSize(24, 24);
        monthRow->addWidget(button);
    }

    auto* weekdays = new QGridLayout;
    weekdays->setContentsMargins(0, 0, 0, 0);
    weekdays->setHorizontalSpacing(4);
    weekdays->setVerticalSpacing(0);
    for (int column = 0; column < 7; ++column) {
        auto* label = new QLabel(this);
        label->setProperty("weekday", true);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedSize(kCalendarCell, 20);
        weekdays->addWidget(label, 0, column);
        m_weekdayLabels.append(label);
    }

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 8, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(0);
    for (int index = 0; index < 42; ++index) {
        auto* button = new QToolButton(this);
        button->setObjectName(QStringLiteral("DatePickerDay"));
        button->setCheckable(true);
        button->setFixedSize(kCalendarCell, kCalendarCell);
        grid->addWidget(button, index / 7, index % 7);
        m_dayButtons.append(button);
        connect(button, &QToolButton::clicked, this, [this, button]() {
            const QDate date = button->property("date").toDate();
            if (!date.isValid()) return;
            if (m_multiSelect) {
                QVector<QDate> toggled = m_selectedDates;
                if (toggled.contains(date)) toggled.removeOne(date);
                else toggled.append(date);
                setSelectedDates(toggled); // 排序去重、刷新勾选并发 selectedDatesChanged
            }
            emit dateActivated(date);
        });
    }

    panelLayout->addLayout(monthRow);
    panelLayout->addLayout(weekdays);
    panelLayout->addLayout(grid);

    connect(previous, &QToolButton::clicked, this, [this]() {
        setVisibleMonth(m_visibleMonth.addMonths(-1));
    });
    connect(next, &QToolButton::clicked, this, [this]() {
        setVisibleMonth(m_visibleMonth.addMonths(1));
    });
    connect(todayButton, &QToolButton::clicked, this, [this]() {
        if (!m_today.isValid()) return;
        setVisibleMonth(m_today);
        emit dateActivated(m_today);
    });
    refreshWeekdays();
    rebuildGrid();
}

void DateCalendarPanel::setFirstDay(int day)
{
    if (day < Qt::Monday || day > Qt::Sunday || day == m_firstDay) return;
    m_firstDay = day;
    refreshWeekdays();
    rebuildGrid();
}

void DateCalendarPanel::setToday(const QDate& date)
{
    if (m_today == date) return;
    m_today = date;
    rebuildGrid();
}

void DateCalendarPanel::setVisibleMonth(const QDate& month)
{
    if (!month.isValid()) return;
    const QDate normalized(month.year(), month.month(), 1);
    const bool changed = normalized != m_visibleMonth;
    m_visibleMonth = normalized;
    if (changed) rebuildGrid();
    else polishButtons();
}

void DateCalendarPanel::setSelectedDate(const QDate& date)
{
    if (m_multiSelect) {
        // 多选模式下视为"把选择收拢为这一个日期"（无效日期则清空），保持两个 API 一致。
        m_selectedDate = date;
        setSelectedDates(date.isValid() ? QVector<QDate>{date} : QVector<QDate>());
        return;
    }
    if (m_selectedDate == date) {
        syncCheckedStates();
        return;
    }
    m_selectedDate = date;
    rebuildGrid();
}

void DateCalendarPanel::setMultiSelectEnabled(bool enabled)
{
    if (m_multiSelect == enabled) return;
    m_multiSelect = enabled;
    bool seeded = false;
    if (m_multiSelect && m_selectedDates.isEmpty() && m_selectedDate.isValid()) {
        // 已预置过多选集合则沿用；否则从当前单选日期携带初始选择。
        m_selectedDates.append(m_selectedDate);
        seeded = true;
    }
    syncCheckedStates();
    if (seeded) emit selectedDatesChanged(m_selectedDates);
}

void DateCalendarPanel::setSelectedDates(const QVector<QDate>& dates)
{
    const QVector<QDate> normalized = normalizedDates(dates);
    if (normalized == m_selectedDates) return;
    m_selectedDates = normalized;
    if (!m_multiSelect) return; // 单选模式仅记录，不刷新显示、不发信号
    syncCheckedStates();
    emit selectedDatesChanged(m_selectedDates);
}

QVector<QDate> DateCalendarPanel::selectedDates() const
{
    if (m_multiSelect) return m_selectedDates;
    if (m_selectedDate.isValid()) return QVector<QDate>{m_selectedDate};
    return QVector<QDate>();
}

void DateCalendarPanel::setCompact(bool compact)
{
    if (m_compact == compact) return;
    m_compact = compact;
    m_monthLabel->setFixedHeight(compact ? 22 : 28);
    for (QLabel* label : m_weekdayLabels)
        label->setFixedSize(compact ? 28 : kCalendarCell, 20);
    for (QToolButton* button : m_dayButtons)
        button->setFixedSize(compact ? 28 : kCalendarCell, kCalendarCell);
    const auto navigation = findChildren<QToolButton*>(QStringLiteral("DatePickerNav"));
    for (QToolButton* button : navigation)
        button->setFixedSize(compact ? 20 : 24, compact ? 20 : 24);
    updateGeometry();
}

void DateCalendarPanel::refreshWeekdays()
{
    const QString names[] = {
        QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"), QStringLiteral("四"),
        QStringLiteral("五"), QStringLiteral("六"), QStringLiteral("日")
    };
    for (int column = 0; column < m_weekdayLabels.size(); ++column) {
        const int day = ((m_firstDay - 1 + column) % 7) + 1;
        m_weekdayLabels.at(column)->setText(names[day - 1]);
    }
}

void DateCalendarPanel::rebuildGrid()
{
    if (m_dayButtons.size() != 42 || !m_visibleMonth.isValid()) return;
    m_monthLabel->setText(QStringLiteral("%1年%2月")
        .arg(m_visibleMonth.year()).arg(m_visibleMonth.month()));
    const QDate first(m_visibleMonth.year(), m_visibleMonth.month(), 1);
    const int offset = (7 + first.dayOfWeek() - m_firstDay) % 7;
    const QDate start = first.addDays(-offset);
    for (int index = 0; index < 42; ++index) {
        const QDate date = start.addDays(index);
        QToolButton* button = m_dayButtons.at(index);
        button->setText(QString::number(date.day()));
        button->setProperty("date", date);
        button->setProperty("outsideMonth", date.month() != m_visibleMonth.month());
        button->setProperty("today", date == m_today);
        button->setChecked(isDateSelected(date));
        button->setProperty("inRange", false);
        button->setToolTip(date.toString(QStringLiteral("yyyy年M月d日")));
        button->setAccessibleName(date.toString(QStringLiteral("yyyy年M月d日 dddd")));
        repolish(button);
    }
}

void DateCalendarPanel::polishButtons()
{
    for (QToolButton* button : m_dayButtons) repolish(button);
}

bool DateCalendarPanel::isDateSelected(const QDate& date) const
{
    return m_multiSelect ? m_selectedDates.contains(date) : date == m_selectedDate;
}

void DateCalendarPanel::syncCheckedStates()
{
    // 只同步 checked 伪状态（无需 repolish），月份切换/模式切换后选择不丢失。
    for (QToolButton* button : m_dayButtons) {
        const QDate date = button->property("date").toDate();
        button->setChecked(isDateSelected(date));
    }
}

} // namespace kl
