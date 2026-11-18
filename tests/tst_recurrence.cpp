#include <QtTest/QtTest>

#include "domain/LunarCalendar.h"
#include "domain/RecurrenceEngine.h"

using namespace kl;

namespace {

RecurrenceAdvance next(const RecurrenceSpec& spec, const QDate& current,
                       const QDate& anchor = QDate()) {
    return nextRecurrence(spec, current, QDate(), 1, 0, anchor);
}

void compareNext(const RecurrenceAdvance& advance, const QDate& expected) {
    QCOMPARE(int(advance.status), int(RecurrenceStatus::Next));
    QCOMPARE(advance.date, expected);
}

} // namespace

class RecurrenceTest : public QObject {
    Q_OBJECT

private slots:
    void solarLunarConversion_data();
    void solarLunarConversion();
    void conversionRange();
    void weeklyMultiSelectKeepsAnchorPhase();
    void monthlyFixedDaysAndLastDay();
    void monthlyOrdinalWeekdays();
    void monthlyOrdinaryWorkdays();
    void yearlyFixedDateAndOrdinalKeepAnchor();
    void completionBasisAndProgress();
    void explicitDatesAdvanceProgress();
    void localRangePreservesInclusiveSpan();
    void lunarMonthlyKeepsOriginalAnchorAndLeapPolicy();
    void lunarYearlyKeepsOriginalAnchorAndLeapPolicy();
    void lunarYearlyDoesNotPromoteFixedDayToLastDay();
};

void RecurrenceTest::solarLunarConversion_data() {
    QTest::addColumn<QDate>("solar");
    QTest::addColumn<int>("year");
    QTest::addColumn<int>("month");
    QTest::addColumn<int>("day");
    QTest::addColumn<bool>("leap");
    QTest::addColumn<bool>("lastDay");

    // 取自仓库 .tools/chinese-lunar-reference-1.0.1/package/test/dates.json。
    QTest::newRow("epoch") << QDate(1900, 1, 31) << 1900 << 1 << 1 << false << false;
    QTest::newRow("2020-new-year") << QDate(2020, 1, 25) << 2020 << 1 << 1 << false << false;
    QTest::newRow("2020-regular-4-end") << QDate(2020, 5, 22) << 2020 << 4 << 30 << false << true;
    QTest::newRow("2020-leap-4-start") << QDate(2020, 5, 23) << 2020 << 4 << 1 << true << false;
    QTest::newRow("2020-leap-4-end") << QDate(2020, 6, 20) << 2020 << 4 << 29 << true << true;
    QTest::newRow("2020-after-leap") << QDate(2020, 6, 21) << 2020 << 5 << 1 << false << false;
    QTest::newRow("2023-regular-2-end") << QDate(2023, 3, 21) << 2023 << 2 << 30 << false << true;
    QTest::newRow("2023-leap-2-start") << QDate(2023, 3, 22) << 2023 << 2 << 1 << true << false;
    QTest::newRow("2023-leap-2-end") << QDate(2023, 4, 19) << 2023 << 2 << 29 << true << true;
    QTest::newRow("2024-new-year") << QDate(2024, 2, 10) << 2024 << 1 << 1 << false << false;
    QTest::newRow("2100-new-year") << QDate(2100, 2, 9) << 2100 << 1 << 1 << false << false;
    QTest::newRow("2100-last-supported-solar-day") << QDate(2100, 12, 31) << 2100 << 12 << 1 << false << false;
}

void RecurrenceTest::solarLunarConversion() {
    QFETCH(QDate, solar);
    QFETCH(int, year);
    QFETCH(int, month);
    QFETCH(int, day);
    QFETCH(bool, leap);
    QFETCH(bool, lastDay);

    LunarDate lunar;
    QString error;
    QVERIFY2(LunarCalendar::fromSolar(solar, &lunar, &error), qPrintable(error));
    QCOMPARE(lunar.year, year);
    QCOMPARE(lunar.month, month);
    QCOMPARE(lunar.day, day);
    QCOMPARE(lunar.leapMonth, leap);
    QCOMPARE(lunar.lastDay, lastDay);

    QDate roundTrip;
    QVERIFY2(LunarCalendar::toSolar(lunar, &roundTrip, &error), qPrintable(error));
    QCOMPARE(roundTrip, solar);
}

