#ifndef KYLINLIST_DATECALENDARPANEL_H
#define KYLINLIST_DATECALENDARPANEL_H

#include <QDate>
#include <QVector>
#include <QWidget>

class QLabel;
class QToolButton;

namespace kl {

// 只负责月历展示与日期激活；调度草稿、范围端点和保存仍由 DatePickerPopup 管理。
// 默认单选：点击只发 dateActivated，选中由外部 setSelectedDate 维护，行为不变。
// 可选多选：setMultiSelectEnabled(true) 后点击日期在内部切换选择（升序去重），
// 发 selectedDatesChanged 的同时继续发 dateActivated；多选沿用 checked/QSS 选中样式。
class DateCalendarPanel : public QWidget {
    Q_OBJECT
public:
    explicit DateCalendarPanel(QWidget* parent = nullptr);

    void setFirstDay(int day);
    int firstDay() const { return m_firstDay; }
    void setToday(const QDate& date);
    void setVisibleMonth(const QDate& month);
    QDate visibleMonth() const { return m_visibleMonth; }
    void setSelectedDate(const QDate& date);
    QDate selectedDate() const { return m_selectedDate; }
    // 多选：过滤无效日期、按日期升序排序去重后作为多选集合。
    // 多选模式下会刷新显示并发 selectedDatesChanged；单选模式下仅记录，
    // 待启用多选时生效（启用时若集合为空则以当前单选日期为初始选择）。
    void setMultiSelectEnabled(bool enabled);
    bool multiSelectEnabled() const { return m_multiSelect; }
    void setSelectedDates(const QVector<QDate>& dates);
    QVector<QDate> selectedDates() const;
    void setCompact(bool compact);

signals:
    void dateActivated(const QDate& date);
    void selectedDatesChanged(const QVector<QDate>& dates);

private:
    void rebuildGrid();
    void refreshWeekdays();
    void polishButtons();
    bool isDateSelected(const QDate& date) const;
    void syncCheckedStates();

    int m_firstDay = Qt::Monday;
    QDate m_today;
    QDate m_visibleMonth;
    QDate m_selectedDate;
    QVector<QDate> m_selectedDates; // 多选集合：均为有效日期、升序、去重
    bool m_multiSelect = false;
    bool m_compact = false;
    QLabel* m_monthLabel = nullptr;
    QVector<QLabel*> m_weekdayLabels;
    QVector<QToolButton*> m_dayButtons;
};

} // namespace kl

#endif // KYLINLIST_DATECALENDARPANEL_H
