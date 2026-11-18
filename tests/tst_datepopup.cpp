#include <cstdio>
#include <climits>
#include <algorithm>
#include <functional>
#include <memory>

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <QWheelEvent>

#include "Common.h"
#include "app/Settings.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/RecurrenceRule.h"
#include "domain/ReminderRule.h"
#include "platform/SystemShadow.h"
#include "ui/AnchoredListPopup.h"
#include "ui/AppStyle.h"
#include "ui/DateCalendarPanel.h"
#include "ui/DatePickerControls.h"
#include "ui/DatePickerPopup.h"
#include "ui/TaskDateController.h"
#include "ui/design/ThemeManager.h"

using namespace kl;

namespace {

// 入口按钮的 accessibleName 固定为 时间/提醒/重复/结束重复；正文摘要写在 text()。
QPushButton* findEntry(DatePickerPopup* picker, const QString& name)
{
    if (!picker) return nullptr;
    for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
        if (button->accessibleName() == name) return button;
    return nullptr;
}

QListWidget* listOf(QWidget* host)
{
    return host ? host->findChild<QListWidget*>(QStringLiteral("DatePickerList")) : nullptr;
}

// 通过入口打开锚定子层（提醒/重复/结束重复共用 AnchoredListPopup 契约）。
// 只返回找到的可见子层，断言留给调用方（本函数非 void，不能用 QTRY 宏）。
AnchoredListPopup* openLayer(DatePickerPopup* picker, const QString& entryName,
                             const QString& layerName)
{
    auto* entry = findEntry(picker, entryName);
    if (!entry || !entry->isVisible()) return nullptr;
    if (QGuiApplication::platformName() == QStringLiteral("windows")
        && !QTest::qWaitForWindowActive(picker)) return nullptr;
    for (QWidget* parent = entry->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* area = qobject_cast<QScrollArea*>(parent)) {
            area->ensureWidgetVisible(entry, 0, 4);
            break;
        }
    }
    QApplication::processEvents();
    QTest::mouseClick(entry, Qt::LeftButton);
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    for (auto* candidate : picker->findChildren<AnchoredListPopup*>(layerName))
        if (candidate->isVisible()) return candidate;
    return nullptr;
}

// 内嵌时间展开区：点击入口（单日=时间入口，范围=开始/结束时间按钮）后出现在父卡片内。
QWidget* openTimeExpansion(DatePickerPopup* picker, QPushButton* anchor)
{
    if (!picker || !anchor || !anchor->isVisible()) return nullptr;
    QTest::mouseClick(anchor, Qt::LeftButton);
    QApplication::processEvents();
    auto* expansion = picker->findChild<QWidget*>(QStringLiteral("DatePickerTimeExpansion"));
    return (expansion && expansion->isVisible()) ? expansion : nullptr;
}