void RecurrenceTest::conversionRange() {
    QCOMPARE(LunarCalendar::minimumSolarYear(), 1900);
    QCOMPARE(LunarCalendar::maximumSolarYear(), 2100);
    LunarDate lunar;
    QVERIFY(!LunarCalendar::fromSolar(QDate(1900, 1, 30), &lunar));
    QVERIFY(!LunarCalendar::fromSolar(QDate(2101, 1, 1), &lunar));
    QDate solar;
    QVERIFY(!LunarCalendar::toSolar(LunarDate{2101, 1, 1, false, false}, &solar));
    QVERIFY(!LunarCalendar::toSolar(LunarDate{2020, 4, 30, true, false}, &solar));
}

void RecurrenceTest::weeklyMultiSelectKeepsAnchorPhase() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Weekly;
    spec.interval = 2;
    spec.weekdays = {Qt::Monday, Qt::Wednesday, Qt::Friday};
    const QDate anchor(2024, 1, 1); // 周一，有效周起点。

    compareNext(next(spec, QDate(2024, 1, 3), anchor), QDate(2024, 1, 5));
    compareNext(next(spec, QDate(2024, 1, 5), anchor), QDate(2024, 1, 15));
    compareNext(next(spec, QDate(2024, 1, 12), anchor), QDate(2024, 1, 15));
    compareNext(next(spec, QDate(2024, 1, 15), anchor), QDate(2024, 1, 17));

    // 周日起始时，星期数值排序不能覆盖周内实际时间顺序。
    spec.weekStart = Qt::Sunday;
    spec.weekdays = {Qt::Monday, Qt::Sunday};
    compareNext(next(spec, QDate(2024, 1, 7), QDate(2024, 1, 7)), QDate(2024, 1, 8));
}

void RecurrenceTest::monthlyFixedDaysAndLastDay() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    spec.monthDays = {10, 31};
    const QDate anchor(2024, 1, 10);

    compareNext(next(spec, QDate(2024, 1, 10), anchor), QDate(2024, 1, 31));
    compareNext(next(spec, QDate(2024, 1, 31), anchor), QDate(2024, 2, 10));
    compareNext(next(spec, QDate(2024, 2, 10), anchor), QDate(2024, 3, 10));

    // 固定 31 日在短月不存在，不能夹成 30 日或月末。
    spec.monthDays = {31};
    compareNext(next(spec, QDate(2024, 1, 31), QDate(2024, 1, 31)), QDate(2024, 3, 31));
    compareNext(next(spec, QDate(2024, 3, 31), QDate(2024, 1, 31)), QDate(2024, 5, 31));

    spec.monthDays = {-1, 15};
    compareNext(next(spec, QDate(2024, 1, 15), QDate(2024, 1, 15)), QDate(2024, 1, 31));
    compareNext(next(spec, QDate(2024, 1, 31), QDate(2024, 1, 15)), QDate(2024, 2, 15));
    compareNext(next(spec, QDate(2024, 2, 15), QDate(2024, 1, 15)), QDate(2024, 2, 29));

    // 间隔相位来自原 series，而不是从当前出现重新起算。
    spec.interval = 2;
    spec.monthDays = {1, 20};
    compareNext(next(spec, QDate(2024, 1, 20), QDate(2024, 1, 1)), QDate(2024, 3, 1));
}

void RecurrenceTest::monthlyOrdinalWeekdays() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    spec.monthMode = RecurrenceSpec::MonthMode::Weekday;
    spec.ordinal = 2;
    spec.ordinalWeekday = Qt::Tuesday;
    compareNext(next(spec, QDate(2024, 2, 13), QDate(2024, 1, 9)), QDate(2024, 3, 12));

    // 2024 年 2、3 月都没有第五个周一，应继续寻找而不是返回空日期。
    spec.ordinal = 5;
    spec.ordinalWeekday = Qt::Monday;
    compareNext(next(spec, QDate(2024, 1, 29), QDate(2024, 1, 29)), QDate(2024, 4, 29));

    spec.ordinal = -1;
    spec.ordinalWeekday = Qt::Sunday;
    compareNext(next(spec, QDate(2024, 1, 28), QDate(2024, 1, 28)), QDate(2024, 2, 25));
}

