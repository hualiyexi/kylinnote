#ifndef KYLINLIST_DATEPICKERPOPUP_H
#define KYLINLIST_DATEPICKERPOPUP_H

#include <QDate>
#include <QDateTime>
#include <QColor>
#include <QDialog>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QVector>

#include "../domain/ScheduleValue.h"

class QCheckBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QToolButton;

namespace kl {

class AnchoredListPopup;
class DateCalendarPanel;
class DatePickerReminderEditor;
class DatePickerRecurrenceEditor;

// 完整调度编辑弹层（Qt Widgets 实现，几何以滴答网页 DOM／计算样式为参照）。
//
// 存储口径：明确时刻一律 UTC；本地显示与输入用 m_schedule.timeZone（实体自带
// 时区，不读全局应用时区）。单时刻写 dueAt；时间段写 startAt+dueAt；全天写
// allDayDate(+allDayEndDate)；hasScheduleRange 区分单日与时间段。
//
// 结构：父卡片按当前模式和行内展开自然调整高度；普通模式内嵌月历，时间段在
// 对应端点行下展开月历或时刻列表。提醒 / 重复使用 AnchoredListPopup，在菜单与
// 独立自定义页之间切换；结束重复在父卡片行下展开。子层只修改父草稿，父层
// accept() 通过校验后才由控制器写库。
//
// 提醒：多次一次性提醒（无持续提醒 UI/状态）。每行保留稳定 UUID，规则以
// ReminderSpec 序列化进 TaskReminder.ruleJson，触发时刻由 resolveReminder 求得。
//
// 重复：RecurrenceSpec 草稿编辑（预置 + 按频率展开的自定义工作区 + 结束条件），
// 自选日期使用月历多选；不提供原始 RRULE 输入、节假日与记忆法。
//
// accept() 校验：开始不得晚于结束（含同日），无效计划/提醒/重复规则以卡片内
// 行内错误提示，绝不静默交换或改写；未选任何日期时 accept 视为清除计划
// （schedule() 同时清空提醒，沿用旧单值口径）。
//
// 兼容：旧单值 getter/setter 是适配层，过渡期保留给现有 TaskDateController；
// 控制器重写后应只使用 setSchedule/schedule。accepted() 用 QDialog 已有信号，
// cleared() 表示用户明确点击“清除”。
class DatePickerPopup : public QDialog {
    Q_OBJECT
    friend class DatePickerReminderEditor;
    friend class DatePickerRecurrenceEditor;
public:
    explicit DatePickerPopup(QWidget* parent = nullptr);

    // —— 权威接口：完整调度草稿 ——
    // 载入时仅有 startAt 的非范围值按“单时刻”处理，输出时写回 dueAt（与旧控制器一致）。
    void setSchedule(const ScheduleValue& schedule);
    ScheduleValue schedule() const;
    void setRecurrenceEnabled(bool enabled);

    // —— 旧单值适配层 ——
    // setDateTime 始终表示明确时刻（含 00:00）；与 setAllDayDate 互斥。
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

    // 校验通过才接受；失败在卡片内显示行内错误并保持打开。
    void accept() override;
    void reject() override;

    void popupBelow(QWidget* anchor);
    void popupAt(const QRect& globalAnchor);

signals:
    void cleared();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class TimeTarget { Single, RangeStart, RangeEnd };
    enum class InlineExpansion { None, Time, RangeCalendar, RecurrenceEnd, ExplicitDates };

    // —— 草稿与换算 ——
    QTimeZone zone() const;
    QDateTime toZoneLocal(const QDateTime& utc) const;
    QDateTime fromZoneLocal(const QDate& date, const QTime& time) const;
    QDateTime endpointFromDraft(const QDateTime& original, const QDate& date,
                                const QTime& time) const;
    QDate today() const;
    QDate anchorDate() const;          // 预设/自定义默认锚点：单日或范围开始日
    bool hasAnyDate() const;
    Entity buildEntity() const;        // 由草稿拼出校验/解析用的实体
    bool resolveDraftReminder(const TaskReminder& row, QDateTime* triggerAt,
                              QString* error) const;
    int findReminderRowByRule(const QString& ruleJson) const;
    void addReminderRule(const QString& ruleJson);
    void removeReminderRule(const QString& ruleJson);
    bool remindersAllResolvable() const;
    QString reminderCaption(const TaskReminder& row) const;
    void switchReminderDraftMode(bool allDay);

    // —— UI 构建 ——
    void buildUi();
    QWidget* buildMainPage(QWidget* parent);
    QWidget* buildDatePage(QWidget* parent);
    QWidget* buildRangePage(QWidget* parent);
    void wireEnterKey(QWidget* scope);
    AnchoredListPopup* ensureReminderPopup();
    AnchoredListPopup* ensureRecurrencePopup();
    QWidget* ensureTimeExpansion();
    DateCalendarPanel* ensureInlineRangeCalendar();
    void closeInlineExpansion(bool restoreFocus = false);
    void closeSubPopups();