DateCalendarPanel* openRangeInlineCalendar(DatePickerPopup* picker, bool end)
{
    auto* button = picker ? picker->findChild<QPushButton*>(end
        ? QStringLiteral("结束日期") : QStringLiteral("开始日期")) : nullptr;
    for (auto* candidate : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
        if (candidate->accessibleName() == (end ? QStringLiteral("结束日期")
                                                : QStringLiteral("开始日期"))) button = candidate;
    if (!button || !button->isVisible()) return nullptr;
    QTest::mouseClick(button, Qt::LeftButton);
    QApplication::processEvents();
    auto* calendar = picker->findChild<DateCalendarPanel*>(
        QStringLiteral("DatePickerRangeCalendarInline"));
    return (calendar && calendar->isVisible()) ? calendar : nullptr;
}

QRect globalRect(const QWidget* widget)
{
    return QRect(widget->mapToGlobal(QPoint()), widget->size());
}

DateCalendarPanel* inlineCalendar(DatePickerPopup* picker)
{
    if (!picker) return nullptr;
    auto* card = picker->findChild<QWidget*>(QStringLiteral("DatePickerCard"));
    return card ? card->findChild<DateCalendarPanel*>(QStringLiteral("DatePickerCalendarPanel"))
                : nullptr;
}

int monthGridTopGlobal(DatePickerPopup* picker)
{
    int top = INT_MAX;
    auto* calendar = inlineCalendar(picker);
    if (!calendar || !calendar->isVisible()) return top;
    for (auto* day : calendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay"))) {
        if (day->isVisible()) top = qMin(top, day->mapToGlobal(QPoint()).y());
    }
    return top;
}

QToolButton* dayButton(DateCalendarPanel* calendar, const QDate& date)
{
    if (!calendar) return nullptr;
    for (auto* day : calendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay")))
        if (day->property("date").toDate() == date) return day;
    return nullptr;
}

QToolButton* dayButton(DatePickerPopup* picker, const QDate& date)
{
    return dayButton(inlineCalendar(picker), date);
}

QToolButton* quickButton(DatePickerPopup* picker, int index)
{
    for (auto* button : picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
        if (button->property("quickIndex").toInt() == index) return button;
    return nullptr;
}

QPushButton* segmentButton(DatePickerPopup* picker, const QString& text)
{
    for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerSegment")))
        if (button->text() == text) return button;
    return nullptr;
}

QPushButton* rangeTimeButton(DatePickerPopup* picker, const QString& accessibleName)
{
    for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerRangeTime")))
        if (button->accessibleName() == accessibleName) return button;
    return nullptr;
}

QPushButton* visibleButtonWithText(QWidget* root, const QString& text)
{
    if (!root) return nullptr;
    for (auto* button : root->findChildren<QPushButton*>())
        if (button->isVisible() && button->text() == text) return button;
    return nullptr;
}

// —— 菜单行（DatePickerMenuRow）：34px、整行可点、选中态右侧对勾 ——

QList<DatePickerMenuRow*> menuRowsOf(QWidget* root)
{
    QList<DatePickerMenuRow*> rows = root ? root->findChildren<DatePickerMenuRow*>()
                                          : QList<DatePickerMenuRow*>();
    std::sort(rows.begin(), rows.end(), [](DatePickerMenuRow* a, DatePickerMenuRow* b) {
        return a->mapToGlobal(QPoint()).y() < b->mapToGlobal(QPoint()).y();
    });
    return rows;
}

QStringList menuRowTexts(QWidget* root)
{
    QStringList texts;
    for (auto* row : menuRowsOf(root)) texts << row->text();
    return texts;
}

DatePickerMenuRow* menuRowByText(QWidget* root, const QString& text)
{
    DatePickerMenuRow* found = nullptr;
    for (auto* row : menuRowsOf(root))
        if (row->text() == text) found = row;
    return found;
}

void clickMenuRowByText(QWidget* root, const QString& text)
{
    auto* row = menuRowByText(root, text);
    QVERIFY2(row, qPrintable(QStringLiteral("菜单行不存在：%1").arg(text)));
    for (QWidget* parent = row->parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* area = qobject_cast<QScrollArea*>(parent)) {
            area->ensureWidgetVisible(row);
            QApplication::processEvents();
            break;
        }
    }
    QTest::mouseClick(row, Qt::LeftButton);
}

// 在当前 DPR 下检查右侧 Primary 墨迹，背景或悬浮色不能冒充选中对勾。
int menuRowCheckInk(DatePickerMenuRow* row)
{
    if (!row) return -1;
    const qreal dpr = row->devicePixelRatioF();
    QImage image(QSize(qRound(row->width() * dpr), qRound(row->height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    row->render(&image);
    const int x0 = qMax(0, qRound((row->width() - 34) * dpr));
    const int x1 = qMin(image.width(), qRound((row->width() - 10) * dpr));
    const QColor primary = ThemeManager::instance().color(ds::ColorToken::Primary);
    int ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = x0; x < x1; ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 80 && qAbs(pixel.red() - primary.red()) < 32
                && qAbs(pixel.green() - primary.green()) < 32
                && qAbs(pixel.blue() - primary.blue()) < 32) ++ink;
        }
    }
    return ink;
}

DatePickerMenuRow* endModeRow(AnchoredListPopup* layer, int mode)
{
    for (auto* row : menuRowsOf(layer))
        if (row->property("endMode").toInt() == mode) return row;
    return nullptr;
}

QComboBox* comboByAccessible(QWidget* root, const QString& name)
{
    for (auto* combo : root->findChildren<QComboBox*>())
        if (combo->accessibleName() == name) return combo;
    return nullptr;
}

QSpinBox* spinByAccessible(QWidget* root, const QString& name)
{
    for (auto* spin : root->findChildren<QSpinBox*>())
        if (spin->accessibleName() == name) return spin;
    return nullptr;
}

QLineEdit* editByAccessible(QWidget* root, const QString& name)
{
    for (auto* edit : root->findChildren<QLineEdit*>())
        if (edit->accessibleName() == name) return edit;
    return nullptr;
}

QToolButton* weekdayToggle(QWidget* root, int value)
{
    for (auto* toggle : root->findChildren<QToolButton*>(QStringLiteral("DatePickerToggle")))
        if (toggle->property("weekdayValue").toInt() == value) return toggle;
    return nullptr;
}

QToolButton* monthDayToggle(QWidget* root, int value)
{
    for (auto* toggle : root->findChildren<QToolButton*>(QStringLiteral("DatePickerToggle")))
        if (toggle->property("dayValue").toInt() == value) return toggle;
    return nullptr;
}

void setAllMonthDayToggles(QWidget* root, bool checked)
{
    for (auto* toggle : root->findChildren<QToolButton*>(QStringLiteral("DatePickerToggle")))
        if (toggle->property("dayValue").isValid()) toggle->setChecked(checked);
}

QLabel* visibleErrorIn(QWidget* root, const QString& contained)
{
    for (auto* label : root->findChildren<QLabel*>(QStringLiteral("DatePickerError")))
        if (label->isVisible() && label->text().contains(contained)) return label;
    return nullptr;
}

QColor renderedPixel(QWidget* widget, const QPoint& point)
{
    QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget->render(&image);
    return image.pixelColor(point);
}

ScheduleValue singleAllDay(const QDate& date)
{
    ScheduleValue value;
    value.allDayDate = date;
    return value;
}

ScheduleValue singleTimed(const QDate& date, const QTime& time)
{
    ScheduleValue value;
    value.dueAt = fromAppTime(QDateTime(date, time));
    return value;
}

ScheduleValue timedRange(const QDate& startDate, const QTime& startTime,
                         const QDate& endDate, const QTime& endTime)
{
    ScheduleValue value;
    value.startAt = fromAppTime(QDateTime(startDate, startTime));
    value.dueAt = fromAppTime(QDateTime(endDate, endTime));
    value.hasScheduleRange = true;
    return value;
}

ScheduleValue allDayRange(const QDate& startDate, const QDate& endDate)
{
    ScheduleValue value;
    value.allDayDate = startDate;
    value.allDayEndDate = endDate;
    value.hasScheduleRange = true;
    return value;
}

ReminderSpec relativeReminder(ReminderSpec::Unit unit, int offset,
                              ReminderSpec::Anchor anchor = ReminderSpec::Anchor::Schedule)
{
    ReminderSpec spec;
    spec.anchor = anchor;
    spec.unit = unit;
    spec.offset = offset;
    return spec;
}

void clearRecentReminderKeys()
{
    // QSettings 已在 init() 指向本轮隔离目录；src 侧最近项接口每次新建
    // QSettings，会读到同一隔离目录。逐条测试清键，避免最近项互相污染。
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
    settings.remove(QStringLiteral("datePicker/recentAllDayReminders"));
    settings.remove(QStringLiteral("datePicker/recentTimedReminders"));
    settings.sync();
}

// 网页实测：子层宽 252、菜单行高 34、距入口 4px。
constexpr int kExpectCardWidth = 252;
constexpr int kExpectRowHeight = 34;
constexpr int kExpectAnchorGap = 4;
constexpr int kPickerShadowMargin = 24;
constexpr int kPickerCardWidth = 260;
constexpr int kCardFooterHeight = 62;
constexpr int kOrdinaryCardHeight = 539;
constexpr int kRangeCardHeight = 315;

} // namespace

// 日期弹层专属测试：
//  - setSchedule/schedule 权威接口：单日全天 / 单时刻（含 00:00）/ 同日时间段 /
//    跨夜时间段 / 跨日时间段 / 全天区间；
//  - 四个快捷按钮保留既有时刻（或保持全天）并立即 accept 关闭；普通月历点击只是草稿；
//  - 时间选择是父卡片内嵌 DatePickerTimeExpansion（48 行半小时表 + 精确分钟输入），
//    Escape 只收内嵌层并把焦点还给入口；旧独立时间子层与 DatePickerNoTime 已移除；
//  - 时间段两端日期用内嵌 DatePickerRangeCalendarInline（插在开始/结束行之间）；
//    结束早于开始：accept 被阻止、卡片内行内报错、弹层保持打开；
//  - 提醒：DatePickerMenuRow 多选（34px、右侧对勾）、取消/确定回滚与提交、
//    自定义页（时刻相对偏移 / 全天按天按周+本地时刻）、最近自定义提醒只在父卡片
//    最终 accept 才写 Settings（子层 accept 仅入父草稿；父 reject/清除不落历史；
//    预设不进最近；上限 3 条最新在前；全天/定时分键），稳定 UUID，无持续提醒 UI；
//  - 重复：预设 + 类型化自定义（公历日/周/月/年、月末、按完成日期限制、农历月/年、
//    自选日期多选日历）、未编辑直接确定原样保留 spec、取消回滚；
//    结束重复 DatePickerRecurrenceEndLayer + EndCalendar/EndCount，切模式清 endDate，
//    自选日期明确拒绝 Count；
//  - 父 accept 先收起未确认子层再校验最终父草稿；入口互斥（closeSubPopups）；
//  - TaskDateController：允许笔记、原子保存与基线冲突保护。
// offscreen 下运行；锚定按屏幕 availableGeometry 运行时计算，不假设屏幕尺寸。
class DatePopupTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        m_settingsDir.reset(new QTemporaryDir);
        QVERIFY(m_settingsDir->isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDir->path());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                           QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        settings.sync();
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, m_dir->path() + QStringLiteral("/settings"));
        clearRecentReminderKeys();
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/kylinlist.db"), &error),
                 qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY(m_store->load(&error));
    }
    void cleanup() {
        m_store.reset();
        Database::instance().close();
        m_dir.reset();
    }

    void popupAtTop(DatePickerPopup& picker) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        picker.popupAt(QRect(QPoint(avail.left() + 60, avail.top() + 40), QSize(80, 28)));
        QTRY_VERIFY(picker.isVisible());
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            QVERIFY(QTest::qWaitForWindowExposed(&picker));
            QVERIFY(QTest::qWaitForWindowActive(&picker));
        }
    }

    // ------------------------------------------------------------------
    // setSchedule / schedule：权威调度草稿的载入与回写
    // ------------------------------------------------------------------
    void scheduleRoundTripSingleAllDay() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 15)));
        popupAtTop(picker);
        QVERIFY(picker.hasDate());
        QVERIFY(!picker.hasTime());
        QCOMPARE(picker.allDayDate(), QDate(2026, 11, 15));
        const ScheduleValue out = picker.schedule();
        QCOMPARE(out.allDayDate, QDate(2026, 11, 15));
        QVERIFY(!out.allDayEndDate.isValid());
        QVERIFY(!out.startAt.isValid());
        QVERIFY(!out.dueAt.isValid());
        QVERIFY(!out.hasScheduleRange);
        picker.reject();
    }

    void scheduleRoundTripSingleTimedIncludingMidnight() {
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(QDate(2026, 11, 15), QTime(0, 0)));
            popupAtTop(picker);
            // 00:00 是有效明确时刻，绝不能被当成“全天”。
            QVERIFY(picker.hasTime());
            QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(0, 0));
            const ScheduleValue out = picker.schedule();
            QVERIFY(!out.allDayDate.isValid());
            QVERIFY(out.dueAt.isValid());
            QVERIFY(!out.startAt.isValid());
            QVERIFY(!out.hasScheduleRange);
            QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 15));
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(0, 0));
            picker.reject();
        }
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(QDate(2026, 11, 15), QTime(23, 30)));
            const ScheduleValue out = picker.schedule();
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(23, 30));
            QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 15));
            QVERIFY(picker.hasTime());
            picker.reject();
        }
    }

    void scheduleRoundTripDstFoldEndpoints() {
        const QTimeZone zone("America/New_York");
        QVERIFY(zone.isValid());
        DatePickerPopup picker;
        ScheduleValue single;
        single.timeZone = QStringLiteral("America/New_York");
        // 2026-11-01 01:30 第一次出现（EDT）；无编辑往返不得跳到第二次（EST）。
        single.dueAt = QDateTime(QDate(2026, 11, 1), QTime(5, 30), Qt::UTC);
        QCOMPARE(single.dueAt.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                 QStringLiteral("2026-11-01 01:30"));
        picker.setSchedule(single);
        QCOMPARE(picker.schedule().dueAt, single.dueAt);
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));

        DatePickerPopup rangePicker;
        ScheduleValue range;
        range.timeZone = single.timeZone;
        range.hasScheduleRange = true;
        // 回拨后墙上结束时间更早，但绝对时间仍晚于开始，是合法范围。
        range.startAt = single.dueAt;
        range.dueAt = QDateTime(QDate(2026, 11, 1), QTime(6, 15), Qt::UTC);
        QCOMPARE(range.dueAt.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                 QStringLiteral("2026-11-01 01:15"));
        rangePicker.setSchedule(range);
        const ScheduleValue out = rangePicker.schedule();
        QCOMPARE(out.startAt, range.startAt);
        QCOMPARE(out.dueAt, range.dueAt);
        rangePicker.accept();
        QCOMPARE(rangePicker.result(), static_cast<int>(QDialog::Accepted));
    }

    void scheduleRoundTripStartOnlyBecomesDue() {
        // 旧库仅有 startAt 的非范围值按“单时刻”载入，输出写回 dueAt。
        ScheduleValue value;
        value.startAt = fromAppTime(QDateTime(QDate(2026, 12, 3), QTime(8, 15)));
        DatePickerPopup picker;
        picker.setSchedule(value);
        QVERIFY(picker.hasTime());
        const ScheduleValue out = picker.schedule();
        QVERIFY(!out.startAt.isValid());
        QVERIFY(out.dueAt.isValid());
        QCOMPARE(toAppTime(out.dueAt).time(), QTime(8, 15));
        QVERIFY(!out.hasScheduleRange);
        picker.reject();
    }

    void scheduleRoundTripRanges() {
        // 同日时间段（不同时刻）。
        {
            DatePickerPopup picker;
            picker.setSchedule(timedRange(QDate(2026, 11, 15), QTime(9, 0),
                                          QDate(2026, 11, 15), QTime(10, 30)));
            popupAtTop(picker);
            const ScheduleValue out = picker.schedule();
            QVERIFY(out.hasScheduleRange);
            QVERIFY(out.startAt.isValid() && out.dueAt.isValid());
            QVERIFY(!out.allDayDate.isValid());
            QCOMPARE(toAppTime(out.startAt).date(), QDate(2026, 11, 15));
            QCOMPARE(toAppTime(out.startAt).time(), QTime(9, 0));
            QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 15));
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(10, 30));
            picker.reject();
        }
        // 跨夜时间段（开始 22:00，结束次日 01:00）。
        {
            DatePickerPopup picker;
            picker.setSchedule(timedRange(QDate(2026, 11, 15), QTime(22, 0),
                                          QDate(2026, 11, 16), QTime(1, 0)));
            const ScheduleValue out = picker.schedule();
            QVERIFY(out.hasScheduleRange);
            QCOMPARE(toAppTime(out.startAt).date(), QDate(2026, 11, 15));
            QCOMPARE(toAppTime(out.startAt).time(), QTime(22, 0));
            QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 16));
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(1, 0));
            picker.reject();
        }
        // 跨日时间段。
        {
            DatePickerPopup picker;
            picker.setSchedule(timedRange(QDate(2026, 11, 10), QTime(9, 0),
                                          QDate(2026, 11, 18), QTime(18, 0)));
            const ScheduleValue out = picker.schedule();
            QVERIFY(out.hasScheduleRange);
            QCOMPARE(toAppTime(out.startAt).date(), QDate(2026, 11, 10));
            QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 18));
            picker.reject();
        }
        // 全天区间（保留包含结束日；不伪造 00:00 时刻）。
        {
            DatePickerPopup picker;
            picker.setSchedule(allDayRange(QDate(2026, 11, 15), QDate(2026, 11, 17)));
            popupAtTop(picker);
            const ScheduleValue out = picker.schedule();
            QVERIFY(out.hasScheduleRange);
            QCOMPARE(out.allDayDate, QDate(2026, 11, 15));
            QCOMPARE(out.allDayEndDate, QDate(2026, 11, 17));
            QVERIFY(!out.startAt.isValid() && !out.dueAt.isValid());
            auto* allDay = picker.findChild<QCheckBox*>(QStringLiteral("DatePickerAllDayCheck"));
            QVERIFY(allDay && allDay->isVisible() && allDay->isChecked());
            picker.reject();
        }
        // 同日全天区间与单日全天的区别只由 hasScheduleRange 表达。
        {
            DatePickerPopup picker;
            picker.setSchedule(allDayRange(QDate(2026, 11, 15), QDate(2026, 11, 15)));
            const ScheduleValue out = picker.schedule();
            QVERIFY(out.hasScheduleRange);
            QCOMPARE(out.allDayDate, QDate(2026, 11, 15));
            QCOMPARE(out.allDayEndDate, QDate(2026, 11, 15));
            picker.reject();
        }
    }

    // ------------------------------------------------------------------
    // 最近自定义提醒：父卡片最终 accept 才写 Settings
    // ------------------------------------------------------------------
    void reminderRecentsOnlyWrittenOnParentAccept() {
        QVERIFY(Settings::instance().recentReminderRules(false).isEmpty());
        QVERIFY(Settings::instance().recentReminderRules(true).isEmpty());
        const QDate day(2026, 11, 20);

        // 子层“确定”只进父草稿（含 pending），父层 reject 不落历史；预设不进最近。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(day, QTime(9, 0)));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            auto* spin = spinByAccessible(layer, QStringLiteral("提醒提前量"));
            auto* unit = comboByAccessible(layer, QStringLiteral("提醒单位"));
            QVERIFY(spin && unit);
            spin->setValue(40);
            unit->setCurrentIndex(0);  // 分钟
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            QCOMPARE(findEntry(&picker, QStringLiteral("提醒"))->text(),
                     QStringLiteral("提前40分钟"));
            QCOMPARE(picker.schedule().reminders.size(), 1);
            QVERIFY(Settings::instance().recentReminderRules(false).isEmpty());

            // 预设行勾选后子层确定：入父草稿，仍不写最近。
            layer = openLayer(&picker, QStringLiteral("提醒"),
                              QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("提前5分钟"));
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            QCOMPARE(picker.schedule().reminders.size(), 2);
            picker.reject();
            QVERIFY(Settings::instance().recentReminderRules(false).isEmpty());
        }

        // 父层最终 accept 才写入；预设规则绝不入列。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(day, QTime(9, 0)));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            spinByAccessible(layer, QStringLiteral("提醒提前量"))->setValue(40);
            comboByAccessible(layer, QStringLiteral("提醒单位"))->setCurrentIndex(0);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            picker.accept();
            QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
            const QStringList timed = Settings::instance().recentReminderRules(false);
            QCOMPARE(timed.size(), 1);
            ReminderSpec spec;
            QVERIFY(reminderFromRuleText(timed.first(), &spec));
            QCOMPARE(spec.unit, ReminderSpec::Unit::Minutes);
            QCOMPARE(spec.offset, 40);
            QVERIFY(!timed.contains(reminderRuleText(
                relativeReminder(ReminderSpec::Unit::Minutes, 5))));
        }

        // “清除”路径同样不落历史。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(day, QTime(9, 0)));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            spinByAccessible(layer, QStringLiteral("提醒提前量"))->setValue(25);
            comboByAccessible(layer, QStringLiteral("提醒单位"))->setCurrentIndex(0);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            QTest::mouseClick(picker.findChild<QPushButton*>(QStringLiteral("DatePickerClear")),
                              Qt::LeftButton);
            QTRY_VERIFY(!picker.isVisible());
            const QStringList timed = Settings::instance().recentReminderRules(false);
            QCOMPARE(timed.size(), 1);
            ReminderSpec spec;
            QVERIFY(reminderFromRuleText(timed.first(), &spec));
            QCOMPARE(spec.offset, 40);
        }

        // 上限 3 条、最新在前：继续叠加 25 / 90 / 10 分钟。
        auto acceptCustom = [&](int minutes) {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(day, QTime(9, 0)));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            spinByAccessible(layer, QStringLiteral("提醒提前量"))->setValue(minutes);
            comboByAccessible(layer, QStringLiteral("提醒单位"))->setCurrentIndex(0);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            picker.accept();
            QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        };
        acceptCustom(25);
        acceptCustom(90);
        acceptCustom(10);
        {
            const QStringList timed = Settings::instance().recentReminderRules(false);
            QCOMPARE(timed.size(), 3);
            QList<int> offsets;
            ReminderSpec spec;
            for (const QString& rule : timed) {
                QVERIFY(reminderFromRuleText(rule, &spec));
                offsets << spec.offset;
            }
            QCOMPARE(offsets, QList<int>({10, 90, 25}));  // 40 分钟被挤出上限
        }

        // 菜单回显：最近行在最前（新→旧），其后预设，最后“自定义”；预设不重复出现。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(day, QTime(9, 0)));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            const QStringList texts = menuRowTexts(layer);
            QCOMPARE(texts.size(), 10);
            QCOMPARE(texts.mid(0, 3),
                     QStringList({QStringLiteral("提前10分钟"), QStringLiteral("提前90分钟"),
                                  QStringLiteral("提前25分钟")}));
            QCOMPARE(texts.mid(3),
                     QStringList({QStringLiteral("准时"), QStringLiteral("提前5分钟"),
                                  QStringLiteral("提前15分钟"), QStringLiteral("提前30分钟"),
                                  QStringLiteral("提前1小时"), QStringLiteral("提前1天"),
                                  QStringLiteral("自定义")}));
            QTest::keyClick(layer, Qt::Key_Escape);
            QTRY_VERIFY(!layer->isVisible());
            picker.reject();
        }

        // 全天最近独立成键，不影响定时最近。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleAllDay(day));
            popupAtTop(picker);
            auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                    QStringLiteral("DatePickerReminderLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            QVERIFY(layer->findChild<QWidget*>(
                        QStringLiteral("DatePickerReminderAllDayCustom"))->isVisible());
            QVERIFY(!layer->findChild<QWidget*>(
                         QStringLiteral("DatePickerReminderTimedCustom"))->isVisible());
            comboByAccessible(layer, QStringLiteral("全天提醒提前方式"))->setCurrentIndex(0);
            spinByAccessible(layer, QStringLiteral("提醒提前天数"))->setValue(2);
            editByAccessible(layer, QStringLiteral("提醒本地时间"))
                ->setText(QStringLiteral("08:30"));
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
            QCOMPARE(findEntry(&picker, QStringLiteral("提醒"))->text(),
                     QStringLiteral("提前2天(08:30)"));
            picker.accept();
            const QStringList allDay = Settings::instance().recentReminderRules(true);
            QCOMPARE(allDay.size(), 1);
            ReminderSpec spec;
            QVERIFY(reminderFromRuleText(allDay.first(), &spec));
            QVERIFY(spec.allDay);
            QCOMPARE(spec.dayOffset, 2);
            QCOMPARE(spec.localTime, QTime(8, 30));
            QCOMPARE(Settings::instance().recentReminderRules(false).size(), 3);
        }
    }

    // ------------------------------------------------------------------
    // 快捷按钮：立即接受；月历点击：仅草稿
    // ------------------------------------------------------------------
    void quickButtonsPreserveTimeOrAllDayAndAcceptImmediately() {
        const QDate today = toAppTime(appNow()).date();
        const QDate expected[] = {today, today.addDays(1), today.addDays(7), today.addMonths(1)};
        for (int index = 0; index < 4; ++index) {
            DatePickerPopup picker;
            picker.setSchedule(singleAllDay(today.addDays(-2)));
            popupAtTop(picker);
            QSignalSpy accepted(&picker, &QDialog::accepted);
            auto* quick = quickButton(&picker, index);
            QVERIFY(quick);
            QTest::mouseClick(quick, Qt::LeftButton);
            QCOMPARE(accepted.count(), 1);          // 立即接受
            QVERIFY(!picker.isVisible());           // 立即关闭
            QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
            const ScheduleValue out = picker.schedule();
            QVERIFY(!out.dueAt.isValid());          // 无时刻 → 保持全天
            QCOMPARE(out.allDayDate, expected[index]);
        }
        // 已有明确时刻：快捷换日保留时刻（含 00:00）。
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(today.addDays(-2), QTime(9, 45)));
            popupAtTop(picker);
            QTest::mouseClick(quickButton(&picker, 1), Qt::LeftButton);
            QVERIFY(!picker.isVisible());
            const ScheduleValue out = picker.schedule();
            QVERIFY(!out.allDayDate.isValid());
            QCOMPARE(toAppTime(out.dueAt).date(), today.addDays(1));
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(9, 45));
        }
        {
            DatePickerPopup picker;
            picker.setSchedule(singleTimed(today.addDays(-2), QTime(0, 0)));
            popupAtTop(picker);
            QTest::mouseClick(quickButton(&picker, 2), Qt::LeftButton);
            QVERIFY(!picker.isVisible());
            const ScheduleValue out = picker.schedule();
            QCOMPARE(toAppTime(out.dueAt).date(), today.addDays(7));
            QCOMPARE(toAppTime(out.dueAt).time(), QTime(0, 0));
        }
    }

    void calendarClickStaysDraftUntilParentAccept() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 15)));
        popupAtTop(picker);
        auto* day = dayButton(&picker, QDate(2026, 11, 20));
        QVERIFY(day && day->isVisible());
        QTest::mouseClick(day, Qt::LeftButton);
        // 普通月历点击只是草稿：弹层保持打开，未被接受。
        QVERIFY(picker.isVisible());
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Rejected));
        QCOMPARE(picker.allDayDate(), QDate(2026, 11, 20));
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(picker.schedule().allDayDate, QDate(2026, 11, 20));
    }

    void ordinaryAndRangeCardsKeepNaturalHeightsAndEntryInkAlignment() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 15), QTime(9, 17)));
        popupAtTop(picker);
        auto* card = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"));
        auto* mainPage = picker.findChild<QWidget*>(QStringLiteral("DatePickerMainPage"));
        auto* mainScroll = picker.findChild<QScrollArea*>(QStringLiteral("DatePickerMainScroll"));
        auto* datePage = picker.findChild<QWidget*>(QStringLiteral("DatePickerDatePage"));
        auto* rangePage = picker.findChild<QWidget*>(QStringLiteral("DatePickerRangePage"));
        auto* calendar = inlineCalendar(&picker);
        QVERIFY(card && mainPage && mainScroll && datePage && rangePage && calendar);
        QVERIFY(datePage->isVisible() && calendar->isVisible());
        QVERIFY(!rangePage->isVisible());
        // 普通卡片：快捷行 74 + 月历自然高 236，隐藏范围页不得撑出额外空白；
        // 主体在 DatePickerMainScroll 内自然展开，清除/确定固定在滚动区之外。
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kOrdinaryCardHeight));
        QCOMPARE(mainScroll->height(), kOrdinaryCardHeight - kCardFooterHeight);
        QCOMPARE(datePage->height(), 74);
        QCOMPARE(calendar->height(), 236);
        QCOMPARE(picker.size(), card->size() + QSize(2 * kPickerShadowMargin,
                                                    2 * kPickerShadowMargin));
        auto* confirm = card->findChild<QPushButton*>(QStringLiteral("DatePickerConfirm"));
        QVERIFY(confirm);
        QCOMPARE(confirm->parentWidget()->parentWidget(), card);  // 页脚在滚动区外

        const QString entryNames[] = {QStringLiteral("时间"), QStringLiteral("提醒"),
                                      QStringLiteral("重复")};
        const QString entryCaptions[] = {QStringLiteral("09:17"), QStringLiteral("提醒"),
                                         QStringLiteral("重复")};
        for (int i = 0; i < 3; ++i) {
            auto* entry = findEntry(&picker, entryNames[i]);
            QVERIFY(entry && entry->isVisible());
            QCOMPARE(entry->size(), QSize(kExpectCardWidth, kExpectRowHeight));
            const QRect geometry(entry->mapTo(card, QPoint()), entry->size());
            QCOMPARE(geometry.left(), 4);
            QCOMPARE(card->width() - geometry.right() - 1, 4);
            QCOMPARE(entry->text(), entryCaptions[i]);
            QCOMPARE(entry->accessibleName(), entryNames[i]);
            QVERIFY(entry->findChildren<QLabel*>().isEmpty());
        }
        // 不重复时“结束重复”入口保持隐藏。
        auto* endEntry = findEntry(&picker, QStringLiteral("结束重复"));
        QVERIFY(endEntry && !endEntry->isVisible());

        // 三个入口都按当前实际文字墨迹验收，不能只让“时间”在 3px 容差内通过。
        for (const QString& name : entryNames) {
            auto* entry = findEntry(&picker, name);
            QVERIFY(entry);
            entry->clearFocus();
            QTest::mouseMove(&picker, QPoint(1, 1));  // 静息态，避免 hover/focus 边框混入墨迹
            QApplication::processEvents();
            const qreal dpr = entry->devicePixelRatioF();
            QImage image(qCeil(entry->width() * dpr), qCeil(entry->height() * dpr),
                         QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            image.setDevicePixelRatio(dpr);
            {
                QPainter painter(&image);
                entry->render(&painter);
            }
            const auto regionInk = [&image, dpr](qreal xFromLogical, qreal xToLogical) {
                QRect bounds;
                const int x0 = qMax(0, qCeil(xFromLogical * dpr));
                const int x1 = qMin(image.width(), qCeil(xToLogical * dpr));
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const QColor pixel = image.pixelColor(x, y);
                        if (pixel.alpha() > 40
                            && qGray(pixel.red(), pixel.green(), pixel.blue()) < 235)
                            bounds = bounds.united(QRect(x, y, 1, 1));
                    }
                }
                return bounds;
            };
            const QRect iconInk = regionInk(12, 38);
            const QRect labelInk = regionInk(42, entry->width() - 40);
            QVERIFY2(!iconInk.isEmpty() && !labelInk.isEmpty(),
                     qPrintable(name + QStringLiteral("缺少图标或文字墨迹")));
            const qreal iconMid = (iconInk.top() + iconInk.bottom()) / 2.0 / dpr;
            const qreal labelMid = (labelInk.top() + labelInk.bottom()) / 2.0 / dpr;
            QVERIFY2(qAbs(iconMid - labelMid) <= 1.0,
                     qPrintable(QStringLiteral("%1：图标中线 %2，文本中线 %3")
                                    .arg(name).arg(iconMid).arg(labelMid)));
            QVERIFY2(labelMid >= iconMid - 1.0,
                     qPrintable(QStringLiteral("%1：文本不得高于图标，图标中线 %2，文本中线 %3")
                                    .arg(name).arg(iconMid).arg(labelMid)));
        }

        auto* hoverEntry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(hoverEntry);
        const QImage restCard = card->grab().toImage();
        QTest::mouseMove(hoverEntry, hoverEntry->rect().center());
        QApplication::processEvents();
        const QImage hoveredCard = card->grab().toImage();
        const qreal hoverDpr = hoveredCard.devicePixelRatio();
        const QRect hoverRect(hoverEntry->mapTo(card, QPoint()), hoverEntry->size());
        for (int y = hoverRect.top() + 2; y <= hoverRect.bottom() - 2; ++y) {
            for (const int x : {1, 2, 3, 256, 257, 258}) {
                const QPoint pixel(qRound(x * hoverDpr), qRound(y * hoverDpr));
                QVERIFY2(hoveredCard.pixelColor(pixel) == restCard.pixelColor(pixel),
                         "提醒悬浮背景不得进入父卡片左右各 4px 的留白");
            }
        }

        // 范围卡片只保留两端行与全天行；内嵌月历及快捷页完全退出布局。
        auto* rangeSegment = segmentButton(&picker, QStringLiteral("时间段"));
        QVERIFY(rangeSegment && rangeSegment->isVisible());
        QTest::mouseClick(rangeSegment, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(rangePage->isVisible());
        QVERIFY(!datePage->isVisible() && !calendar->isVisible());
        for (auto* day : calendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay")))
            QVERIFY(!day->isVisible());
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kRangeCardHeight));
        QCOMPARE(picker.size(), card->size() + QSize(2 * kPickerShadowMargin,
                                                    2 * kPickerShadowMargin));
        auto* startDate = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("开始日期")) startDate = button;
        auto* endDate = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("结束日期")) endDate = button;
        auto* startTime = rangeTimeButton(&picker, QStringLiteral("开始时间"));
        auto* endTime = rangeTimeButton(&picker, QStringLiteral("结束时间"));
        QVERIFY(startDate && endDate && startTime && endTime);
        QCOMPARE(startDate->height(), 28);
        QCOMPARE(endDate->height(), 28);
        QCOMPARE(startTime->size(), QSize(95, 28));
        QCOMPARE(endTime->size(), QSize(95, 28));
        QVERIFY(startTime->isVisible() && endTime->isVisible());
        const QString startTimeDraft = startTime->text();
        const QString endTimeDraft = endTime->text();
        QCOMPARE(startTimeDraft, QStringLiteral("09:17"));
        QCOMPARE(endTimeDraft, QStringLiteral("10:17"));

        auto* allDay = picker.findChild<QCheckBox*>(QStringLiteral("DatePickerAllDayCheck"));
        QVERIFY(allDay && allDay->isVisible() && !allDay->isChecked());
        QCOMPARE(allDay->size(), QSize(30, 18));
        QCOMPARE(allDay->accessibleName(), QStringLiteral("全天"));
        QVERIFY(allDay->text().isEmpty());
        bool separateAllDayLabel = false;
        for (auto* label : allDay->parentWidget()->findChildren<QLabel*>())
            if (label->isVisible() && label->text() == QStringLiteral("全天"))
                separateAllDayLabel = true;
        QVERIFY(separateAllDayLabel);

        QTest::mouseClick(allDay, Qt::LeftButton);
        QVERIFY(allDay->isChecked());
        QVERIFY(!startTime->isVisible() && !endTime->isVisible());
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kRangeCardHeight));
        const ScheduleValue allDayOut = picker.schedule();
        QVERIFY(allDayOut.hasScheduleRange);
        QCOMPARE(allDayOut.allDayDate, QDate(2026, 11, 15));
        QCOMPARE(allDayOut.allDayEndDate, QDate(2026, 11, 15));
        QVERIFY(!allDayOut.startAt.isValid() && !allDayOut.dueAt.isValid());

        // 取消全天只恢复可见性，不丢两端时刻草稿，也不改变紧凑卡片高度。
        QTest::mouseClick(allDay, Qt::LeftButton);
        QVERIFY(!allDay->isChecked());
        QVERIFY(startTime->isVisible() && endTime->isVisible());
        QCOMPARE(startTime->text(), startTimeDraft);
        QCOMPARE(endTime->text(), endTimeDraft);
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kRangeCardHeight));
        const ScheduleValue restored = picker.schedule();
        QVERIFY(restored.hasScheduleRange);
        QCOMPARE(toAppTime(restored.startAt).date(), QDate(2026, 11, 15));
        QCOMPARE(toAppTime(restored.startAt).time(), QTime(9, 17));
        QCOMPARE(toAppTime(restored.dueAt).date(), QDate(2026, 11, 15));
        QCOMPARE(toAppTime(restored.dueAt).time(), QTime(10, 17));
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 时间段：内嵌两端日历（插在开始/结束行之间）与跨夜选择
    // ------------------------------------------------------------------
    void rangeUiSelectsCrossNightRange() {
        DatePickerPopup picker;
        // 时刻单值进入时间段：保留 09:00、结束自动 +1 小时，全天开关不勾选。
        picker.setSchedule(singleTimed(QDate(2026, 11, 15), QTime(9, 0)));
        popupAtTop(picker);
        auto* rangeSegment = segmentButton(&picker, QStringLiteral("时间段"));
        QVERIFY(rangeSegment && rangeSegment->isVisible());
        QTest::mouseClick(rangeSegment, Qt::LeftButton);
        QApplication::processEvents();
        auto* allDay = picker.findChild<QCheckBox*>(QStringLiteral("DatePickerAllDayCheck"));
        QVERIFY(allDay && allDay->isVisible() && !allDay->isChecked());
        auto* timeEntry = findEntry(&picker, QStringLiteral("时间"));
        QVERIFY(timeEntry && !timeEntry->isVisible());  // 时间入口让位给两端行
        auto* inlineDays = inlineCalendar(&picker);
        QVERIFY(inlineDays && !inlineDays->isVisible());
        for (auto* day : inlineDays->findChildren<QToolButton*>(QStringLiteral("DatePickerDay")))
            QVERIFY(!day->isVisible());

        auto* rangePage = picker.findChild<QWidget*>(QStringLiteral("DatePickerRangePage"));
        auto* startDateButton = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("开始日期")) startDateButton = button;
        auto* endDateButton = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("结束日期")) endDateButton = button;
        QVERIFY(startDateButton && endDateButton
                && startDateButton->isVisible() && endDateButton->isVisible());
        QCOMPARE(startDateButton->height(), 28);
        QCOMPARE(endDateButton->height(), 28);

        // 开始日历内嵌插在开始行与结束行之间；Escape 只收日历并还焦点。
        auto* calendar = openRangeInlineCalendar(&picker, false);
        QVERIFY(calendar);
        QVERIFY(calendar->parentWidget() == rangePage);
        QCOMPARE(picker.findChildren<QToolButton*>(QStringLiteral("DatePickerDay")).size(), 84);
        QVERIFY(!inlineDays->isVisible());
        const QPoint startRowBottom = startDateButton->parentWidget()
            ->mapTo(rangePage, QPoint(0, startDateButton->parentWidget()->height()));
        const QPoint endRowTop = endDateButton->parentWidget()->mapTo(rangePage, QPoint());
        QVERIFY(calendar->mapTo(rangePage, QPoint()).y() >= startRowBottom.y());
        QVERIFY(calendar->mapTo(rangePage, QPoint()).y() + calendar->height() <= endRowTop.y());
        QTest::keyClick(&picker, Qt::Key_Escape);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QTRY_VERIFY(startDateButton->hasFocus());

        // 再点同一个日期入口只收起内嵌日历，父层草稿仍保持打开。
        calendar = openRangeInlineCalendar(&picker, false);
        QVERIFY(calendar);
        QTest::mouseClick(startDateButton, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());

        // 开始日历只修改开始端；选日后收起，父层与结束端草稿不变。
        calendar = openRangeInlineCalendar(&picker, false);
        QVERIFY(calendar);
        auto* selectedDay = dayButton(calendar, QDate(2026, 11, 15));
        QVERIFY(selectedDay && selectedDay->isVisible());
        QTest::mouseClick(selectedDay, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(startDateButton->text(), QStringLiteral("11月15日"));
        QCOMPARE(endDateButton->text(), QStringLiteral("11月15日"));

        // 结束日历插在结束行之下，并且只把结束端改到 11-17。
        calendar = openRangeInlineCalendar(&picker, true);
        QVERIFY(calendar);
        const int endRowBottom = endDateButton->parentWidget()
            ->mapTo(rangePage, QPoint(0, endDateButton->parentWidget()->height())).y();
        QVERIFY(calendar->mapTo(rangePage, QPoint()).y() >= endRowBottom);
        selectedDay = dayButton(calendar, QDate(2026, 11, 17));
        QVERIFY(selectedDay && selectedDay->isVisible());
        QTest::mouseClick(selectedDay, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(startDateButton->text(), QStringLiteral("11月15日"));
        QCOMPARE(endDateButton->text(), QStringLiteral("11月17日"));

        // 两端时刻：22:00 → 01:00（跨夜）；时间展开区插在对应行之下。
        auto* startButton = rangeTimeButton(&picker, QStringLiteral("开始时间"));
        auto* endButton = rangeTimeButton(&picker, QStringLiteral("结束时间"));
        QVERIFY(startButton && endButton && startButton->isVisible() && endButton->isVisible());
        QCOMPARE(startButton->size(), QSize(95, 28));
        QCOMPARE(endButton->size(), QSize(95, 28));
        auto* expansion = openTimeExpansion(&picker, startButton);
        QVERIFY(expansion);
        QVERIFY(expansion->parentWidget() == rangePage);
        QVERIFY(expansion->mapTo(rangePage, QPoint()).y()
                >= startButton->parentWidget()->mapTo(rangePage, QPoint(0, 28)).y());
        QVERIFY(expansion->mapTo(rangePage, QPoint()).y() + expansion->height()
                <= endDateButton->parentWidget()->mapTo(rangePage, QPoint()).y());
        auto* list = listOf(expansion);
        QVERIFY(list);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(44)).center());  // 22:00
        QTRY_VERIFY(!expansion->isVisible());
        expansion = openTimeExpansion(&picker, endButton);
        QVERIFY(expansion);
        list = listOf(expansion);
        QVERIFY(list);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(2)).center());   // 01:00
        QTRY_VERIFY(!expansion->isVisible());
        QCOMPARE(startButton->text(), QStringLiteral("22:00"));
        QCOMPARE(endButton->text(), QStringLiteral("01:00"));

        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        const ScheduleValue out = picker.schedule();
        QVERIFY(out.hasScheduleRange);
        QCOMPARE(toAppTime(out.startAt).date(), QDate(2026, 11, 15));
        QCOMPARE(toAppTime(out.startAt).time(), QTime(22, 0));
        QCOMPARE(toAppTime(out.dueAt).date(), QDate(2026, 11, 17));
        QCOMPARE(toAppTime(out.dueAt).time(), QTime(1, 0));
        QVERIFY(!out.allDayDate.isValid());
        picker.reject();
    }

    void rangeInvalidEndBlocksAcceptAndStaysOpen() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 20)));  // 固定可见月份
        popupAtTop(picker);
        auto* rangeSegment = segmentButton(&picker, QStringLiteral("时间段"));
        QVERIFY(rangeSegment && rangeSegment->isVisible());
        QTest::mouseClick(rangeSegment, Qt::LeftButton);
        QApplication::processEvents();
        auto* allDay = picker.findChild<QCheckBox*>(QStringLiteral("DatePickerAllDayCheck"));
        QVERIFY(allDay && allDay->isChecked());  // 全天单值进入时间段保持全天
        auto* startDateButton = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("开始日期")) startDateButton = button;
        auto* endDateButton = picker.findChild<QPushButton*>(QStringLiteral("DatePickerRangeDate"));
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerRangeDate")))
            if (button->accessibleName() == QStringLiteral("结束日期")) endDateButton = button;
        QVERIFY(startDateButton && endDateButton
                && startDateButton->isVisible() && endDateButton->isVisible());
        // 全天区间：开始、结束分别在自己的内嵌日历选择。
        auto* calendar = openRangeInlineCalendar(&picker, false);
        QVERIFY(calendar);
        auto* selectedDay = dayButton(calendar, QDate(2026, 11, 20));
        QVERIFY(selectedDay && selectedDay->isVisible());
        QTest::mouseClick(selectedDay, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        calendar = openRangeInlineCalendar(&picker, true);
        QVERIFY(calendar);
        selectedDay = dayButton(calendar, QDate(2026, 11, 10));
        QVERIFY(selectedDay && selectedDay->isVisible());
        QTest::mouseClick(selectedDay, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(startDateButton->text(), QStringLiteral("11月20日"));
        QCOMPARE(endDateButton->text(), QStringLiteral("11月10日"));

        // 结束早于开始 → 行内报错、保持打开；报错行之外不多出空白。
        picker.accept();
        QVERIFY(picker.isVisible());
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Rejected));
        auto* error = visibleErrorIn(&picker, QStringLiteral("结束日期不能早于开始日期"));
        QVERIFY(error);
        auto* card = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"));
        QVERIFY(card);
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kRangeCardHeight + error->height()));
        QCOMPARE(picker.size(), card->size() + QSize(2 * kPickerShadowMargin,
                                                    2 * kPickerShadowMargin));
        // 结束端保持 11-10（绝不静默交换开始端）；改为合法结束日后错误清除并可接受。
        calendar = openRangeInlineCalendar(&picker, true);
        QVERIFY(calendar);
        selectedDay = dayButton(calendar, QDate(2026, 11, 25));
        QVERIFY(selectedDay && selectedDay->isVisible());
        QTest::mouseClick(selectedDay, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QVERIFY(!visibleErrorIn(&picker, QStringLiteral("结束日期不能早于开始日期")));
        QCOMPARE(card->size(), QSize(kPickerCardWidth, kRangeCardHeight));
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        const ScheduleValue out = picker.schedule();
        QVERIFY(out.hasScheduleRange);
        QCOMPARE(out.allDayDate, QDate(2026, 11, 20));
        QCOMPARE(out.allDayEndDate, QDate(2026, 11, 25));

        // 计时区间同日 10:00 → 09:00：同样阻止。
        DatePickerPopup timed;
        // 时刻单值进入时间段：开始 10:00、结束自动 +1 小时。
        timed.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(10, 0)));
        popupAtTop(timed);
        auto* timedSegment = segmentButton(&timed, QStringLiteral("时间段"));
        QVERIFY(timedSegment && timedSegment->isVisible());
        QTest::mouseClick(timedSegment, Qt::LeftButton);
        QApplication::processEvents();
        auto* startButton = rangeTimeButton(&timed, QStringLiteral("开始时间"));
        auto* endButton = rangeTimeButton(&timed, QStringLiteral("结束时间"));
        QVERIFY(startButton && endButton && startButton->isVisible() && endButton->isVisible());
        auto* expansion = openTimeExpansion(&timed, endButton);
        QVERIFY(expansion);
        auto* list = listOf(expansion);
        QVERIFY(list);
        auto* earlierTime = list->item(18);  // 09:00 < 10:00
        QVERIFY(earlierTime);
        list->scrollToItem(earlierTime, QAbstractItemView::PositionAtCenter);
        QApplication::processEvents();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(earlierTime).center());
        QTRY_VERIFY(!expansion->isVisible());
        timed.accept();
        QVERIFY(timed.isVisible());
        QVERIFY(visibleErrorIn(&timed, QStringLiteral("结束时间不能早于开始时间")));
        timed.reject();
    }

    // ------------------------------------------------------------------
    // 模式切换按钮：键盘可达、无焦点环
    // ------------------------------------------------------------------
    void modeButtonsKeepKeyboardWithoutFocusRing() {
        DatePickerPopup picker;
        picker.show(); QApplication::processEvents();
        auto* host = picker.findChild<QWidget*>(QStringLiteral("DatePickerSegmentHost"));
        QVERIFY(host); QCOMPARE(host->height(), 30);
        const auto buttons = picker.findChildren<QPushButton*>(QStringLiteral("DatePickerSegment"));
        QCOMPARE(buttons.size(), 2);
        for (auto* button : buttons) {
            QCOMPARE(button->focusPolicy(), Qt::StrongFocus);  // 键盘可达
            QVERIFY(!button->autoDefault());                    // AutoDefault false
            QVERIFY(!button->isDefault());
            QCOMPARE(button->height(), 26);
            QCOMPARE(button->mapTo(host, QPoint(0, 0)).y(), 2);
            QCOMPARE(host->height() - (button->mapTo(host, QPoint(0, button->height())).y()), 2);
        }
        const QString qss = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"))->styleSheet();
        QVERIFY(!qss.contains(QStringLiteral("DatePickerSegment:focus")));  // 无 focus 边框
        QVERIFY(qss.contains(QStringLiteral("DatePickerSegment:checked")));  // 保留 checked 底色
        QVERIFY(qss.contains(QStringLiteral("border-radius: 6px")));
        QVERIFY(qss.contains(QStringLiteral("border-radius: 14px")));
        QVERIFY(!picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"))->graphicsEffect());
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 内嵌时间展开区：48 行半小时表 + 精确分钟 + Escape/焦点
    // ------------------------------------------------------------------
    void timeTextParserAcceptsSingleDigitsAndRejectsMalformedInput() {
        const struct { const char* text; QTime expected; } valid[] = {
            {"0:0", QTime(0, 0)}, {"9:4", QTime(9, 4)}, {"9:04", QTime(9, 4)},
            {"09:4", QTime(9, 4)}, {"09:04", QTime(9, 4)}, {"23:59", QTime(23, 59)}
        };
        for (const auto& value : valid) {
            QTime parsed;
            QVERIFY2(DatePickerUi::parseTimeText(QString::fromLatin1(value.text), &parsed), value.text);
            QCOMPARE(parsed, value.expected);
        }
        const QStringList invalid = {QStringLiteral("24:00"), QStringLiteral("23:60"),
            QStringLiteral("9:"), QStringLiteral(":04"), QStringLiteral("9:004"),
            QStringLiteral("9:4:00"), QStringLiteral("123:4")};
        for (const QString& text : invalid) {
            QTime parsed(12, 34);
            QVERIFY(!DatePickerUi::parseTimeText(text, &parsed));
            QVERIFY(!parsed.isValid());
        }
    }

    void timeExpansionInlineListPreciseInputAndEscape() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 15)));
        popupAtTop(picker);
        auto* card = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"));
        auto* entry = findEntry(&picker, QStringLiteral("时间"));
        QVERIFY(entry && entry->isVisible());
        auto* expansion = openTimeExpansion(&picker, entry);
        QVERIFY(expansion);
        QVERIFY(expansion->parentWidget()->objectName() == QStringLiteral("DatePickerMainPage"));
        auto* list = listOf(expansion);
        auto* edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(list && edit);
        QVERIFY(list->isVisible() && edit->isVisible());
        QCOMPARE(list->count(), 48);                       // 00:00..23:30 半小时表
        QCOMPARE(list->item(0)->text(), QStringLiteral("00:00"));
        QCOMPARE(list->item(47)->text(), QStringLiteral("23:30"));
        QCOMPARE(list->visualItemRect(list->item(0)).height(), kExpectRowHeight);
        QCOMPARE(list->height(), 6 * kExpectRowHeight);    // 固定 6 行视口
        QCOMPARE(list->verticalScrollMode(), QAbstractItemView::ScrollPerPixel);
        // 展开期内嵌月历让位；展开区正好压在时间入口行之上（同列、间距 0）。
        auto* calendar = inlineCalendar(&picker);
        QVERIFY(calendar && !calendar->isVisible());
        const QPoint sectionTop = entry->parentWidget()->mapTo(card, QPoint());
        QCOMPARE(expansion->mapTo(card, QPoint()).y() + expansion->height(), sectionTop.y());
        // 旧“无时间”按钮已移除：清空输入回车承担移除时刻的职责。
        QVERIFY(picker.findChild<QPushButton*>(QStringLiteral("DatePickerNoTime")) == nullptr);
        // 滚轮可用：视口收到滚轮事件后滚动条前进。
        auto* bar = list->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        QCOMPARE(bar->value(), 0);
        QWheelEvent wheel(list->viewport()->rect().center(), QPointF(), QPoint(0, -120),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate,
                          Qt::MouseEventNotSynthesized);
        QApplication::sendEvent(list->viewport(), &wheel);
        QVERIFY(bar->value() > 0);
        // 输入行是列表兄弟节点：滚动列表不移动输入行。
        const QPoint editBefore = edit->mapToGlobal(QPoint());
        list->scrollToBottom();
        QApplication::processEvents();
        QCOMPARE(edit->mapToGlobal(QPoint()), editBefore);

        // 单数字小时／分钟与混合写法都规范化为 09:04，回车即落地并收起。
        const QStringList acceptedInputs = {QStringLiteral("9:4"), QStringLiteral("9:04"),
                                            QStringLiteral("09:4")};
        for (int index = 0; index < acceptedInputs.size(); ++index) {
            if (index > 0) {
                expansion = openTimeExpansion(&picker, entry);
                QVERIFY(expansion);
                edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
                QVERIFY(edit);
            }
            edit->setText(acceptedInputs.at(index));
            QTest::keyClick(edit, Qt::Key_Return);
            QTRY_VERIFY(!expansion->isVisible());
            QVERIFY(picker.hasTime());
            QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(9, 4));
            QCOMPARE(entry->text(), QStringLiteral("09:04"));
            QVERIFY(calendar->isVisible());
            QTRY_VERIFY(entry->hasFocus());
        }

        // 非法输入：停留展开区、标红、不落地。
        expansion = openTimeExpansion(&picker, entry);
        QVERIFY(expansion);
        edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(edit);
        edit->setText(QStringLiteral("25:99"));
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(expansion->isVisible());
        QVERIFY(edit->property("invalid").toBool());
        QVERIFY(picker.hasTime());  // 原值 09:04 未被破坏

        // 清空回车（单日）＝ 移除时刻，回到全天；保留日期。
        edit->setText(QString());
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(!picker.hasTime());
        QCOMPARE(picker.allDayDate(), QDate(2026, 11, 15));
        QCOMPARE(entry->text(), QStringLiteral("时间"));

        // Escape 只收内嵌时间层，父层保持打开，焦点还入口。
        expansion = openTimeExpansion(&picker, entry);
        QVERIFY(expansion);
        QTest::keyClick(&picker, Qt::Key_Escape);
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(picker.isVisible());
        QTRY_VERIFY(entry->hasFocus());
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 提醒 / 重复锚定子层：自然高度与窗口模型（AnchoredListPopup 契约）
    // ------------------------------------------------------------------
    void reminderAndRecurrenceLayersKeepNaturalHeights() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 15), QTime(9, 0)));
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        picker.popupAt(QRect(QPoint(avail.left() + 60, avail.top() + 40), QSize(80, 28)));
        QTRY_VERIFY(picker.isVisible());

        auto* entry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(entry && entry->isVisible());
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        // 窗口模型：Popup/Frameless/NoDropShadow + 半透明 + 阴影边距，绝不 setMask。
        QVERIFY(reminder->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY((reminder->windowFlags() & Qt::Popup) == Qt::Popup);
        QVERIFY((reminder->windowFlags() & Qt::FramelessWindowHint) == Qt::FramelessWindowHint);
        QVERIFY((reminder->windowFlags() & Qt::NoDropShadowWindowHint) == Qt::NoDropShadowWindowHint);
        QVERIFY(reminder->mask().isEmpty());
        const QRect card = reminder->cardGeometryGlobal();
        QCOMPARE(card.width(), kExpectCardWidth);
        QCOMPARE(reminder->frameGeometry().size(),
                 QSize(kExpectCardWidth + 2 * AnchoredListPopup::shadowMargin(),
                       card.height() + 2 * AnchoredListPopup::shadowMargin()));
        // 锚定：默认从入口下方 4px 展开。
        const QRect entryRect = globalRect(entry);
        QCOMPARE(card.top(), entryRect.bottom() + 1 + kExpectAnchorGap);
        QVERIFY(!reminder->flippedAbove());
        QVERIFY(card.bottom() <= avail.bottom());
        // 菜单：6 预设 + 自定义 = 7 行（最近项每测试已清空）；34px 菜单行 + 滚动区。
        const auto rows = menuRowsOf(reminder);
        QCOMPARE(rows.size(), 7);
        for (auto* row : rows) QCOMPARE(row->height(), kExpectRowHeight);
        auto* scroll = reminder->findChild<QScrollArea*>(QStringLiteral("DatePickerScroll"));
        QVERIFY(scroll);
        QVERIFY(scroll->height() <= 7 * kExpectRowHeight);  // 小屏按下方空间压缩
        QVERIFY(scroll->height() >= 2 * kExpectRowHeight);
        // 不再是 QListWidget；也没有持续提醒类复选控件。
        QVERIFY(reminder->findChild<QListWidget*>() == nullptr);
        QVERIFY(reminder->findChildren<QCheckBox*>().isEmpty());
        auto* done = visibleButtonWithText(reminder, QStringLiteral("确定"));
        QVERIFY(done && done->isVisible());
        QCOMPARE(card.height(), 4 + scroll->height() + 20 + done->height());
        // “自定义”入口切换独立自定义页（同一锚定卡片）。
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        QVERIFY(reminder->findChild<QWidget*>(
                    QStringLiteral("DatePickerReminderCustomPage"))->isVisible());
        QVERIFY(!reminder->findChild<QWidget*>(
                     QStringLiteral("DatePickerReminderMenu"))->isVisible());
        QTest::keyClick(reminder, Qt::Key_Escape);
        QTRY_VERIFY(!reminder->isVisible());

        // 重复子层：未重复时 6 个预设行、无原始 RRULE 输入框。
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        // 2026-11-15 是周日。
        QCOMPARE(menuRowTexts(recurrence),
                 QStringList({QStringLiteral("每天"), QStringLiteral("每周 (周日)"),
                              QStringLiteral("每月 (15日)"), QStringLiteral("每年 (11月15日)"),
                              QStringLiteral("工作日"), QStringLiteral("自定义")}));
        auto* recurrenceScroll = recurrence->findChild<QScrollArea*>(QStringLiteral("DatePickerScroll"));
        QVERIFY(recurrenceScroll);
        QCOMPARE(recurrence->cardGeometryGlobal().height(), recurrenceScroll->height());
        QVERIFY(recurrenceScroll->height() >= 2 * kExpectRowHeight);
        QVERIFY(recurrenceScroll->height() <= 6 * kExpectRowHeight + 8);
        QVERIFY(avail.contains(recurrence->cardGeometryGlobal()));
        const int recurrenceBelow = avail.bottom()
            - globalRect(findEntry(&picker, QStringLiteral("重复"))).bottom() - kExpectAnchorGap;
        if (recurrenceBelow >= 8 + 6 * kExpectRowHeight)
            QCOMPARE(recurrenceScroll->height(), 6 * kExpectRowHeight + 8);
        for (QLineEdit* edit : recurrence->findChildren<QLineEdit*>())
            QVERIFY(qobject_cast<QAbstractSpinBox*>(edit->parentWidget()));  // 无原始 RRULE 输入
        QTest::keyClick(recurrence, Qt::Key_Escape);
        QTRY_VERIFY(!recurrence->isVisible());
        picker.reject();
    }

    void layerShrinksListThenFlipsNearScreenBottom() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 15)));
        popupAtTop(picker);
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        auto* entry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(entry);
        AnchoredListPopup* layer = openLayer(&picker, QStringLiteral("提醒"),
                                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(layer);
        auto* scroll = layer->findChild<QScrollArea*>(QStringLiteral("DatePickerScroll"));
        QVERIFY(scroll);
        const int fixedPart = layer->cardGeometryGlobal().height() - scroll->height();
        QCOMPARE(fixedPart, 48);  // 上边距4 + 页脚44；六行全天菜单首选204px。
        const int naturalCardHeight = fixedPart + 6 * kExpectRowHeight;
        const int entryBottomOffset = globalRect(entry).bottom() - picker.y();
        layer->reject();
        QTRY_VERIFY(!layer->isVisible());

        // 直接重锚同一真实子卡片，单独覆盖屏幕边缘策略；父卡片的自动夹入另有回归。
        const int targetListHeight = 150;
        picker.move(avail.left() + 40, avail.bottom() - fixedPart - targetListHeight
                    - kExpectAnchorGap - entryBottomOffset);
        layer->setUpperBoundGlobalY(monthGridTopGlobal(&picker) + 30);
        layer->popupBelow(entry);
        QTRY_VERIFY(layer->isVisible());
        QCOMPARE(layer->cardGeometryGlobal().top(),
                 globalRect(entry).bottom() + 1 + kExpectAnchorGap);
        QVERIFY(!layer->flippedAbove());
        QCOMPARE(scroll->height(), targetListHeight);
        QVERIFY(layer->cardGeometryGlobal().height() < naturalCardHeight);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QVERIFY(avail.contains(layer->cardGeometryGlobal()));
        layer->reject();
        QTRY_VERIFY(!layer->isVisible());

        // 下方连两行都放不下时上翻；本例上方仍能在月历首行之后容纳最小菜单。
        const int tooLittleBelow = fixedPart + 2 * kExpectRowHeight - 1;
        picker.move(avail.left() + 40, avail.bottom() - tooLittleBelow
                    - kExpectAnchorGap - entryBottomOffset);
        const int guard = monthGridTopGlobal(&picker) + 30;
        layer->setUpperBoundGlobalY(guard);
        layer->popupBelow(entry);
        QTRY_VERIFY(layer->isVisible());
        const QRect card = layer->cardGeometryGlobal();
        QVERIFY(layer->flippedAbove());
        QVERIFY(card.bottom() < globalRect(entry).top());
        QVERIFY(card.top() >= guard);
        QVERIFY(avail.contains(card));
        QVERIFY(scroll->height() >= 2 * kExpectRowHeight);
        QVERIFY(picker.isVisible());
        picker.reject();
    }

    void calendarSelectionAndPresetEndConditionsStayStable() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));
        popupAtTop(picker);
        auto* selected = dayButton(&picker, QDate(2026, 11, 18));
        QVERIFY(selected && selected->isChecked());
        QTest::mouseClick(selected, Qt::LeftButton);
        QVERIFY(selected->isChecked());
        QTest::mouseClick(selected, Qt::LeftButton);
        QVERIFY(selected->isChecked());
        QCOMPARE(picker.schedule().allDayDate, QDate(2026, 11, 18));

        ScheduleValue value = picker.schedule();
        value.repeating = true;
        value.recurrence.frequency = RecurrenceSpec::Frequency::Daily;
        value.recurrence.weekStart = Qt::Sunday;
        value.recurrence.endMode = RecurrenceSpec::EndMode::Date;
        value.recurrence.endDate = QDate(2027, 1, 30);
        picker.setSchedule(value);
        auto* layer = openLayer(&picker, QStringLiteral("重复"),
                                 QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(layer);
        auto* daily = menuRowByText(layer, QStringLiteral("每天"));
        QVERIFY(daily && daily->isChecked());
        clickMenuRowByText(layer, QStringLiteral("每周 (周三)"));
        QTRY_VERIFY(!layer->isVisible());
        const RecurrenceSpec out = picker.schedule().recurrence;
        QCOMPARE(out.frequency, RecurrenceSpec::Frequency::Weekly);
        QCOMPARE(out.weekStart, static_cast<int>(Qt::Sunday));
        QCOMPARE(out.endMode, RecurrenceSpec::EndMode::Date);
        QCOMPARE(out.endDate, QDate(2027, 1, 30));
        QVERIFY(isValidRecurrence(out));
        picker.reject();
    }

    void menuKeyboardSelectionCancelsWithItsLayer() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(9, 0)));
        popupAtTop(picker);
        auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                 QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(layer);
        auto* onTime = menuRowByText(layer, QStringLiteral("准时"));
        auto* before = menuRowByText(layer, QStringLiteral("提前5分钟"));
        QVERIFY(onTime && before);
        onTime->setFocus();
        QTest::keyClick(onTime, Qt::Key_Down);
        QTRY_VERIFY(before->hasFocus());
        QTest::keyClick(before, Qt::Key_Return);
        QVERIFY(before->isChecked());
        QCOMPARE(picker.schedule().reminders.size(), 1);
        QTest::keyClick(before, Qt::Key_Up);
        QTRY_VERIFY(onTime->hasFocus());
        QTest::keyClick(onTime, Qt::Key_Return);
        QVERIFY(onTime->isChecked());
        QCOMPARE(picker.schedule().reminders.size(), 2);
        QTest::keyClick(onTime, Qt::Key_Escape);
        QTRY_VERIFY(!layer->isVisible());
        QVERIFY(picker.isVisible());
        QVERIFY(picker.schedule().reminders.isEmpty());
        QTRY_VERIFY(findEntry(&picker, QStringLiteral("提醒"))->hasFocus());
        picker.reject();
    }

    void nestedDropdownEscapeAndParentCloseCascade() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));
        popupAtTop(picker);
        auto* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                      QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        auto* basis = comboByAccessible(recurrence, QStringLiteral("重复基准"));
        QVERIFY(basis && basis->isVisible());
        QTest::mouseClick(basis, Qt::LeftButton);
        QPointer<QMenu> menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu || !menu->isVisible());
        QVERIFY(recurrence->isVisible());
        QVERIFY(picker.isVisible());
        QTRY_VERIFY(basis->hasFocus());
        QTest::keyClick(basis, Qt::Key_Escape);
        QTRY_VERIFY(!recurrence->isVisible());
        QVERIFY(picker.isVisible());
        QTRY_VERIFY(findEntry(&picker, QStringLiteral("重复"))->hasFocus());

        recurrence = openLayer(&picker, QStringLiteral("重复"),
                                QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        basis = comboByAccessible(recurrence, QStringLiteral("重复基准"));
        QVERIFY(basis);
        QTest::mouseClick(basis, Qt::LeftButton);
        menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        QVERIFY(menu && menu->isVisible());
        picker.reject();
        QTRY_VERIFY(!picker.isVisible());
        QTRY_VERIFY(!recurrence->isVisible());
        QTRY_VERIFY(!menu || !menu->isVisible());
        QVERIFY(!picker.schedule().repeating);
    }

    void legacyAbsoluteReminderKeepsIdentityAcrossMenuCancel() {
        DatePickerPopup picker;
        ScheduleValue value = singleTimed(QDate(2026, 11, 20), QTime(9, 0));
        TaskReminder reminder;
        reminder.id = newUuid();
        reminder.triggerAt = fromAppTime(QDateTime(QDate(2026, 11, 20), QTime(8, 20)));
        value.reminders.append(reminder);
        picker.setSchedule(value);
        popupAtTop(picker);
        auto* layer = openLayer(&picker, QStringLiteral("提醒"),
                                 QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(layer);
        auto* row = menuRowByText(layer, QStringLiteral("2026年11月20日 08:20"));
        QVERIFY(row && row->isChecked());
        QTest::mouseClick(row, Qt::LeftButton);
        QVERIFY(picker.schedule().reminders.isEmpty());
        QTest::keyClick(row, Qt::Key_Escape);
        QTRY_VERIFY(!layer->isVisible());
        const auto restored = picker.schedule().reminders;
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.first().id, reminder.id);
        QVERIFY(restored.first().ruleJson.isEmpty());
        QCOMPARE(restored.first().triggerAt, reminder.triggerAt);
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(picker.schedule().reminders.first().id, reminder.id);
    }

    void escapeCancelsOnlySubLayerAndReturnsFocus() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 12, 1), QTime(8, 0)));
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        picker.popupAt(QRect(QPoint(avail.left() + 60, avail.top() + 40), QSize(80, 28)));
        QTRY_VERIFY(picker.isVisible());
        auto* timeEntry = findEntry(&picker, QStringLiteral("时间"));
        QVERIFY(timeEntry);

        // 列表键盘可达：Enter 激活当前项（08:00 = 行 16）。
        auto* expansion = openTimeExpansion(&picker, timeEntry);
        QVERIFY(expansion);
        auto* list = listOf(expansion);
        QVERIFY(list);
        list->setCurrentRow(16);
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(picker.isVisible());  // 收内嵌层只取消内嵌层，不关父
        QVERIFY(picker.hasTime());
        QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(8, 0));
        QTRY_VERIFY(timeEntry->hasFocus());  // 焦点回入口

        // Escape 收内嵌时间层：父层草稿保持不变。
        expansion = openTimeExpansion(&picker, timeEntry);
        QVERIFY(expansion);
        QTest::keyClick(&picker, Qt::Key_Escape);
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(8, 0));
        QTRY_VERIFY(timeEntry->hasFocus());

        // 提醒子层：勾选行保持打开（多选）；“确定”只关子层并保留父草稿。
        auto* reminderEntry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(reminderEntry);
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("准时"));
        QVERIFY(reminder->isVisible());  // 多选：子层保持打开
        QCOMPARE(picker.schedule().reminders.size(), 1);
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(picker.schedule().reminders.size(), 1);
        QCOMPARE(reminderEntry->text(), QStringLiteral("准时"));
        QTRY_VERIFY(reminderEntry->hasFocus());

        // 父层关闭时子层一并收起。
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        picker.reject();
        QTRY_VERIFY(!picker.isVisible());
        QTRY_VERIFY(!recurrence->isVisible());
    }

    void entriesAreMutuallyExclusiveAndAcceptClosesUnconfirmedSubLayer() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 18), QTime(9, 0)));
        popupAtTop(picker);
        // 打开提醒子层后点“重复”入口：提醒子层收起、重复子层打开。
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        QTest::mouseClick(findEntry(&picker, QStringLiteral("重复")), Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        AnchoredListPopup* recurrence = nullptr;
        for (auto* candidate : picker.findChildren<AnchoredListPopup*>(
                 QStringLiteral("DatePickerRecurrenceLayer")))
            if (candidate->isVisible()) recurrence = candidate;
        QVERIFY(recurrence);
        QTest::keyClick(recurrence, Qt::Key_Escape);
        QTRY_VERIFY(!recurrence->isVisible());

        // 父层 accept 先收起未确认子层（回滚其未提交勾选）再校验最终父草稿。
        reminder = openLayer(&picker, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("提前5分钟"));
        QCOMPARE(picker.schedule().reminders.size(), 1);  // 子层草稿可见
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(!reminder->isVisible());
        // 未确认的勾选被回滚：最终父草稿没有这条提醒。
        QCOMPARE(picker.schedule().reminders.size(), 0);
        QCOMPARE(findEntry(&picker, QStringLiteral("提醒"))->text(), QStringLiteral("提醒"));
    }

    // ------------------------------------------------------------------
    // 多条一次性提醒（ReminderSpec）：菜单行多选、对勾、稳定 UUID
    // ------------------------------------------------------------------
    void reminderMenuRowsToggleWithCheckmarkAndUuid() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(9, 0)));
        popupAtTop(picker);
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        const QStringList texts = menuRowTexts(reminder);
        QCOMPARE(texts.size(), 7);
        QCOMPARE(texts, QStringList({QStringLiteral("准时"), QStringLiteral("提前5分钟"),
                                     QStringLiteral("提前15分钟"), QStringLiteral("提前30分钟"),
                                     QStringLiteral("提前1小时"), QStringLiteral("提前1天"),
                                     QStringLiteral("自定义")}));
        QVERIFY(!texts.contains(QStringLiteral("结束时")));  // 非时间段没有结束锚点
        QVERIFY(!texts.join(QString()).contains(QStringLiteral("持续")));  // 无持续提醒 UI
        auto* row = menuRowByText(reminder, QStringLiteral("准时"));
        QVERIFY(row);
        QTest::mouseClick(row, Qt::LeftButton);
        QVERIFY(reminder->isVisible());   // 多选：子层保持打开
        QVERIFY(row->isChecked());
        ScheduleValue out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        ReminderSpec spec;
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QVERIFY(!spec.allDay);
        QCOMPARE(spec.anchor, ReminderSpec::Anchor::Schedule);
        QCOMPARE(spec.offset, 0);
        QVERIFY(isUuid(out.reminders.first().id));           // 新行生成 UUID
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).date(), QDate(2026, 11, 20));
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).time(), QTime(9, 0));
        // 选中行右侧渲染对勾；未选中行右侧没有墨迹。
        QVERIFY(menuRowCheckInk(row) > 0);
        auto* other = menuRowByText(reminder, QStringLiteral("提前1小时"));
        QVERIFY(other);
        QCOMPARE(menuRowCheckInk(other), 0);
        // 取消勾选：移除；再次勾选：恢复。
        QTest::mouseClick(row, Qt::LeftButton);
        QVERIFY(!row->isChecked());
        QCOMPARE(picker.schedule().reminders.size(), 0);
        QTest::mouseClick(row, Qt::LeftButton);
        QCOMPARE(picker.schedule().reminders.size(), 1);

        // 已载入的提醒行保留原 UUID（稳定 UUID：不重建）。
        const QString keptId = picker.schedule().reminders.first().id;
        ScheduleValue withReminder = singleTimed(QDate(2026, 11, 20), QTime(9, 0));
        withReminder.reminders = picker.schedule().reminders;
        picker.setSchedule(withReminder);
        out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        QCOMPARE(out.reminders.first().id, keptId);
        QCOMPARE(out.reminders.first().ruleJson, withReminder.reminders.first().ruleJson);
        picker.reject();
    }

    void reminderCustomTimedAndAllDayForms() {
        // 时刻计划：相对偏移自定义（40 分钟前）。
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(9, 0)));
        popupAtTop(picker);
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        QVERIFY(reminder->findChild<QWidget*>(
                    QStringLiteral("DatePickerReminderTimedCustom"))->isVisible());
        QVERIFY(!reminder->findChild<QWidget*>(
                     QStringLiteral("DatePickerReminderAllDayCustom"))->isVisible());
        auto* spin = spinByAccessible(reminder, QStringLiteral("提醒提前量"));
        auto* unit = comboByAccessible(reminder, QStringLiteral("提醒单位"));
        QVERIFY(spin && unit);
        spin->setValue(40);
        unit->setCurrentIndex(0);  // 分钟
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        ScheduleValue out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        ReminderSpec spec;
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QCOMPARE(spec.unit, ReminderSpec::Unit::Minutes);
        QCOMPARE(spec.offset, 40);
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).time(), QTime(8, 20));
        QCOMPARE(findEntry(&picker, QStringLiteral("提醒"))->text(),
                 QStringLiteral("提前40分钟"));
        // 重复添加同一自定义规则：去重，不再新增。
        reminder = openLayer(&picker, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        spinByAccessible(reminder, QStringLiteral("提醒提前量"))->setValue(40);
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QCOMPARE(picker.schedule().reminders.size(), 1);
        picker.reject();

        // 全天计划：按天提前 + 本地时刻。
        DatePickerPopup allDay;
        allDay.setSchedule(singleAllDay(QDate(2026, 11, 20)));
        popupAtTop(allDay);
        reminder = openLayer(&allDay, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        // 全天预设 5 行 + 自定义。
        const QStringList labels = menuRowTexts(reminder);
        QCOMPARE(labels.size(), 6);
        QCOMPARE(labels.mid(0, 5),
                 QStringList({QStringLiteral("当天"), QStringLiteral("提前1天"),
                              QStringLiteral("提前2天"), QStringLiteral("提前3天"),
                              QStringLiteral("提前1周")}));
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        QVERIFY(reminder->findChild<QWidget*>(
                    QStringLiteral("DatePickerReminderAllDayCustom"))->isVisible());
        QVERIFY(!reminder->findChild<QWidget*>(
                     QStringLiteral("DatePickerReminderTimedCustom"))->isVisible());
        auto* mode = comboByAccessible(reminder, QStringLiteral("全天提醒提前方式"));
        auto* daySpin = spinByAccessible(reminder, QStringLiteral("提醒提前天数"));
        auto* timeEdit = editByAccessible(reminder, QStringLiteral("提醒本地时间"));
        QVERIFY(mode && daySpin && timeEdit);
        mode->setCurrentIndex(0);  // 按天提前
        daySpin->setValue(2);
        timeEdit->setText(QStringLiteral("8:3"));
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        out = allDay.schedule();
        QCOMPARE(out.reminders.size(), 1);
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QVERIFY(spec.allDay);
        QCOMPARE(spec.dayOffset, 2);
        QCOMPARE(spec.localTime, QTime(8, 3));
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).date(), QDate(2026, 11, 18));
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).time(), QTime(8, 3));
        QCOMPARE(findEntry(&allDay, QStringLiteral("提醒"))->text(),
                 QStringLiteral("提前2天(08:03)"));

        // 按周提前：1 周 × 09:00 → “提前1周”。
        reminder = openLayer(&allDay, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        comboByAccessible(reminder, QStringLiteral("全天提醒提前方式"))->setCurrentIndex(1);
        spinByAccessible(reminder, QStringLiteral("提醒提前天数"))->setValue(1);
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        out = allDay.schedule();
        QCOMPARE(out.reminders.size(), 2);
        QVERIFY(reminderFromRuleText(out.reminders.last().ruleJson, &spec));
        QVERIFY(spec.allDay);
        QCOMPARE(spec.dayOffset, 7);
        QCOMPARE(spec.localTime, QTime(9, 0));
        // 摘要按规则顺序：提前2天(08:03) 在前、提前1周 在后。
        QCOMPARE(findEntry(&allDay, QStringLiteral("提醒"))->text(),
                 QStringLiteral("提前2天(08:03), 提前1周"));

        // 非法本地时刻：行内报错、保持打开、不落地。
        reminder = openLayer(&allDay, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("自定义"));
        timeEdit = editByAccessible(reminder, QStringLiteral("提醒本地时间"));
        QVERIFY(timeEdit);
        timeEdit->setText(QStringLiteral("25:99"));
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QVERIFY(reminder->isVisible());
        QVERIFY(timeEdit->property("invalid").toBool());
        QVERIFY(visibleErrorIn(reminder, QStringLiteral("提醒时间需为")));
        QCOMPARE(allDay.schedule().reminders.size(), 2);
        QTest::keyClick(reminder, Qt::Key_Escape);
        QTRY_VERIFY(!reminder->isVisible());
        allDay.reject();
    }

    void reminderRangeShowsEndAnchorAndNoContinuousUi() {
        DatePickerPopup picker;
        picker.setSchedule(timedRange(QDate(2026, 11, 20), QTime(9, 0),
                                      QDate(2026, 11, 21), QTime(10, 0)));
        popupAtTop(picker);
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        const QStringList texts = menuRowTexts(reminder);
        QCOMPARE(texts.size(), 8);  // 6 相对预设 + 结束时 + 自定义
        QCOMPARE(texts.at(6), QStringLiteral("结束时"));
        clickMenuRowByText(reminder, QStringLiteral("结束时"));
        ScheduleValue out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        ReminderSpec spec;
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QCOMPARE(spec.anchor, ReminderSpec::Anchor::End);
        // 触发时刻锚定结束端 2026-11-21 10:00。
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).date(), QDate(2026, 11, 21));
        QCOMPARE(toAppTime(out.reminders.first().triggerAt).time(), QTime(10, 0));
        // 无持续提醒 UI：菜单行与控件里都不出现“持续”。
        QVERIFY(!texts.join(QString()).contains(QStringLiteral("持续")));
        QVERIFY(reminder->findChildren<QCheckBox*>().isEmpty());
        picker.reject();
    }

    void reminderSummaryOrderCancelConfirm() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(9, 0)));
        popupAtTop(picker);
        QPushButton* entry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(entry);
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        // 先选30再选15，摘要仍按预设顺序显示，而不是点击顺序。
        QTest::mouseClick(menuRowByText(reminder, QStringLiteral("提前30分钟")), Qt::LeftButton);
        QCOMPARE(entry->text(), QStringLiteral("提前30分钟"));
        QTest::mouseClick(menuRowByText(reminder, QStringLiteral("提前15分钟")), Qt::LeftButton);
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟, 提前30分钟"));
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("取消")), Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QVERIFY(picker.schedule().reminders.isEmpty());  // 取消回滚本次全部勾选
        QCOMPARE(entry->text(), QStringLiteral("提醒"));

        // 取消后重新打开不残留未提交勾选；确定才保留到父草稿。
        reminder = openLayer(&picker, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        QVERIFY(!menuRowByText(reminder, QStringLiteral("提前15分钟"))->isChecked());
        QVERIFY(!menuRowByText(reminder, QStringLiteral("提前30分钟"))->isChecked());
        QTest::mouseClick(menuRowByText(reminder, QStringLiteral("提前15分钟")), Qt::LeftButton);
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QCOMPARE(picker.schedule().reminders.size(), 1);
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟"));
        const QString keptId = picker.schedule().reminders.first().id;
        QVERIFY(isUuid(keptId));

        reminder = openLayer(&picker, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        QTest::mouseClick(menuRowByText(reminder, QStringLiteral("提前30分钟")), Qt::LeftButton);
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟, 提前30分钟"));
        QCOMPARE(picker.schedule().reminders.size(), 2);
        QCOMPARE(picker.schedule().reminders.first().id, keptId);

        // Escape 与取消一致：移除本次新选的5分钟，已确定的15／30及UUID不变。
        reminder = openLayer(&picker, QStringLiteral("提醒"),
                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        QTest::mouseClick(menuRowByText(reminder, QStringLiteral("提前5分钟")), Qt::LeftButton);
        QCOMPARE(entry->text(), QStringLiteral("提前5分钟, 提前15分钟, 提前30分钟"));
        QTest::keyClick(reminder, Qt::Key_Escape);
        QTRY_VERIFY(!reminder->isVisible());
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟, 提前30分钟"));
        QCOMPARE(picker.schedule().reminders.size(), 2);
        QCOMPARE(picker.schedule().reminders.first().id, keptId);
        picker.reject();
    }

    void reminderDraftsSurviveTimedAllDayRoundTrip() {
        ScheduleValue initial = timedRange(QDate(2026, 11, 20), QTime(9, 0),
                                          QDate(2026, 11, 20), QTime(10, 0));
        TaskReminder timed;
        timed.id = newUuid();
        timed.ruleJson = reminderRuleText(relativeReminder(ReminderSpec::Unit::Minutes, 15));
        initial.reminders.append(timed);
        DatePickerPopup picker;
        picker.setSchedule(initial);
        popupAtTop(picker);
        QPushButton* entry = findEntry(&picker, QStringLiteral("提醒"));
        QCheckBox* allDay = picker.findChild<QCheckBox*>(QStringLiteral("DatePickerAllDayCheck"));
        QVERIFY(entry && allDay);
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟"));

        QTest::mouseClick(allDay, Qt::LeftButton);
        QVERIFY(allDay->isChecked());
        QVERIFY(picker.schedule().reminders.isEmpty());
        QCOMPARE(entry->text(), QStringLiteral("提醒"));
        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        // 先选提前3天再选当天，确定后仍按菜单顺序；默认09:00在父摘要省略。
        clickMenuRowByText(reminder, QStringLiteral("提前3天"));
        clickMenuRowByText(reminder, QStringLiteral("当天"));
        QCOMPARE(entry->text(), QStringLiteral("当天, 提前3天"));
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        ScheduleValue out = picker.schedule();
        QCOMPARE(out.reminders.size(), 2);
        ReminderSpec spec;
        for (const TaskReminder& row : out.reminders) {
            QVERIFY(reminderFromRuleText(row.ruleJson, &spec));
            QVERIFY(spec.allDay);
            QCOMPARE(spec.localTime, QTime(9, 0));
        }

        // 切回定时恢复最初15分钟；再切全天则恢复刚确定的当天／提前3天。
        QTest::mouseClick(allDay, Qt::LeftButton);
        QVERIFY(!allDay->isChecked());
        out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        QCOMPARE(out.reminders.first().id, timed.id);
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟"));
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QVERIFY(!spec.allDay);
        QCOMPARE(spec.offset, 15);

        QTest::mouseClick(allDay, Qt::LeftButton);
        out = picker.schedule();
        QCOMPARE(out.reminders.size(), 2);
        QCOMPARE(entry->text(), QStringLiteral("当天, 提前3天"));
        for (const TaskReminder& row : out.reminders) {
            QVERIFY(reminderFromRuleText(row.ruleJson, &spec));
            QVERIFY(spec.allDay);
        }
        picker.reject();
    }

    void singleDateReminderDraftsSurviveTimeToggle() {
        ScheduleValue initial = singleTimed(QDate(2026, 11, 20), QTime(9, 0));
        TaskReminder timed;
        timed.id = newUuid();
        timed.ruleJson = reminderRuleText(relativeReminder(ReminderSpec::Unit::Minutes, 15));
        initial.reminders.append(timed);
        DatePickerPopup picker;
        picker.setSchedule(initial);
        popupAtTop(picker);
        QPushButton* entry = findEntry(&picker, QStringLiteral("提醒"));
        QVERIFY(entry);
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟"));

        // 清空时间输入并回车 = 移除时刻（取代旧 DatePickerNoTime 按钮）。
        auto* timeEntry = findEntry(&picker, QStringLiteral("时间"));
        QVERIFY(timeEntry);
        auto* expansion = openTimeExpansion(&picker, timeEntry);
        QVERIFY(expansion);
        auto* edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(edit);
        edit->setText(QString());
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(picker.schedule().reminders.isEmpty());
        QCOMPARE(entry->text(), QStringLiteral("提醒"));

        AnchoredListPopup* reminder = openLayer(&picker, QStringLiteral("提醒"),
                                                QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(reminder);
        clickMenuRowByText(reminder, QStringLiteral("当天"));
        QTest::mouseClick(visibleButtonWithText(reminder, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!reminder->isVisible());
        QCOMPARE(entry->text(), QStringLiteral("当天"));

        // 普通单日重新设置时刻后，恢复定时提醒；再次取消时刻则恢复全天提醒。
        expansion = openTimeExpansion(&picker, timeEntry);
        QVERIFY(expansion);
        auto* list = listOf(expansion);
        QVERIFY(list);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(18)).center());  // 09:00
        QTRY_VERIFY(!expansion->isVisible());
        QCOMPARE(entry->text(), QStringLiteral("提前15分钟"));
        QCOMPARE(picker.schedule().reminders.size(), 1);
        QCOMPARE(picker.schedule().reminders.first().id, timed.id);

        expansion = openTimeExpansion(&picker, timeEntry);
        QVERIFY(expansion);
        edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(edit);
        edit->setText(QString());
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QCOMPARE(entry->text(), QStringLiteral("当天"));
        ScheduleValue out = picker.schedule();
        QCOMPARE(out.reminders.size(), 1);
        ReminderSpec spec;
        QVERIFY(reminderFromRuleText(out.reminders.first().ruleJson, &spec));
        QVERIFY(spec.allDay);
        QCOMPARE(spec.dayOffset, 0);
        picker.reject();
    }

    void rangeTimeInputsAcceptExactMinutes() {
        DatePickerPopup picker;
        picker.setSchedule(timedRange(QDate(2026, 11, 20), QTime(9, 0),
                                      QDate(2026, 11, 20), QTime(10, 0)));
        popupAtTop(picker);
        auto* start = rangeTimeButton(&picker, QStringLiteral("开始时间"));
        auto* end = rangeTimeButton(&picker, QStringLiteral("结束时间"));
        QVERIFY(start && end);
        auto* expansion = openTimeExpansion(&picker, start);
        QVERIFY(expansion);
        auto* edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(edit);
        edit->setText(QStringLiteral("9:7"));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QCOMPARE(start->text(), QStringLiteral("09:07"));
        QCOMPARE(toAppTime(picker.schedule().startAt).time(), QTime(9, 7));

        expansion = openTimeExpansion(&picker, end);
        QVERIFY(expansion);
        edit = expansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(edit);
        edit->setText(QStringLiteral("25:99"));
        QTest::keyClick(edit, Qt::Key_Return);
        QVERIFY(expansion->isVisible());
        QVERIFY(edit->property("invalid").toBool());
        edit->setText(QStringLiteral("10:3"));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(!expansion->isVisible());
        QCOMPARE(end->text(), QStringLiteral("10:03"));
        ScheduleValue out = picker.schedule();
        QCOMPARE(toAppTime(out.startAt).time(), QTime(9, 7));
        QCOMPARE(toAppTime(out.dueAt).time(), QTime(10, 3));
        QVERIFY(out.hasScheduleRange);
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));
    }

    void quickIconsAndTodayBackgroundMatchWeb() {
        DatePickerPopup picker;
        const QDate current = toAppTime(appNow()).date();
        picker.setSchedule(singleAllDay(current.addDays(1)));
        popupAtTop(picker);
        QWidget* card = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"));
        QVERIFY(card);
        for (int i = 0; i < 4; ++i) {
            QToolButton* quick = quickButton(&picker, i);
            QVERIFY(quick);
            QCOMPARE(quick->size(), QSize(26, 26));
            QCOMPARE(quick->iconSize(), QSize(26, 26));
            QVERIFY(!quick->icon().isNull());
        }
        const QPoint first = quickButton(&picker, 0)->mapTo(card, QPoint());
        const QPoint last = quickButton(&picker, 3)->mapTo(card, QPoint());
        QCOMPARE(first.x(), 22);
        QCOMPARE(card->width() - last.x() - 26, 22);
        QCOMPARE(first.y(), last.y());

        QToolButton* todayButton = dayButton(&picker, current);
        QToolButton* selected = dayButton(&picker, current.addDays(1));
        QVERIFY(todayButton && selected);
        QVERIFY(todayButton->property("today").toBool());
        QVERIFY(!todayButton->isChecked());
        QVERIFY(selected->isChecked());
        QCOMPARE(renderedPixel(todayButton, QPoint(3, 15)),
                 ThemeManager::instance().color(ds::ColorToken::PrimarySoft));
        QCOMPARE(renderedPixel(selected, QPoint(3, 15)),
                 ThemeManager::instance().color(ds::ColorToken::Primary));
        picker.reject();
    }

    void cardRangeAndFooterBordersStayBalanced() {
        DatePickerPopup picker;
        picker.setSchedule(singleTimed(QDate(2026, 11, 20), QTime(9, 0)));
        popupAtTop(picker);
        QWidget* card = picker.findChild<QWidget*>(QStringLiteral("DatePickerCard"));
        QVERIFY(card);
        const QColor surface = ThemeManager::instance().color(ds::ColorToken::Surface);
        QCOMPARE(renderedPixel(card, QPoint(0, card->height() / 2)), surface);
        QCOMPARE(renderedPixel(card, QPoint(card->width() - 1, card->height() / 2)), surface);
        QPushButton* clear = card->findChild<QPushButton*>(QStringLiteral("DatePickerClear"));
        QPushButton* confirm = card->findChild<QPushButton*>(QStringLiteral("DatePickerConfirm"));
        QVERIFY(clear && confirm);
        QCOMPARE(clear->height(), 30);
        QCOMPARE(confirm->height(), 30);
        QCOMPARE(clear->width(), confirm->width());
        const QColor border = ThemeManager::instance().color(ds::ColorToken::Border);
        QCOMPARE(renderedPixel(clear, QPoint(0, 15)), border);
        QCOMPARE(renderedPixel(clear, QPoint(clear->width() - 1, 15)), border);
        const QColor primary = ThemeManager::instance().color(ds::ColorToken::Primary);
        QCOMPARE(renderedPixel(confirm, QPoint(0, 15)), primary);
        QCOMPARE(renderedPixel(confirm, QPoint(confirm->width() - 1, 15)), primary);

        QTest::mouseClick(segmentButton(&picker, QStringLiteral("时间段")), Qt::LeftButton);
        QApplication::processEvents();
        QPushButton* start = rangeTimeButton(&picker, QStringLiteral("开始时间"));
        QVERIFY(start && start->isVisible());
        QCOMPARE(start->size(), QSize(95, 28));
        QCOMPARE(renderedPixel(start, QPoint(0, 14)), border);
        QCOMPARE(renderedPixel(start, QPoint(start->width() - 1, 14)), border);
        QCOMPARE(renderedPixel(start, QPoint(start->width() / 2, 0)), border);
        QCOMPARE(renderedPixel(start, QPoint(start->width() / 2, start->height() - 1)), border);
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 重复（RecurrenceSpec）：预设行
    // ------------------------------------------------------------------
    void recurrencePresetsApplyTypedSpecs() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));  // 周三
        popupAtTop(picker);
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        // 未重复时没有“不重复”行。
        QCOMPARE(menuRowTexts(recurrence),
                 QStringList({QStringLiteral("每天"), QStringLiteral("每周 (周三)"),
                              QStringLiteral("每月 (18日)"), QStringLiteral("每年 (11月18日)"),
                              QStringLiteral("工作日"), QStringLiteral("自定义")}));

        // 预设行点击即应用草稿并收起子层；父弹层保持打开（仍需父层确认）。
        clickMenuRowByText(recurrence, QStringLiteral("每天"));
        QTRY_VERIFY(!recurrence->isVisible());
        QVERIFY(picker.isVisible());
        ScheduleValue out = picker.schedule();
        QVERIFY(out.repeating);
        QCOMPARE(out.recurrence.kind, RecurrenceSpec::Kind::Gregorian);
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Daily);
        QVERIFY(out.recurrence.interval == 1);  // typed JSON：不再是 RRULE 文本

        recurrence = openLayer(&picker, QStringLiteral("重复"),
                               QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        // 重复中：首行“不重复”，“每天”为当前选中态（右侧对勾）。
        QCOMPARE(menuRowTexts(recurrence).first(), QStringLiteral("不重复"));
        QVERIFY(!menuRowByText(recurrence, QStringLiteral("不重复"))->isChecked());
        QVERIFY(menuRowByText(recurrence, QStringLiteral("每天"))->isChecked());
        // 工作日 = Weekly + weekdays 1..5。
        clickMenuRowByText(recurrence, QStringLiteral("工作日"));
        QTRY_VERIFY(!recurrence->isVisible());
        out = picker.schedule();
        QVERIFY(out.repeating);
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Weekly);
        QCOMPARE(out.recurrence.weekdays, QVector<int>({1, 2, 3, 4, 5}));
        // 每年：锚点日 11-18。
        recurrence = openLayer(&picker, QStringLiteral("重复"),
                               QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("每年 (11月18日)"));
        QTRY_VERIFY(!recurrence->isVisible());
        out = picker.schedule();
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Yearly);
        QCOMPARE(out.recurrence.yearMode, RecurrenceSpec::YearMode::Date);
        QCOMPARE(out.recurrence.month, 11);
        QVERIFY(out.recurrence.monthDays.contains(18));
        // 不重复。
        recurrence = openLayer(&picker, QStringLiteral("重复"),
                               QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("不重复"));
        QTRY_VERIFY(!recurrence->isVisible());
        QVERIFY(!picker.schedule().repeating);
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 自定义重复：公历表单、未编辑保留、取消回滚、完成基准限制
    // ------------------------------------------------------------------
    void customRecurrenceGregorianFormsAndRollback() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));  // 周三
        popupAtTop(picker);
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        // 同一锚定卡片内切换页面：预设页隐藏、自定义页可见。
        QVERIFY(recurrence->findChild<QWidget*>(
                    QStringLiteral("DatePickerCustomRecurrencePage"))->isVisible());
        QVERIFY(!recurrence->findChild<QWidget*>(
                     QStringLiteral("DatePickerRecurrenceMenu"))->isVisible());
        auto* basis = comboByAccessible(recurrence, QStringLiteral("重复基准"));
        auto* freq = comboByAccessible(recurrence, QStringLiteral("重复频率"));
        auto* interval = spinByAccessible(recurrence, QStringLiteral("重复间隔"));
        QVERIFY(basis && freq && interval);
        // 公历每周、间隔 2、按到期日（默认勾选锚点星期三）。
        freq->setCurrentIndex(1);   // 周
        interval->setValue(2);
        basis->setCurrentIndex(0);  // 按到期日期
        QTest::mouseClick(visibleButtonWithText(recurrence, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!recurrence->isVisible());
        QVERIFY(picker.isVisible());
        const ScheduleValue out = picker.schedule();
        QVERIFY(out.repeating);
        QCOMPARE(out.recurrence.kind, RecurrenceSpec::Kind::Gregorian);
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Weekly);
        QCOMPARE(out.recurrence.interval, 2);
        QCOMPARE(out.recurrence.basis, RecurrenceSpec::Basis::DueDate);
        QCOMPARE(out.recurrence.weekdays, QVector<int>({3}));
        // typed JSON 序列化可往返（领域严格 v1 JSON）。
        RecurrenceSpec parsed;
        QString error;
        QVERIFY(recurrenceFromRuleText(recurrenceRuleText(out.recurrence), &parsed, &error));
        QCOMPARE(parsed.interval, 2);
        QCOMPARE(parsed.weekdays, QVector<int>({3}));

        // 取消回滚：改动不落地。
        recurrence = openLayer(&picker, QStringLiteral("重复"),
                               QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        interval = spinByAccessible(recurrence, QStringLiteral("重复间隔"));
        QVERIFY(interval);
        QCOMPARE(interval->value(), 2);  // 重新打开时回显当前草稿
        spinByAccessible(recurrence, QStringLiteral("重复间隔"))->setValue(5);
        QTest::mouseClick(visibleButtonWithText(recurrence, QStringLiteral("取消")),
                          Qt::LeftButton);
        QTRY_VERIFY(!recurrence->isVisible());
        ScheduleValue after = picker.schedule();
        QCOMPARE(after.recurrence.frequency, RecurrenceSpec::Frequency::Weekly);
        QCOMPARE(after.recurrence.interval, 2);

        // 按完成日期 + 每日间隔 3：完成基准只保留普通日/周/月间隔。
        recurrence = openLayer(&picker, QStringLiteral("重复"),
                               QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        basis = comboByAccessible(recurrence, QStringLiteral("重复基准"));
        freq = comboByAccessible(recurrence, QStringLiteral("重复频率"));
        interval = spinByAccessible(recurrence, QStringLiteral("重复间隔"));
        QVERIFY(basis && freq && interval);
        basis->setCurrentIndex(1);  // 按完成日期
        // 年 / 农历月 / 农历年选项被禁用；星期与固定月日工作区隐藏。
        auto* model = qobject_cast<QStandardItemModel*>(freq->model());
        QVERIFY(model);
        QVERIFY(model->item(2)->isEnabled());   // 月仍可选
        QVERIFY(!model->item(3)->isEnabled());
        QVERIFY(!model->item(4)->isEnabled());  // 农历月禁用
        QVERIFY(!model->item(5)->isEnabled());  // 农历年禁用
        auto* customPage = recurrence->findChild<QWidget*>(
            QStringLiteral("DatePickerCustomRecurrencePage"));
        QVERIFY(customPage);
        for (auto* toggle : customPage->findChildren<QToolButton*>(
                 QStringLiteral("DatePickerToggle")))
            QVERIFY(!toggle->isVisible());  // 星期/固定月日工作区整体隐藏
        freq->setCurrentIndex(0);  // 天
        interval->setValue(3);
        QTest::mouseClick(visibleButtonWithText(recurrence, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!recurrence->isVisible());
        after = picker.schedule();
        QVERIFY(after.repeating);
        QCOMPARE(after.recurrence.basis, RecurrenceSpec::Basis::CompletionDate);
        QCOMPARE(after.recurrence.frequency, RecurrenceSpec::Frequency::Daily);
        QCOMPARE(after.recurrence.interval, 3);
        QCOMPARE(findEntry(&picker, QStringLiteral("重复"))->text(),
                 QStringLiteral("完成后3天"));
        // 自定义重复只保留数值控件的内置编辑框，没有原始 RRULE/结束日期输入。
        for (QLineEdit* edit : recurrence->findChildren<QLineEdit*>())
            QVERIFY(qobject_cast<QAbstractSpinBox*>(edit->parentWidget()));
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 自定义重复：各频率、月末、农历、自选日期多选、校验错误
    // ------------------------------------------------------------------
    void customRecurrenceFrequenciesAndExplicitDates() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));  // 周三
        popupAtTop(picker);
        auto applyCustom = [&](const std::function<void(AnchoredListPopup*)>& adjust) {
            AnchoredListPopup* layer = openLayer(&picker, QStringLiteral("重复"),
                                                 QStringLiteral("DatePickerRecurrenceLayer"));
            QVERIFY2(layer, "recurrence layer");
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            adjust(layer);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QTRY_VERIFY(!layer->isVisible());
        };
        ScheduleValue out;

        // 每隔 3 天。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(0);
            spinByAccessible(layer, QStringLiteral("重复间隔"))->setValue(3);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Daily);
        QCOMPARE(out.recurrence.interval, 3);

        // 每周多星期（一/三/五）。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(1);
            for (int day = 1; day <= 7; ++day)
                if (auto* toggle = weekdayToggle(layer, day)) toggle->setChecked(false);
            weekdayToggle(layer, 1)->setChecked(true);
            weekdayToggle(layer, 3)->setChecked(true);
            weekdayToggle(layer, 5)->setChecked(true);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Weekly);
        QCOMPARE(out.recurrence.weekdays, QVector<int>({1, 3, 5}));

        // 每月按日期：1、15 号 + 最后一天 → monthDays 排序输出（-1 在前）。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(2);
            comboByAccessible(layer, QStringLiteral("每月重复方式"))->setCurrentIndex(0);
            setAllMonthDayToggles(layer, false);
            monthDayToggle(layer, 1)->setChecked(true);
            monthDayToggle(layer, 15)->setChecked(true);
            for (auto* toggle : layer->findChildren<QToolButton*>(
                     QStringLiteral("DatePickerToggle")))
                if (toggle->text() == QStringLiteral("最后一天")) toggle->setChecked(true);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Monthly);
        QCOMPARE(out.recurrence.monthMode, RecurrenceSpec::MonthMode::Days);
        QCOMPARE(out.recurrence.monthDays, QVector<int>({-1, 1, 15}));

        // 每月第 2 个周三。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(2);
            comboByAccessible(layer, QStringLiteral("每月重复方式"))->setCurrentIndex(1);
            comboByAccessible(layer, QStringLiteral("第几个"))->setCurrentIndex(1);
            comboByAccessible(layer, QStringLiteral("星期几"))->setCurrentIndex(2);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.monthMode, RecurrenceSpec::MonthMode::Weekday);
        QCOMPARE(out.recurrence.ordinal, 2);
        QCOMPARE(out.recurrence.ordinalWeekday, 3);

        // 每月最后一个工作日。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(2);
            comboByAccessible(layer, QStringLiteral("每月重复方式"))->setCurrentIndex(2);
            comboByAccessible(layer, QStringLiteral("每月工作日"))->setCurrentIndex(1);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.monthMode, RecurrenceSpec::MonthMode::Workday);
        QVERIFY(!out.recurrence.firstWorkday);

        // 每年 5 月 20 日。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(3);
            comboByAccessible(layer, QStringLiteral("每年重复方式"))->setCurrentIndex(0);
            comboByAccessible(layer, QStringLiteral("月份"))->setCurrentIndex(4);
            spinByAccessible(layer, QStringLiteral("日期"))->setValue(20);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Yearly);
        QCOMPARE(out.recurrence.yearMode, RecurrenceSpec::YearMode::Date);
        QCOMPARE(out.recurrence.month, 5);
        QCOMPARE(out.recurrence.monthDays, QVector<int>({20}));

        // 每年按日期选“最后一天”：年度日期 spin 0 映射月末 -1。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(3);
            comboByAccessible(layer, QStringLiteral("每年重复方式"))->setCurrentIndex(0);
            auto* daySpin = spinByAccessible(layer, QStringLiteral("日期"));
            daySpin->setValue(0);
            QCOMPARE(daySpin->text(), QStringLiteral("最后一天"));
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.monthDays, QVector<int>({-1}));

        // 每年第 3 个周五。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(3);
            comboByAccessible(layer, QStringLiteral("每年重复方式"))->setCurrentIndex(1);
            comboByAccessible(layer, QStringLiteral("第几个"))->setCurrentIndex(2);
            comboByAccessible(layer, QStringLiteral("星期几"))->setCurrentIndex(4);
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.yearMode, RecurrenceSpec::YearMode::Weekday);
        QCOMPARE(out.recurrence.ordinal, 3);
        QCOMPARE(out.recurrence.ordinalWeekday, 5);

        // 农历月：农历工作区（锚点说明 + 闰月重复）、间隔锁定 1；勾闰月后保留。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(4);
            auto* leap = layer->findChild<QCheckBox*>(QStringLiteral("DatePickerCheck"));
            QVERIFY(leap && leap->isVisible());
            QCOMPARE(leap->accessibleName(), QStringLiteral("闰月重复"));
            bool anchorShown = false;
            for (auto* label : layer->findChildren<QLabel*>(QStringLiteral("DatePickerHint")))
                if (label->isVisible() && label->text().startsWith(QStringLiteral("按农历")))
                    anchorShown = true;
            QVERIFY(anchorShown);
            QVERIFY(!spinByAccessible(layer, QStringLiteral("重复间隔"))->isEnabled());
            QTest::mouseClick(leap, Qt::LeftButton, Qt::NoModifier,
                              QPoint(8, leap->height() / 2));  // 命中指示器
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.kind, RecurrenceSpec::Kind::LunarMonthly);
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Monthly);
        QCOMPARE(out.recurrence.interval, 1);
        QVERIFY(out.recurrence.lunarLeapMonthRepeat);

        // 农历年（闰月沿用上次的勾选状态由重开载入，这里显式取消）。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(5);
            auto* leap = layer->findChild<QCheckBox*>(QStringLiteral("DatePickerCheck"));
            QVERIFY(leap && leap->isVisible() && leap->isChecked());  // 由农历月草稿载入
            QTest::keyClick(leap, Qt::Key_Space);  // 键盘取消勾选
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.kind, RecurrenceSpec::Kind::LunarYearly);
        QCOMPARE(out.recurrence.frequency, RecurrenceSpec::Frequency::Yearly);
        QVERIFY(!out.recurrence.lunarLeapMonthRepeat);

        // 自选日期：多选日历，点 3 个再去掉 1 个 → 升序去重。
        applyCustom([&](AnchoredListPopup* layer) {
            comboByAccessible(layer, QStringLiteral("重复基准"))->setCurrentIndex(2);
            auto* calendar = layer->findChild<DateCalendarPanel*>(
                QStringLiteral("DatePickerExplicitDatesCalendar"));
            QVERIFY(calendar && calendar->isVisible());
            QVERIFY(calendar->multiSelectEnabled());
            QVERIFY(!layer->findChild<QWidget*>(QStringLiteral("DatePickerToggle"))
                     || !weekdayToggle(layer, 1)->isVisible());
            for (const QDate date : {QDate(2026, 11, 27), QDate(2026, 11, 3),
                                     QDate(2026, 11, 12)}) {
                auto* day = dayButton(calendar, date);
                QVERIFY(day && day->isVisible());
                QTest::mouseClick(day, Qt::LeftButton);
            }
            QTest::mouseClick(dayButton(calendar, QDate(2026, 11, 12)), Qt::LeftButton);
            QTest::mouseClick(dayButton(calendar, QDate(2026, 11, 18)), Qt::LeftButton);
            QCOMPARE(calendar->selectedDates(),
                     QVector<QDate>({QDate(2026, 11, 3), QDate(2026, 11, 27)}));
        });
        out = picker.schedule();
        QCOMPARE(out.recurrence.kind, RecurrenceSpec::Kind::ExplicitDates);
        QCOMPARE(out.recurrence.explicitDates,
                 QVector<QDate>({QDate(2026, 11, 3), QDate(2026, 11, 27)}));
        QCOMPARE(findEntry(&picker, QStringLiteral("重复"))->text(),
                 QStringLiteral("按自选日期"));

        // 自选为空：行内报错、保持打开、草稿不落地。
        {
            AnchoredListPopup* layer = openLayer(&picker, QStringLiteral("重复"),
                                                 QStringLiteral("DatePickerRecurrenceLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            comboByAccessible(layer, QStringLiteral("重复基准"))->setCurrentIndex(2);
            auto* calendar = layer->findChild<DateCalendarPanel*>(
                QStringLiteral("DatePickerExplicitDatesCalendar"));
            QVERIFY(calendar);
            QTest::mouseClick(dayButton(calendar, QDate(2026, 11, 3)), Qt::LeftButton);
            QTest::mouseClick(dayButton(calendar, QDate(2026, 11, 27)), Qt::LeftButton);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QVERIFY(layer->isVisible());
            QVERIFY(visibleErrorIn(layer, QStringLiteral("请至少选择一个重复日期")));
            QVERIFY(picker.schedule().recurrence.kind == RecurrenceSpec::Kind::ExplicitDates);
            QTest::keyClick(layer, Qt::Key_Escape);
            QTRY_VERIFY(!layer->isVisible());
        }

        // 每周无星期：行内报错。
        {
            AnchoredListPopup* layer = openLayer(&picker, QStringLiteral("重复"),
                                                 QStringLiteral("DatePickerRecurrenceLayer"));
            QVERIFY(layer);
            clickMenuRowByText(layer, QStringLiteral("自定义"));
            comboByAccessible(layer, QStringLiteral("重复基准"))->setCurrentIndex(0);
            comboByAccessible(layer, QStringLiteral("重复频率"))->setCurrentIndex(1);
            for (int day = 1; day <= 7; ++day)
                if (auto* toggle = weekdayToggle(layer, day)) toggle->setChecked(false);
            QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")),
                              Qt::LeftButton);
            QVERIFY(layer->isVisible());
            QVERIFY(visibleErrorIn(layer, QStringLiteral("请至少选择一个星期")));
            // 无节假日/记忆法空控件。
            const QStringList forbidden = {QStringLiteral("节假日"), QStringLiteral("记忆"),
                                           QStringLiteral("艾宾浩斯")};
            for (auto* label : layer->findChildren<QLabel*>())
                for (const QString& word : forbidden)
                    QVERIFY(!label->text().contains(word));
            for (auto* button : layer->findChildren<QAbstractButton*>())
                for (const QString& word : forbidden)
                    QVERIFY(!button->text().contains(word));
            for (auto* combo : layer->findChildren<QComboBox*>())
                for (int i = 0; i < combo->count(); ++i)
                    for (const QString& word : forbidden)
                        QVERIFY(!combo->itemText(i).contains(word));
            QTest::keyClick(layer, Qt::Key_Escape);
            QTRY_VERIFY(!layer->isVisible());
        }
        picker.reject();
    }

    // 打开已有规则不做任何修改直接“确定”：原样保留未编辑字段
    // （weekStart、农历闰月、月末等），不被默认值覆盖。
    void customRecurrenceReconfirmPreservesUneditedSpec() {
        ScheduleValue initial = singleAllDay(QDate(2026, 11, 18));
        initial.repeating = true;
        initial.recurrence.kind = RecurrenceSpec::Kind::LunarMonthly;
        initial.recurrence.frequency = RecurrenceSpec::Frequency::Monthly;
        initial.recurrence.weekStart = Qt::Sunday;           // 非默认周一
        initial.recurrence.lunarLeapMonthRepeat = true;
        QVERIFY(isValidRecurrence(initial.recurrence));
        DatePickerPopup picker;
        picker.setSchedule(initial);
        popupAtTop(picker);
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("自定义"));
        QCOMPARE(comboByAccessible(recurrence, QStringLiteral("重复频率"))->currentText(),
                 QStringLiteral("农历月"));
        QVERIFY(recurrence->findChild<QCheckBox*>(QStringLiteral("DatePickerCheck"))->isChecked());
        // 未做任何编辑，直接确定。
        QTest::mouseClick(visibleButtonWithText(recurrence, QStringLiteral("确定")),
                          Qt::LeftButton);
        QTRY_VERIFY(!recurrence->isVisible());
        const RecurrenceSpec out = picker.schedule().recurrence;
        QCOMPARE(out.kind, RecurrenceSpec::Kind::LunarMonthly);
        QCOMPARE(out.weekStart, Qt::Sunday);
        QVERIFY(out.lunarLeapMonthRepeat);
        QCOMPARE(recurrenceRuleText(out), recurrenceRuleText(initial.recurrence));
        picker.reject();
    }

    // ------------------------------------------------------------------
    // 结束重复：EndLayer 菜单 + EndCalendar / EndCount
    // ------------------------------------------------------------------
    void recurrenceEndLayerCalendarAndCount() {
        DatePickerPopup picker;
        picker.setSchedule(singleAllDay(QDate(2026, 11, 18)));
        popupAtTop(picker);
        AnchoredListPopup* recurrence = openLayer(&picker, QStringLiteral("重复"),
                                                  QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(recurrence);
        clickMenuRowByText(recurrence, QStringLiteral("每天"));
        QTRY_VERIFY(!recurrence->isVisible());

        // 应用预设后父卡片出现“结束重复”入口（默认一直重复）。
        auto* endEntry = findEntry(&picker, QStringLiteral("结束重复"));
        QVERIFY(endEntry && endEntry->isVisible());
        QCOMPARE(endEntry->text(), QStringLiteral("一直重复"));

        QTest::mouseClick(endEntry, Qt::LeftButton);
        QApplication::processEvents();
        auto* endLayer = picker.findChild<AnchoredListPopup*>(
            QStringLiteral("DatePickerRecurrenceEndLayer"));
        QVERIFY(endLayer && endLayer->isVisible());
        QVERIFY(endModeRow(endLayer, 0)->isChecked());       // 一直重复
        QVERIFY(!endModeRow(endLayer, 1)->isChecked());
        QVERIFY(!endModeRow(endLayer, 2)->isChecked());

        // 按日期结束：默认锚点+1 个月，选日后写入 endDate 并收起内嵌日历。
        QTest::mouseClick(endModeRow(endLayer, 1), Qt::LeftButton);
        QTRY_VERIFY(!endLayer->isVisible());
        auto* calendar = picker.findChild<DateCalendarPanel*>(
            QStringLiteral("DatePickerRecurrenceEndCalendar"));
        QVERIFY(calendar && calendar->isVisible());
        const QDate month = calendar->visibleMonth();
        const QDate target(month.year(), month.month(), 10);
        auto* day = dayButton(calendar, target);
        QVERIFY(day && day->isVisible());
        QTest::mouseClick(day, Qt::LeftButton);
        QTRY_VERIFY(!calendar->isVisible());
        QVERIFY(picker.isVisible());
        QCOMPARE(endEntry->text(), target.toString(QStringLiteral("直到yyyy/M/d")));
        QCOMPARE(picker.schedule().recurrence.endMode, RecurrenceSpec::EndMode::Date);
        QCOMPARE(picker.schedule().recurrence.endDate, target);

        // 切回一直重复：endDate 被清掉，避免残留字段让规则无效。
        QTest::mouseClick(endEntry, Qt::LeftButton);
        QApplication::processEvents();
        endLayer = picker.findChild<AnchoredListPopup*>(
            QStringLiteral("DatePickerRecurrenceEndLayer"));
        QVERIFY(endLayer && endLayer->isVisible());
        QVERIFY(endModeRow(endLayer, 1)->isChecked());
        QTest::mouseClick(endModeRow(endLayer, 0), Qt::LeftButton);
        QTRY_VERIFY(!endLayer->isVisible());
        QCOMPARE(picker.schedule().recurrence.endMode, RecurrenceSpec::EndMode::Never);
        QVERIFY(!picker.schedule().recurrence.endDate.isValid());
        QCOMPARE(endEntry->text(), QStringLiteral("一直重复"));

        // 按次数结束：计数编辑器，默认 2；实时更新入口文案；确定收起。
        QTest::mouseClick(endEntry, Qt::LeftButton);
        QApplication::processEvents();
        endLayer = picker.findChild<AnchoredListPopup*>(
            QStringLiteral("DatePickerRecurrenceEndLayer"));
        QVERIFY(endLayer && endLayer->isVisible());
        QTest::mouseClick(endModeRow(endLayer, 2), Qt::LeftButton);
        QTRY_VERIFY(!endLayer->isVisible());
        auto* countEditor = picker.findChild<QWidget*>(
            QStringLiteral("DatePickerRecurrenceEndCount"));
        QVERIFY(countEditor && countEditor->isVisible());
        auto* countSpin = spinByAccessible(countEditor, QStringLiteral("结束次数"));
        auto* countDone = countEditor->findChild<QPushButton*>(
            QStringLiteral("DatePickerSubConfirm"));
        QVERIFY(countSpin && countDone);
        QCOMPARE(countSpin->value(), 2);
        countSpin->setValue(5);
        QCOMPARE(endEntry->text(), QStringLiteral("重复5次后结束"));
        QTest::mouseClick(countDone, Qt::LeftButton);
        QTRY_VERIFY(!countEditor->isVisible());
        QCOMPARE(picker.schedule().recurrence.endMode, RecurrenceSpec::EndMode::Count);
        QCOMPARE(picker.schedule().recurrence.endCount, 5);
        QVERIFY(!picker.schedule().recurrence.endDate.isValid());  // 切 Count 同样清日期
        picker.accept();
        QCOMPARE(picker.result(), static_cast<int>(QDialog::Accepted));

        // 自选日期规则：结束层“按次数”行禁用；自定义页 gather 明确拒绝 Count。
        DatePickerPopup explicitPicker;
        ScheduleValue initial = singleAllDay(QDate(2026, 11, 18));
        explicitPicker.setSchedule(initial);
        popupAtTop(explicitPicker);
        AnchoredListPopup* layer = openLayer(&explicitPicker, QStringLiteral("重复"),
                                             QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(layer);
        clickMenuRowByText(layer, QStringLiteral("自定义"));
        comboByAccessible(layer, QStringLiteral("重复基准"))->setCurrentIndex(2);
        auto* explicitCalendar = layer->findChild<DateCalendarPanel*>(
            QStringLiteral("DatePickerExplicitDatesCalendar"));
        QVERIFY(explicitCalendar);
        QTest::mouseClick(dayButton(explicitCalendar, QDate(2026, 11, 20)), Qt::LeftButton);
        QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")), Qt::LeftButton);
        QTRY_VERIFY(!layer->isVisible());
        endLayer = openLayer(&explicitPicker, QStringLiteral("结束重复"),
                              QStringLiteral("DatePickerRecurrenceEndLayer"));
        QVERIFY(endLayer && endLayer->isVisible());
        QVERIFY(!endModeRow(endLayer, 2)->isEnabled());  // 自选日期禁用按次数
        QTest::keyClick(endLayer, Qt::Key_Escape);       // Escape 只关子层
        QTRY_VERIFY(!endLayer->isVisible());
        QVERIFY(explicitPicker.isVisible());

        // 已是 Count 的规则切到自选日期：gather 明确报错，不静默清 Count。
        DatePickerPopup countPicker;
        countPicker.setSchedule(initial);
        popupAtTop(countPicker);
        layer = openLayer(&countPicker, QStringLiteral("重复"),
                          QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(layer);
        clickMenuRowByText(layer, QStringLiteral("每天"));
        QTRY_VERIFY(!layer->isVisible());
        QTest::mouseClick(findEntry(&countPicker, QStringLiteral("结束重复")), Qt::LeftButton);
        QApplication::processEvents();
        endLayer = countPicker.findChild<AnchoredListPopup*>(
            QStringLiteral("DatePickerRecurrenceEndLayer"));
        QVERIFY(endLayer && endLayer->isVisible());
        QTest::mouseClick(endModeRow(endLayer, 2), Qt::LeftButton);
        QTRY_VERIFY(!endLayer->isVisible());
        auto* countEditor2 = countPicker.findChild<QWidget*>(
            QStringLiteral("DatePickerRecurrenceEndCount"));
        QVERIFY(countEditor2 && countEditor2->isVisible());
        QTest::mouseClick(countEditor2->findChild<QPushButton*>(
                              QStringLiteral("DatePickerSubConfirm")), Qt::LeftButton);
        QTRY_VERIFY(!countEditor2->isVisible());
        layer = openLayer(&countPicker, QStringLiteral("重复"),
                          QStringLiteral("DatePickerRecurrenceLayer"));
        QVERIFY(layer);
        clickMenuRowByText(layer, QStringLiteral("自定义"));
        comboByAccessible(layer, QStringLiteral("重复基准"))->setCurrentIndex(2);
        auto* pickCalendar = layer->findChild<DateCalendarPanel*>(
            QStringLiteral("DatePickerExplicitDatesCalendar"));
        QVERIFY(pickCalendar);
        QTest::mouseClick(dayButton(pickCalendar, QDate(2026, 11, 20)), Qt::LeftButton);
        QTest::mouseClick(visibleButtonWithText(layer, QStringLiteral("确定")), Qt::LeftButton);
        QVERIFY(layer->isVisible());  // 明确拒绝，保持打开
        QVERIFY(visibleErrorIn(layer, QStringLiteral("自选日期不支持按次数结束")));
        QCOMPARE(countPicker.schedule().recurrence.endMode, RecurrenceSpec::EndMode::Count);
        QTest::keyClick(layer, Qt::Key_Escape);
        QTRY_VERIFY(!layer->isVisible());
        countPicker.reject();
        explicitPicker.reject();
    }

    // ------------------------------------------------------------------
    // TaskDateController：笔记、原子保存、基线冲突
    // ------------------------------------------------------------------
    void controllerWritesOnlyOnParentConfirm() {
        const QString id = m_store->createTask(QStringLiteral("子层草稿"));
        QVERIFY(!id.isEmpty());
        QWidget owner;
        owner.resize(400, 250);
        owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());

        const QDate today = toAppTime(appNow()).date();
        auto* entry = findEntry(picker.data(), QStringLiteral("时间"));
        QVERIFY(entry && entry->isVisible());
        auto* expansion = openTimeExpansion(picker.data(), entry);
        QVERIFY(expansion);
        auto* list = listOf(expansion);
        QVERIFY(list);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          list->visualItemRect(list->item(8)).center());  // 04:00
        QTRY_VERIFY(!expansion->isVisible());
        QVERIFY(picker->hasTime());
        QVERIFY(!m_store->entity(id).dueAt.isValid());  // 父层未确认：尚未写入
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        QVERIFY(m_store->entity(id).dueAt.isValid());
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).date(), today);
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).time(), QTime(4, 0));
        QTRY_VERIFY(!picker);

        // 父层取消：0 写。
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        AnchoredListPopup* layer = openLayer(picker.data(), QStringLiteral("提醒"),
                                             QStringLiteral("DatePickerReminderLayer"));
        QVERIFY(layer);
        layer->reject();
        QTRY_VERIFY(!layer->isVisible());
        picker->reject();
        QTRY_VERIFY(!picker);
        QCOMPARE(saved.count(), 1);
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).time(), QTime(4, 0));
    }

    void controllerAcceptsNoteWithFullScheduleButNoRecurrence() {
        const QString note = m_store->createNote(QStringLiteral("可排期的笔记"));
        QVERIFY(!note.isEmpty());
        QWidget owner;
        owner.resize(400, 250);
        owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        controller.open(note, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        // 笔记允许日期与提醒，但不提供重复入口。
        QVERIFY(findEntry(picker.data(), QStringLiteral("时间"))->isVisible());
        QVERIFY(findEntry(picker.data(), QStringLiteral("提醒"))->isVisible());
        QVERIFY(!findEntry(picker.data(), QStringLiteral("重复"))->isVisible());

        const QDate day = toAppTime(appNow()).date().addDays(2);
        picker->setDateTime(fromAppTime(QDateTime(day, QTime(9, 30))));
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        const Entity stored = m_store->entity(note);
        QCOMPARE(stored.kind, EntityKind::Note);
        QVERIFY(stored.dueAt.isValid());
        QCOMPARE(toAppTime(stored.dueAt).date(), day);
        QCOMPARE(toAppTime(stored.dueAt).time(), QTime(9, 30));
        QTRY_VERIFY(!picker);
    }

    void controllerSavesScheduleAtomicallyAndMergesUnrelatedFields() {
        const QString id = m_store->createTask(QStringLiteral("原子调度保存"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("并行编辑的正文");
        task.priority = 2;
        QVERIFY(m_store->saveTask(task));
        QWidget owner;
        owner.resize(400, 250);
        owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        // 弹层打开期间其他人改了标题：调度保存不得覆盖无关字段。
        Entity concurrent = m_store->entity(id);
        concurrent.title = QStringLiteral("并发新标题");
        QVERIFY(m_store->saveTask(concurrent));
        const QDate day = toAppTime(appNow()).date().addDays(4);
        picker->setSchedule(singleAllDay(day));
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        const Entity after = m_store->entity(id);
        QCOMPARE(after.allDayDate, day);
        QVERIFY(!after.dueAt.isValid() && !after.startAt.isValid());
        QCOMPARE(after.title, QStringLiteral("并发新标题"));
        QCOMPARE(after.body, QStringLiteral("并行编辑的正文"));
        QCOMPARE(after.priority, 2);
        QTRY_VERIFY(!picker);
    }

    void controllerRejectsScheduleBaselineConflict() {
        const QString id = m_store->createTask(QStringLiteral("基线冲突保护"));
        QWidget owner;
        owner.resize(400, 250);
        owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        QSignalSpy failed(&controller, &TaskDateController::saveFailed);
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        // 弹层打开期间调度基线被其他窗口改写。
        const QDate externalDay = toAppTime(appNow()).date().addDays(9);
        QVERIFY(m_store->saveSchedule(m_store->entity(id), singleAllDay(externalDay)));
        const QDate localDay = toAppTime(appNow()).date().addDays(5);
        picker->setSchedule(singleAllDay(localDay));
        picker->accept();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(saved.count(), 0);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("其他窗口")));
        // 冲突时不覆盖外部值。
        QCOMPARE(m_store->entity(id).allDayDate, externalDay);
        QTRY_VERIFY(!picker);

        // 无冲突的正常路径仍然成功；标题修改不构成调度基线冲突。
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker);
        QTRY_VERIFY(picker->isVisible());
        Entity renamed = m_store->entity(id);
        renamed.title = QStringLiteral("标题改动不是调度冲突");
        QVERIFY(m_store->saveTask(renamed));
        picker->setSchedule(singleAllDay(localDay));
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(m_store->entity(id).allDayDate, localDay);
    }

private:
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};

QTEST_MAIN(DatePopupTest)
#include "tst_datepopup.moc"