void RecurrenceTest::monthlyOrdinaryWorkdays() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    spec.monthMode = RecurrenceSpec::MonthMode::Workday;
    spec.firstWorkday = true;
    compareNext(next(spec, QDate(2024, 5, 1), QDate(2024, 4, 1)), QDate(2024, 6, 3));
    // 劳动节也是普通周三工作日，不引入法定节假日或调休数据。
    compareNext(next(spec, QDate(2024, 4, 1), QDate(2024, 4, 1)), QDate(2024, 5, 1));

    spec.firstWorkday = false;
    compareNext(next(spec, QDate(2024, 5, 31), QDate(2024, 4, 30)), QDate(2024, 6, 28));
}

void RecurrenceTest::yearlyFixedDateAndOrdinalKeepAnchor() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Yearly;
    spec.month = 2;
    spec.monthDays = {29};
    const QDate leapAnchor(2024, 2, 29);
    compareNext(next(spec, leapAnchor, leapAnchor), QDate(2028, 2, 29));
    compareNext(next(spec, QDate(2028, 2, 29), leapAnchor), QDate(2032, 2, 29));

    spec.monthDays = {-1};
    compareNext(next(spec, QDate(2024, 2, 29), leapAnchor), QDate(2025, 2, 28));

    spec.month = 3;
    spec.monthDays = {10};
    spec.interval = 2;
    compareNext(next(spec, QDate(2026, 3, 10), QDate(2024, 3, 10)), QDate(2028, 3, 10));

    spec.yearMode = RecurrenceSpec::YearMode::Weekday;
    spec.monthDays.clear();
    spec.month = 5;
    spec.ordinal = -1;
    spec.ordinalWeekday = Qt::Monday;
    compareNext(next(spec, QDate(2024, 5, 27), QDate(2024, 5, 27)), QDate(2026, 5, 25));
}

void RecurrenceTest::completionBasisAndProgress() {
    RecurrenceSpec spec;
    spec.basis = RecurrenceSpec::Basis::CompletionDate;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    RecurrenceAdvance advance = nextRecurrence(spec, QDate(2024, 1, 20), QDate(2024, 1, 31), 4, 2);
    compareNext(advance, QDate(2024, 2, 29));
    QCOMPARE(advance.occurrenceOrdinal, 5);
    QCOMPARE(advance.completionCount, 3);

    spec = RecurrenceSpec();
    spec.frequency = RecurrenceSpec::Frequency::Daily;
    spec.endMode = RecurrenceSpec::EndMode::Count;
    spec.endCount = 3;
    advance = nextRecurrence(spec, QDate(2024, 1, 1), QDate(), 1, 0);
    compareNext(advance, QDate(2024, 1, 2));
    advance = nextRecurrence(spec, advance.date, QDate(), advance.occurrenceOrdinal, advance.completionCount);
    compareNext(advance, QDate(2024, 1, 3));
    advance = nextRecurrence(spec, advance.date, QDate(), advance.occurrenceOrdinal, advance.completionCount);
    QCOMPARE(int(advance.status), int(RecurrenceStatus::Exhausted));
    QCOMPARE(advance.occurrenceOrdinal, 3);
    QCOMPARE(advance.completionCount, 2);
}

void RecurrenceTest::explicitDatesAdvanceProgress() {
    RecurrenceSpec spec;
    spec.kind = RecurrenceSpec::Kind::ExplicitDates;
    spec.explicitDates = {QDate(2024, 1, 1), QDate(2024, 1, 5), QDate(2024, 1, 10)};
    RecurrenceAdvance advance = nextRecurrence(spec, QDate(2024, 1, 1), QDate(), 4, 2);
    compareNext(advance, QDate(2024, 1, 5));
    QCOMPARE(advance.occurrenceOrdinal, 5);
    QCOMPARE(advance.completionCount, 3);
}

