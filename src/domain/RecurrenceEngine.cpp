#include "RecurrenceEngine.h"

#include "LunarCalendar.h"

#include <QDate>
#include <algorithm>

namespace kl {
namespace {

QDate fixedMonthDay(int year, int month, int target) {
    const QDate first(year, month, 1);
    if (!first.isValid()) return QDate();
    const int day = target == -1 ? first.daysInMonth() : target;
    // 固定日期在短月中不存在时跳过，绝不夹到另一个日期；-1 才表示月末。
    if (day < 1 || day > first.daysInMonth()) return QDate();
    return QDate(year, month, day);
}

QDate ordinalWeekday(int year, int month, int ordinal, int weekday) {
    const QDate first(year, month, 1);
    if (!first.isValid()) return QDate();
    if (ordinal == -1) {
        QDate result(year, month, first.daysInMonth());
        result = result.addDays(-((result.dayOfWeek() - weekday + 7) % 7));
        return result;
    }
    QDate result = first.addDays((weekday - first.dayOfWeek() + 7) % 7);
    result = result.addDays((ordinal - 1) * 7);
    return result.month() == month ? result : QDate();
}

QDate workday(int year, int month, bool firstDay) {
    const QDate first(year, month, 1);
    if (!first.isValid()) return QDate();
    QDate value(year, month, firstDay ? 1 : first.daysInMonth());
    // 普通周一至周五，不使用法定节假日数据或调休规则。
    while (value.isValid() && value.dayOfWeek() >= Qt::Saturday)
        value = value.addDays(firstDay ? 1 : -1);
    return value;
}

int positiveModulo(qint64 value, int divisor) {
    const int result = int(value % divisor);
    return result < 0 ? result + divisor : result;
}

QDate weekStart(const QDate& value, int firstWeekday) {
    return value.addDays(-((value.dayOfWeek() - firstWeekday + 7) % 7));
}

QDate nextWeekly(const RecurrenceSpec& spec, const QDate& current,
                 const QDate& seriesAnchor) {
    if (spec.weekdays.isEmpty()) return current.addDays(7 * spec.interval);

    const QDate anchorWeek = weekStart(seriesAnchor, spec.weekStart);
    const QDate currentWeek = weekStart(current, spec.weekStart);
    const qint64 elapsedWeeks = anchorWeek.daysTo(currentWeek) / 7;
    const int remainder = positiveModulo(elapsedWeeks, spec.interval);
    const int firstOffset = remainder == 0 ? 0 : spec.interval - remainder;

    // 每个有效周至少有一个候选；当前周无后续候选时，下一有效周必有。
    for (int activeWeek = 0; activeWeek < 2; ++activeWeek) {
        const QDate week = currentWeek.addDays((firstOffset + activeWeek * spec.interval) * 7);
        if (!week.isValid()) return QDate();
        QVector<QDate> dates;
        for (int weekday : spec.weekdays) {
            const int offset = (weekday - spec.weekStart + 7) % 7;
            const QDate candidate = week.addDays(offset);
            if (candidate > current) dates.append(candidate);
        }
        std::sort(dates.begin(), dates.end());
        if (!dates.isEmpty()) return dates.first();
    }
    return QDate();
}

QVector<QDate> monthlyCandidates(const RecurrenceSpec& spec, const QDate& month) {
    QVector<QDate> dates;
    if (spec.monthMode == RecurrenceSpec::MonthMode::Workday) {
        const QDate candidate = workday(month.year(), month.month(), spec.firstWorkday);
        if (candidate.isValid()) dates.append(candidate);
    } else if (spec.monthMode == RecurrenceSpec::MonthMode::Weekday) {
        const QDate candidate = ordinalWeekday(month.year(), month.month(),
                                               spec.ordinal, spec.ordinalWeekday);
        if (candidate.isValid()) dates.append(candidate);
    } else {
        for (int day : spec.monthDays) {
            const QDate candidate = fixedMonthDay(month.year(), month.month(), day);
            if (candidate.isValid()) dates.append(candidate);
        }
    }
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

QDate nextMonthly(const RecurrenceSpec& spec, const QDate& current,
                  const QDate& seriesAnchor) {
    const QDate currentMonth(current.year(), current.month(), 1);
    const qint64 elapsedMonths = (qint64(current.year()) - seriesAnchor.year()) * 12
        + current.month() - seriesAnchor.month();
    const int remainder = positiveModulo(elapsedMonths, spec.interval);
    const int firstOffset = remainder == 0 ? 0 : spec.interval - remainder;

    // 任意连续 12 个有效月都包含 31 日和第五个任意星期几。
    // 13 次还覆盖了首个有效月只剩余当前日之前候选的情况。
    for (int activeMonth = 0; activeMonth < 13; ++activeMonth) {
        const QDate month = currentMonth.addMonths(firstOffset + activeMonth * spec.interval);
        if (!month.isValid()) return QDate();
        const QVector<QDate> dates = monthlyCandidates(spec, month);
        for (const QDate& candidate : dates) if (candidate > current) return candidate;
    }
    return QDate();
}

QDate yearlyCandidate(const RecurrenceSpec& spec, int year) {
    if (spec.yearMode == RecurrenceSpec::YearMode::Weekday)
        return ordinalWeekday(year, spec.month, spec.ordinal, spec.ordinalWeekday);
    // v1 规则要求固定按年日期恰好一个 monthDays 项。
    return fixedMonthDay(year, spec.month, spec.monthDays.value(0));
}

QDate nextYearly(const RecurrenceSpec& spec, const QDate& current,
                 const QDate& seriesAnchor) {
    const int remainder = positiveModulo(qint64(current.year()) - seriesAnchor.year(), spec.interval);
    const int firstOffset = remainder == 0 ? 0 : spec.interval - remainder;
    int year = current.year() + firstOffset;

    // Gregorian 的星期和闰日模式每 400 年重复。以有效周期年计，
    // 400 次足以判定不存在的相位组合，第 401 次覆盖当前候选后的完整周期。
    for (int activeYear = 0; activeYear <= 400; ++activeYear) {
        const QDate candidate = yearlyCandidate(spec, year);
        if (candidate > current) return candidate;
        year += spec.interval;
    }
    return QDate();
}

QDate nextCandidate(const RecurrenceSpec& spec, const QDate& current,
                    const QDate& seriesAnchor) {
    if (spec.kind == RecurrenceSpec::Kind::LunarMonthly
        || spec.kind == RecurrenceSpec::Kind::LunarYearly) {
        LunarDate anchor;
        if (!LunarCalendar::fromSolar(seriesAnchor, &anchor)) return QDate();
        bool found = false;
        const QDate next = spec.kind == RecurrenceSpec::Kind::LunarMonthly
            ? LunarCalendar::nextMonthly(current, anchor, spec.lunarLeapMonthRepeat, &found)
            : LunarCalendar::nextYearly(current, anchor, spec.lunarLeapMonthRepeat, &found);
        return found && next > current ? next : QDate();
    }
    if (spec.kind != RecurrenceSpec::Kind::Gregorian) return QDate();

    switch (spec.frequency) {
        case RecurrenceSpec::Frequency::Daily:
            return current.addDays(spec.interval);
        case RecurrenceSpec::Frequency::Weekly:
            return nextWeekly(spec, current, seriesAnchor);
        case RecurrenceSpec::Frequency::Monthly:
            // 按完成日期的普通月间隔没有固定日选择，Qt 的月末夹取语义保持不变。
            if (spec.monthDays.isEmpty() && spec.monthMode == RecurrenceSpec::MonthMode::Days)
                return current.addMonths(spec.interval);
            return nextMonthly(spec, current, seriesAnchor);
        case RecurrenceSpec::Frequency::Yearly:
            return nextYearly(spec, current, seriesAnchor);
    }
    return QDate();
}

} // namespace

RecurrenceAdvance nextRecurrence(const RecurrenceSpec& spec, const QDate& current,
                                 const QDate& completionDate,
                                 int occurrenceOrdinal, int completionCount,
                                 const QDate& seriesAnchor) {
    RecurrenceAdvance result;
    result.occurrenceOrdinal = occurrenceOrdinal;
    result.completionCount = completionCount;
    QString error;
    if (!current.isValid() || !isValidRecurrence(spec, &error)
        || occurrenceOrdinal < 1 || completionCount < 0) {
        result.error = error.isEmpty() ? QStringLiteral("当前重复日期或进度无效") : error;
        return result;
    }
    if (spec.endMode == RecurrenceSpec::EndMode::Count
        && completionCount + 1 >= spec.endCount) {
        result.status = RecurrenceStatus::Exhausted;
        return result;
    }
    if (spec.endMode == RecurrenceSpec::EndMode::Date && current >= spec.endDate) {
        result.status = RecurrenceStatus::Exhausted;
        return result;
    }

    if (spec.kind == RecurrenceSpec::Kind::ExplicitDates) {
        for (const QDate& candidate : spec.explicitDates) {
            if (candidate <= current) continue;
            if (spec.endMode == RecurrenceSpec::EndMode::Date && candidate > spec.endDate) {
                result.status = RecurrenceStatus::Exhausted;
                return result;
            }
            result.status = RecurrenceStatus::Next;
            result.date = candidate;
            result.occurrenceOrdinal = occurrenceOrdinal + 1;
            result.completionCount = completionCount + 1;
            return result;
        }
        result.status = RecurrenceStatus::Exhausted;
        return result;
    }

    const bool completionBasis = spec.basis == RecurrenceSpec::Basis::CompletionDate;
    const QDate base = completionBasis
        ? (completionDate.isValid() ? completionDate : current) : current;
    // DTSTART 的本地日期是周期相位、固定日及农历月末/闰月语义的原始锚点。
    // 未传锚点仅作为无持久化 DTSTART 调用者的回退，不进行旧规则迁移。
    const QDate originalAnchor = completionBasis || !seriesAnchor.isValid() ? base : seriesAnchor;
    const QDate candidate = nextCandidate(spec, base, originalAnchor);
    if (!candidate.isValid() || candidate <= base) {
        if (spec.endMode == RecurrenceSpec::EndMode::Date) {
            result.status = RecurrenceStatus::Exhausted;
            return result;
        }
        result.error = QStringLiteral("无法计算下一次有效重复日期");
        return result;
    }
    if (spec.endMode == RecurrenceSpec::EndMode::Date && candidate > spec.endDate) {
        result.status = RecurrenceStatus::Exhausted;
        return result;
    }
    result.status = RecurrenceStatus::Next;
    result.date = candidate;
    result.occurrenceOrdinal = occurrenceOrdinal + 1;
    result.completionCount = completionCount + 1;
    return result;
}

bool advanceLocalRange(const RecurrenceSpec& spec, const QDate& currentStart,
                       const QDate& currentEnd, const QDate& completionDate,
                       int occurrenceOrdinal, int completionCount,
                       QDate* nextStart, QDate* nextEnd,
                       RecurrenceAdvance* advance, QString* error,
                       const QDate& seriesAnchor) {
    if (!nextStart || !nextEnd || !advance || !currentStart.isValid()
        || !currentEnd.isValid() || currentEnd < currentStart) {
        if (error) *error = QStringLiteral("时间段端点无效");
        return false;
    }
    // 时间段的重复以结束日为当前出现，保持本地日期的包含式日历跨度。
    *advance = nextRecurrence(spec, currentEnd, completionDate, occurrenceOrdinal,
                              completionCount, seriesAnchor);
    if (advance->status == RecurrenceStatus::Exhausted) {
        *nextStart = QDate(); *nextEnd = QDate();
        return true;
    }
    if (advance->status != RecurrenceStatus::Next) {
        if (error) *error = advance->error;
        return false;
    }
    const qint64 span = currentStart.daysTo(currentEnd);
    *nextEnd = advance->date;
    *nextStart = advance->date.addDays(-span);
    if (!nextStart->isValid()) {
        if (error) *error = QStringLiteral("下一次时间段超出有效日期范围");
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace kl
