#ifndef KYLINLIST_DATEPICKERPOPUP_H
#define KYLINLIST_DATEPICKERPOPUP_H

#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QToolButton;
class QTime;

namespace kl {

// 明确时刻使用 UTC；纯日期使用 QDate，只有显示与输入时刻才换算应用时区。
// accepted() 使用 QDialog 已有信号，cleared() 表示用户明确点击了“清除”。
// 给定接口中信号与 const getter 同名；在非 const 对象上查询状态需通过 const 引用调用 getter。
class DatePickerPopup : public QDialog {
    Q_OBJECT
public:
    explicit DatePickerPopup(QWidget* parent = nullptr);

    // setDateTime 始终表示明确时刻，包含 00:00；与 setAllDayDate 互斥。
    void setDateTime(const QDateTime& dt);
    void setAllDayDate(const QDate& date);
    void setHasDate(bool has);
    void setReminder(const QDateTime& reminder);
    void setRecurrence(const QString& rule);

    QDateTime dateTime() const;
    QDate allDayDate() const;
    bool hasDate() const;
    bool hasTime() const;
    QDateTime reminder() const;
    QString recurrence() const;
    bool cleared() const;

    static QDate quickDate(int index, const QDate& today);

    void popupBelow(QWidget* anchor);
    void popupAt(const QRect& globalAnchor);

signals:
    void cleared();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    QWidget* buildMainPage(QWidget* parent);
    QWidget* buildTimePage(QWidget* parent);
    QWidget* buildReminderPage(QWidget* parent);
    QWidget* buildRecurrencePage(QWidget* parent);
    void buildCalendarGrid();
    void refreshWeekdayLabels();
    void updateMonthLabel();
    void selectDate(const QDate& date);
    void applyTheme();
    void setRangeMode(bool range);
    void showMainPage();
    void openTimePage();
    void openReminderPage();
    void openRecurrencePage();
    void applyTime(const QTime& time);
    bool applyCustomTime();
    bool applyReminder();
    bool applyCustomRecurrence();
    QDate selectedDate() const;

    QDateTime m_dateTime;
    QDate m_allDayDate;
    QDateTime m_reminder;
    QString m_recurrence = QStringLiteral("none");
    QDate m_visibleMonth;
    int m_firstDay = Qt::Monday;
    bool m_cleared = false;
    bool m_persistentReminder = false;

    QWidget* m_card = nullptr;
    QStackedWidget* m_pages = nullptr;
    QStackedWidget* m_modeStack = nullptr;
    QLabel* m_monthLabel = nullptr;
    QPushButton* m_dateButton = nullptr;
    QPushButton* m_rangeButton = nullptr;
    QPushButton* m_okButton = nullptr;
    QVector<QToolButton*> m_quickButtons;
    QVector<QLabel*> m_weekdayLabels;
    QVector<QToolButton*> m_dayButtons;

    QListWidget* m_timeList = nullptr;
    QLineEdit* m_timeEdit = nullptr;
    QListWidget* m_reminderList = nullptr;
    QLineEdit* m_customReminderEdit = nullptr;
    QCheckBox* m_persistentCheck = nullptr;
    QListWidget* m_recurrenceList = nullptr;
    QLineEdit* m_customRecurrenceEdit = nullptr;
};

} // namespace kl

#endif // KYLINLIST_DATEPICKERPOPUP_H