void RecurrenceTest::localRangePreservesInclusiveSpan() {
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    spec.monthDays = {10, 31};
    QDate nextStart;
    QDate nextEnd;
    RecurrenceAdvance advance;
    QString error;
    QVERIFY2(advanceLocalRange(spec, QDate(2024, 1, 29), QDate(2024, 1, 31), QDate(),
                               7, 3, &nextStart, &nextEnd, &advance, &error,
                               QDate(2024, 1, 10)), qPrintable(error));
    QCOMPARE(nextStart, QDate(2024, 2, 8));
    QCOMPARE(nextEnd, QDate(2024, 2, 10));
    QCOMPARE(advance.occurrenceOrdinal, 8);
    QCOMPARE(advance.completionCount, 4);
}

void RecurrenceTest::lunarMonthlyKeepsOriginalAnchorAndLeapPolicy() {
    LunarDate lastDayAnchor;
    QVERIFY(LunarCalendar::fromSolar(QDate(2020, 5, 22), &lastDayAnchor)); // 四月三十，月末。
    bool found = false;
    QCOMPARE(LunarCalendar::nextMonthly(QDate(2020, 5, 22), lastDayAnchor, false, &found),
             QDate(2020, 7, 20)); // 跳过闰四月，五月三十。
    QVERIFY(found);
    QCOMPARE(LunarCalendar::nextMonthly(QDate(2020, 5, 22), lastDayAnchor, true, &found),
             QDate(2020, 6, 20)); // 闰四月二十九，实际月末。
    QVERIFY(found);
    QCOMPARE(LunarCalendar::nextMonthly(QDate(2020, 6, 20), lastDayAnchor, true, &found),
             QDate(2020, 7, 20));

    RecurrenceSpec spec;
    spec.kind = RecurrenceSpec::Kind::LunarMonthly;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    // 原五月二十九不是月末；九月二十九虽是当月末，也不能把后续规则改成月末。
    compareNext(next(spec, QDate(2020, 9, 16), QDate(2020, 7, 19)), QDate(2020, 10, 15));
}

void RecurrenceTest::lunarYearlyKeepsOriginalAnchorAndLeapPolicy() {
    LunarDate regularAnchor;
    QVERIFY(LunarCalendar::fromSolar(QDate(2023, 3, 21), &regularAnchor)); // 二月三十，月末。
    bool found = false;
    QCOMPARE(LunarCalendar::nextYearly(QDate(2023, 3, 21), regularAnchor, true, &found),
             QDate(2023, 4, 19)); // 同年闰二月二十九。
    QVERIFY(found);
    QCOMPARE(LunarCalendar::nextYearly(QDate(2023, 3, 21), regularAnchor, false, &found),
             QDate(2024, 4, 8));

    LunarDate leapAnchor;
    QVERIFY(LunarCalendar::fromSolar(QDate(2023, 4, 19), &leapAnchor));
    QVERIFY(leapAnchor.leapMonth);
    // 原闰月锚点保留，但 2024 没有闰二月，回退到正常二月月末。
    QCOMPARE(LunarCalendar::nextYearly(QDate(2023, 4, 19), leapAnchor, false, &found),
             QDate(2024, 4, 8));
}

void RecurrenceTest::lunarYearlyDoesNotPromoteFixedDayToLastDay() {
    LunarDate anchor;
    QVERIFY(LunarCalendar::fromSolar(QDate(2020, 7, 19), &anchor)); // 五月二十九，非月末。
    QVERIFY(!anchor.lastDay);
    QDate current;
    QDate expected;
    QVERIFY(LunarCalendar::toSolar(LunarDate{2029, 5, 29, false, true}, &current));
    QVERIFY(LunarCalendar::toSolar(LunarDate{2030, 5, 29, false, false}, &expected));
    LunarDate currentLunar;
    QVERIFY(LunarCalendar::fromSolar(current, &currentLunar));
    QVERIFY(currentLunar.lastDay); // 2029 五月仅 29 天。
    bool found = false;
    QCOMPARE(LunarCalendar::nextYearly(current, anchor, false, &found), expected);
    QVERIFY(found);
}

QTEST_GUILESS_MAIN(RecurrenceTest)
#include "tst_recurrence.moc"