    // —— 刷新 ——
    void syncFromDraft();
    void refreshAll();
    void updateEntriesVisibility();
    void updateEntryCaptions();
    void updateRangeRows();
    void updateCardGeometry();
    void applyModeToUi();
    void showCardError(const QString& message);
    void clearCardError();

    // —— 交互 ——
    void selectDate(const QDate& date);
    void selectRangeDate(const QDate& date);
    void setRangeMode(bool range);
    void setAllDayRange(bool allDay);
    void applyTheme();
    void openRangeCalendar(bool end);
    void openTimePageFor(TimeTarget target, QWidget* anchor);
    void openTimePage();
    void openReminderPage();
    void openRecurrencePage();
    void openCustomRecurrencePage();
    void openRecurrenceEndPage();
    void showRecurrenceEndExpansion();
    void applyRecurrencePreset(const RecurrenceSpec& spec);
    void applyTime(const QTime& time);
    bool applyCustomTime();
    int calendarGuardY() const;
    QDate selectedDate() const;

    // —— 草稿状态 ——
    ScheduleValue m_schedule;          // 权威保留 timeZone / repeating / recurrence
    QVector<TaskReminder> m_reminders; // 当前模式提醒草稿（稳定 UUID + ruleJson）
    QVector<TaskReminder> m_timedReminderDraft;
    QVector<TaskReminder> m_allDayReminderDraft;
    QVector<TaskReminder> m_reminderEditSnapshot;
    QStringList m_recentReminderRulesPending;
    bool m_reminderModeAllDay = true;
    bool m_reminderEditSnapshotValid = false;
    bool m_range = false;              // 时间段模式
    bool m_allDayRange = false;        // 时间段全天（隐藏时刻控件但保留草稿）
    QDate m_singleDate;                // 单模式日期；无效 = 未选
    QTime m_singleTime;                // 单模式时刻草稿；无效 = 全天（00:00 为有效值）
    QDate m_rangeStartDate;
    QDate m_rangeEndDate;
    QTime m_rangeStartTime;
    QTime m_rangeEndTime;
    bool m_rangeCalendarEnd = false;   // 当前日历子弹层编辑哪一端
    TimeTarget m_timeTarget = TimeTarget::Single;
    InlineExpansion m_inlineExpansion = InlineExpansion::None;
    int m_firstDay = Qt::Monday;
    bool m_cleared = false;
    bool m_recurrenceEnabled = true;

    // —— 父卡片 ——
    void rebuildShadow(const QColor& color);
    QWidget* m_card = nullptr;
    QScrollArea* m_mainScroll = nullptr;
    QWidget* m_mainPage = nullptr;
    QWidget* m_footer = nullptr;
    QImage m_shadow;
    QColor m_shadowColor;
    QWidget* m_datePage = nullptr;
    QWidget* m_rangePage = nullptr;
    QWidget* m_calendarSection = nullptr;
    QWidget* m_timeEntrySection = nullptr;
    QWidget* m_recurrenceEntrySection = nullptr;
    DateCalendarPanel* m_inlineCalendar = nullptr;
    QPushButton* m_dateButton = nullptr;
    QPushButton* m_rangeButton = nullptr;
    QPushButton* m_okButton = nullptr;
    QLabel* m_errorLabel = nullptr;
    QVector<QToolButton*> m_quickButtons;

    QCheckBox* m_allDayCheck = nullptr;
    QWidget* m_rangeStartRow = nullptr;
    QWidget* m_rangeEndRow = nullptr;
    QPushButton* m_rangeStartDateButton = nullptr;
    QPushButton* m_rangeEndDateButton = nullptr;
    QPushButton* m_rangeStartTimeButton = nullptr;
    QPushButton* m_rangeEndTimeButton = nullptr;

    QPushButton* m_timeEntry = nullptr;
    QPushButton* m_reminderEntry = nullptr;
    QPushButton* m_recurrenceEntry = nullptr;
    QWidget* m_recurrenceEndSection = nullptr;
    QPushButton* m_recurrenceEndEntry = nullptr;
    AnchoredListPopup* m_recurrenceEndPopup = nullptr;
    DateCalendarPanel* m_recurrenceEndCalendar = nullptr;
    QWidget* m_recurrenceEndCountEditor = nullptr;
    QSpinBox* m_recurrenceEndCountSpin = nullptr;

    // —— 子层 ——
    AnchoredListPopup* m_reminderPopup = nullptr;
    DatePickerReminderEditor* m_reminderEditor = nullptr;
    AnchoredListPopup* m_recurrencePopup = nullptr;
    DatePickerRecurrenceEditor* m_recurrenceEditor = nullptr;
    DateCalendarPanel* m_rangeCalendar = nullptr;

    QListWidget* m_timeList = nullptr;
    QLineEdit* m_timeEdit = nullptr;
    QWidget* m_timeExpansion = nullptr;
    QPointer<QWidget> m_inlineAnchor;
    QRect m_popupAnchor;
    bool m_popupPositioned = false;
};

} // namespace kl

#endif // KYLINLIST_DATEPICKERPOPUP_H
