#include "RecurrenceRule.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <algorithm>
#include <limits>

namespace kl {
namespace {

QString text(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString();
}

bool integer(const QJsonObject& object, const char* key, int* value) {
    const QJsonValue raw = object.value(QLatin1String(key));
    if (!raw.isDouble()) return false;
    const double number = raw.toDouble(-1);
    if (number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max())
        || number != static_cast<double>(static_cast<int>(number))) return false;
    *value = static_cast<int>(number);
    return true;
}

template<typename Enum>
using EnumMap = QVector<QPair<QString, Enum>>;

template<typename Enum>
bool readEnum(const QJsonObject& object, const char* key,
              const EnumMap<Enum>& values, Enum* result) {
    const QString name = text(object, key);
    for (const auto& entry : values) if (name == entry.first) {
        *result = entry.second;
        return true;
    }
    return false;
}

template<typename Enum>
QString enumName(Enum value, const EnumMap<Enum>& values) {
    for (const auto& entry : values) if (entry.second == value) return entry.first;
    return QString();
}

QJsonArray integers(const QVector<int>& values) {
    QJsonArray result;
    for (int value : values) result.append(value);
    return result;
}

bool readIntegers(const QJsonObject& object, const char* key, QVector<int>* result) {
    if (!object.value(QLatin1String(key)).isArray()) return false;
    result->clear();
    for (const QJsonValue& value : object.value(QLatin1String(key)).toArray()) {
        if (!value.isDouble()) return false;
        const int number = value.toInt(0);
        if (number != value.toDouble()) return false;
        result->append(number);
    }
    return true;
}

QString date(const QDate& value) {
    return value.isValid() ? value.toString(Qt::ISODate) : QString();
}

bool readDate(const QJsonObject& object, const char* key, QDate* result, bool optional = false) {
    const QString value = text(object, key);
    if (optional && value.isEmpty()) { *result = QDate(); return true; }
    const QDate parsed = QDate::fromString(value, Qt::ISODate);
    if (!parsed.isValid() || parsed.toString(Qt::ISODate) != value) return false;
    *result = parsed;
    return true;
}

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

} // namespace

QJsonObject recurrenceToJson(const RecurrenceSpec& spec) {
    QString ignoredError;
    if (!isValidRecurrence(spec, &ignoredError)) return QJsonObject();
    static const EnumMap<RecurrenceSpec::Kind> kinds = {qMakePair(QString::fromLatin1("gregorian"), RecurrenceSpec::Kind::Gregorian),
        qMakePair(QString::fromLatin1("lunar-monthly"), RecurrenceSpec::Kind::LunarMonthly),
        qMakePair(QString::fromLatin1("lunar-yearly"), RecurrenceSpec::Kind::LunarYearly),
        qMakePair(QString::fromLatin1("explicit"), RecurrenceSpec::Kind::ExplicitDates)};
    static const EnumMap<RecurrenceSpec::Basis> bases = {qMakePair(QString::fromLatin1("due"), RecurrenceSpec::Basis::DueDate),
        qMakePair(QString::fromLatin1("completion"), RecurrenceSpec::Basis::CompletionDate)};
    static const EnumMap<RecurrenceSpec::Frequency> frequencies = {qMakePair(QString::fromLatin1("daily"), RecurrenceSpec::Frequency::Daily),
        qMakePair(QString::fromLatin1("weekly"), RecurrenceSpec::Frequency::Weekly),
        qMakePair(QString::fromLatin1("monthly"), RecurrenceSpec::Frequency::Monthly),
        qMakePair(QString::fromLatin1("yearly"), RecurrenceSpec::Frequency::Yearly)};
    static const EnumMap<RecurrenceSpec::MonthMode> monthModes = {qMakePair(QString::fromLatin1("days"), RecurrenceSpec::MonthMode::Days),
        qMakePair(QString::fromLatin1("weekday"), RecurrenceSpec::MonthMode::Weekday),
        qMakePair(QString::fromLatin1("workday"), RecurrenceSpec::MonthMode::Workday)};
    static const EnumMap<RecurrenceSpec::YearMode> yearModes = {qMakePair(QString::fromLatin1("date"), RecurrenceSpec::YearMode::Date),
        qMakePair(QString::fromLatin1("weekday"), RecurrenceSpec::YearMode::Weekday)};
    static const EnumMap<RecurrenceSpec::EndMode> ends = {qMakePair(QString::fromLatin1("never"), RecurrenceSpec::EndMode::Never),
        qMakePair(QString::fromLatin1("date"), RecurrenceSpec::EndMode::Date),
        qMakePair(QString::fromLatin1("count"), RecurrenceSpec::EndMode::Count)};
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("kind"), enumName(spec.kind, kinds));
    object.insert(QStringLiteral("basis"), enumName(spec.basis, bases));
    object.insert(QStringLiteral("frequency"), enumName(spec.frequency, frequencies));
    object.insert(QStringLiteral("interval"), spec.interval);
    object.insert(QStringLiteral("weekdays"), integers(spec.weekdays));
    object.insert(QStringLiteral("monthDays"), integers(spec.monthDays));
    object.insert(QStringLiteral("monthMode"), enumName(spec.monthMode, monthModes));
    object.insert(QStringLiteral("ordinal"), spec.ordinal);
    object.insert(QStringLiteral("ordinalWeekday"), spec.ordinalWeekday);
    object.insert(QStringLiteral("firstWorkday"), spec.firstWorkday);
    object.insert(QStringLiteral("yearMode"), enumName(spec.yearMode, yearModes));
    object.insert(QStringLiteral("month"), spec.month);
    QJsonArray dates;
    for (const QDate& value : spec.explicitDates) dates.append(date(value));
    object.insert(QStringLiteral("explicitDates"), dates);
    object.insert(QStringLiteral("endMode"), enumName(spec.endMode, ends));
    object.insert(QStringLiteral("endDate"), date(spec.endDate));
    object.insert(QStringLiteral("endCount"), spec.endCount);
    object.insert(QStringLiteral("weekStart"), spec.weekStart);
    object.insert(QStringLiteral("lunarLeapMonthRepeat"), spec.lunarLeapMonthRepeat);
    return object;
}

bool recurrenceFromJson(const QJsonObject& object, RecurrenceSpec* spec, QString* error) {
    static const EnumMap<RecurrenceSpec::Kind> kinds = {qMakePair(QString::fromLatin1("gregorian"), RecurrenceSpec::Kind::Gregorian),
        qMakePair(QString::fromLatin1("lunar-monthly"), RecurrenceSpec::Kind::LunarMonthly),
        qMakePair(QString::fromLatin1("lunar-yearly"), RecurrenceSpec::Kind::LunarYearly),
        qMakePair(QString::fromLatin1("explicit"), RecurrenceSpec::Kind::ExplicitDates)};
    static const EnumMap<RecurrenceSpec::Basis> bases = {qMakePair(QString::fromLatin1("due"), RecurrenceSpec::Basis::DueDate),
        qMakePair(QString::fromLatin1("completion"), RecurrenceSpec::Basis::CompletionDate)};
    static const EnumMap<RecurrenceSpec::Frequency> frequencies = {qMakePair(QString::fromLatin1("daily"), RecurrenceSpec::Frequency::Daily),
        qMakePair(QString::fromLatin1("weekly"), RecurrenceSpec::Frequency::Weekly),
        qMakePair(QString::fromLatin1("monthly"), RecurrenceSpec::Frequency::Monthly),
        qMakePair(QString::fromLatin1("yearly"), RecurrenceSpec::Frequency::Yearly)};
    static const EnumMap<RecurrenceSpec::MonthMode> monthModes = {qMakePair(QString::fromLatin1("days"), RecurrenceSpec::MonthMode::Days),
        qMakePair(QString::fromLatin1("weekday"), RecurrenceSpec::MonthMode::Weekday),
        qMakePair(QString::fromLatin1("workday"), RecurrenceSpec::MonthMode::Workday)};
    static const EnumMap<RecurrenceSpec::YearMode> yearModes = {qMakePair(QString::fromLatin1("date"), RecurrenceSpec::YearMode::Date),
        qMakePair(QString::fromLatin1("weekday"), RecurrenceSpec::YearMode::Weekday)};
    static const EnumMap<RecurrenceSpec::EndMode> ends = {qMakePair(QString::fromLatin1("never"), RecurrenceSpec::EndMode::Never),
        qMakePair(QString::fromLatin1("date"), RecurrenceSpec::EndMode::Date),
        qMakePair(QString::fromLatin1("count"), RecurrenceSpec::EndMode::Count)};
    if (!spec || object.value(QStringLiteral("version")).toInt() != 1
        || object.value(QStringLiteral("version")).toDouble() != 1.0)
        return fail(error, QStringLiteral("重复规则版本无效"));
    const QSet<QString> known = QSet<QString>::fromList(QStringList()
        << QStringLiteral("version") << QStringLiteral("kind") << QStringLiteral("basis")
        << QStringLiteral("frequency") << QStringLiteral("interval") << QStringLiteral("weekdays")
        << QStringLiteral("monthDays") << QStringLiteral("monthMode") << QStringLiteral("ordinal")
        << QStringLiteral("ordinalWeekday") << QStringLiteral("firstWorkday") << QStringLiteral("yearMode")
        << QStringLiteral("month") << QStringLiteral("explicitDates") << QStringLiteral("endMode")
        << QStringLiteral("endDate") << QStringLiteral("endCount") << QStringLiteral("weekStart")
        << QStringLiteral("lunarLeapMonthRepeat"));
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!known.contains(it.key())) return fail(error, QStringLiteral("重复规则含未支持字段"));
    RecurrenceSpec value;
    if (!readEnum(object, "kind", kinds, &value.kind)
        || !readEnum(object, "basis", bases, &value.basis)
        || !readEnum(object, "frequency", frequencies, &value.frequency)
        || !integer(object, "interval", &value.interval)
        || !readIntegers(object, "weekdays", &value.weekdays)
        || !readIntegers(object, "monthDays", &value.monthDays)
        || !readEnum(object, "monthMode", monthModes, &value.monthMode)
        || !integer(object, "ordinal", &value.ordinal)
        || !integer(object, "ordinalWeekday", &value.ordinalWeekday)
        || !object.value(QStringLiteral("firstWorkday")).isBool()
        || !readEnum(object, "yearMode", yearModes, &value.yearMode)
        || !integer(object, "month", &value.month)
        || !object.value(QStringLiteral("explicitDates")).isArray()
        || !readEnum(object, "endMode", ends, &value.endMode)
        || !readDate(object, "endDate", &value.endDate, true)
        || !integer(object, "endCount", &value.endCount)
        || !integer(object, "weekStart", &value.weekStart)
        || !object.value(QStringLiteral("lunarLeapMonthRepeat")).isBool())
        return fail(error, QStringLiteral("重复规则字段不完整或类型错误"));
    value.firstWorkday = object.value(QStringLiteral("firstWorkday")).toBool();
    value.lunarLeapMonthRepeat = object.value(QStringLiteral("lunarLeapMonthRepeat")).toBool();
    for (const QJsonValue& raw : object.value(QStringLiteral("explicitDates")).toArray()) {
        if (!raw.isString()) return fail(error, QStringLiteral("自选重复日期格式无效"));
        const QDate parsed = QDate::fromString(raw.toString(), Qt::ISODate);
        if (!parsed.isValid() || parsed.toString(Qt::ISODate) != raw.toString())
            return fail(error, QStringLiteral("自选重复日期无效"));
        value.explicitDates.append(parsed);
    }
    return isValidRecurrence(value, error) ? (*spec = value, true) : false;
}

bool isValidRecurrence(const RecurrenceSpec& spec, QString* error) {
    const bool gregorian = spec.kind == RecurrenceSpec::Kind::Gregorian;
    const bool explicitDates = spec.kind == RecurrenceSpec::Kind::ExplicitDates;
    const bool lunarMonthly = spec.kind == RecurrenceSpec::Kind::LunarMonthly;
    const bool lunarYearly = spec.kind == RecurrenceSpec::Kind::LunarYearly;
    if (!gregorian && !explicitDates && !lunarMonthly && !lunarYearly)
        return fail(error, QStringLiteral("重复规则类型无效"));
    if (spec.interval < 1 || spec.interval > 999
        || spec.weekStart < Qt::Monday || spec.weekStart > Qt::Sunday
        || spec.month < 1 || spec.month > 12
        || spec.ordinalWeekday < Qt::Monday || spec.ordinalWeekday > Qt::Sunday
        || !((spec.ordinal >= 1 && spec.ordinal <= 5) || spec.ordinal == -1)
        || (spec.endMode != RecurrenceSpec::EndMode::Never
            && spec.endMode != RecurrenceSpec::EndMode::Date
            && spec.endMode != RecurrenceSpec::EndMode::Count)
        || (spec.endMode == RecurrenceSpec::EndMode::Date && !spec.endDate.isValid())
        || (spec.endMode != RecurrenceSpec::EndMode::Date && spec.endDate.isValid())
        || (spec.endMode == RecurrenceSpec::EndMode::Count
            && (spec.endCount < 2 || spec.endCount > 200)))
        return fail(error, QStringLiteral("重复规则的间隔、结束条件或日期参数无效"));
    const auto unique = [](QVector<int> values) {
        std::sort(values.begin(), values.end());
        return std::unique(values.begin(), values.end()) == values.end();
    };
    if (!std::is_sorted(spec.weekdays.begin(), spec.weekdays.end())
        || !std::is_sorted(spec.monthDays.begin(), spec.monthDays.end())
        || !unique(spec.weekdays) || !unique(spec.monthDays))
        return fail(error, QStringLiteral("星期或月份日期必须有序且不能重复"));
    for (int day : spec.weekdays) if (day < Qt::Monday || day > Qt::Sunday)
        return fail(error, QStringLiteral("星期参数无效"));
    for (int day : spec.monthDays) if ((day < 1 || day > 31) && day != -1)
        return fail(error, QStringLiteral("月份日期参数无效"));
    if (spec.basis == RecurrenceSpec::Basis::CompletionDate
        && (spec.kind != RecurrenceSpec::Kind::Gregorian
            || spec.frequency == RecurrenceSpec::Frequency::Yearly
            || !spec.weekdays.isEmpty() || !spec.monthDays.isEmpty()
            || spec.monthMode != RecurrenceSpec::MonthMode::Days
            || spec.ordinal != 1 || spec.ordinalWeekday != Qt::Monday
            || spec.yearMode != RecurrenceSpec::YearMode::Date || spec.month != 1))
        return fail(error, QStringLiteral("按完成日期只支持普通日、周、月间隔"));
    if (explicitDates) {
        if (spec.basis != RecurrenceSpec::Basis::DueDate
            || spec.explicitDates.isEmpty() || spec.endMode == RecurrenceSpec::EndMode::Count
            || !spec.weekdays.isEmpty() || !spec.monthDays.isEmpty()
            || spec.monthMode != RecurrenceSpec::MonthMode::Days
            || spec.ordinal != 1 || spec.ordinalWeekday != Qt::Monday
            || spec.yearMode != RecurrenceSpec::YearMode::Date || spec.month != 1)
            return fail(error, QStringLiteral("自选日期规则包含不适用的周期参数"));
        for (int i = 0; i < spec.explicitDates.size(); ++i)
            if (!spec.explicitDates.at(i).isValid()
                || (i > 0 && spec.explicitDates.at(i - 1) >= spec.explicitDates.at(i)))
                return fail(error, QStringLiteral("自选日期必须有效且严格递增"));
    } else if (!spec.explicitDates.isEmpty()) {
        return fail(error, QStringLiteral("周期规则不能包含自选日期"));
    }
    if (lunarMonthly || lunarYearly) {
        if (spec.basis != RecurrenceSpec::Basis::DueDate || spec.interval != 1
            || spec.frequency != (lunarMonthly ? RecurrenceSpec::Frequency::Monthly : RecurrenceSpec::Frequency::Yearly)
            || !spec.weekdays.isEmpty() || !spec.monthDays.isEmpty()
            || spec.monthMode != RecurrenceSpec::MonthMode::Days
            || spec.ordinal != 1 || spec.ordinalWeekday != Qt::Monday
            || spec.yearMode != RecurrenceSpec::YearMode::Date
            || (lunarMonthly && spec.month != 1))
            return fail(error, QStringLiteral("农历规则包含不适用的公历参数"));
    } else {
        if (spec.lunarLeapMonthRepeat)
            return fail(error, QStringLiteral("非农历规则不能启用闰月重复"));
        const bool daily = spec.frequency == RecurrenceSpec::Frequency::Daily;
        const bool weekly = spec.frequency == RecurrenceSpec::Frequency::Weekly;
        const bool monthly = spec.frequency == RecurrenceSpec::Frequency::Monthly;
        const bool yearly = spec.frequency == RecurrenceSpec::Frequency::Yearly;
        if (!daily && !weekly && !monthly && !yearly)
            return fail(error, QStringLiteral("重复频率无效"));
        if ((daily && (!spec.weekdays.isEmpty() || !spec.monthDays.isEmpty()
                      || spec.monthMode != RecurrenceSpec::MonthMode::Days
                      || spec.ordinal != 1 || spec.ordinalWeekday != Qt::Monday
                      || spec.yearMode != RecurrenceSpec::YearMode::Date || spec.month != 1))
            || (weekly && (!spec.monthDays.isEmpty() || spec.monthMode != RecurrenceSpec::MonthMode::Days
                           || spec.ordinal != 1 || spec.ordinalWeekday != Qt::Monday
                           || spec.yearMode != RecurrenceSpec::YearMode::Date || spec.month != 1))
            || (monthly && (!spec.weekdays.isEmpty() || spec.yearMode != RecurrenceSpec::YearMode::Date
                            || spec.month != 1))
            || (yearly && (!spec.weekdays.isEmpty()
                           || spec.monthMode != RecurrenceSpec::MonthMode::Days
                           || (spec.yearMode == RecurrenceSpec::YearMode::Date && spec.monthDays.size() != 1)
                           || (spec.yearMode == RecurrenceSpec::YearMode::Weekday && !spec.monthDays.isEmpty()))))
            return fail(error, QStringLiteral("重复频率包含不适用的组合参数"));
        if (monthly && spec.monthMode == RecurrenceSpec::MonthMode::Days && spec.monthDays.isEmpty()
            && spec.basis == RecurrenceSpec::Basis::DueDate)
            return fail(error, QStringLiteral("按月重复至少选择一个日期"));
        if (monthly && spec.monthMode != RecurrenceSpec::MonthMode::Days && !spec.monthDays.isEmpty())
            return fail(error, QStringLiteral("按月星期或工作日规则不能包含固定日期"));
        if (monthly && spec.monthMode == RecurrenceSpec::MonthMode::Weekday
            && (spec.ordinal == 0 || spec.ordinalWeekday < Qt::Monday || spec.ordinalWeekday > Qt::Sunday))
            return fail(error, QStringLiteral("按月星期参数无效"));
        if (yearly && spec.yearMode == RecurrenceSpec::YearMode::Weekday
            && (spec.ordinal == 0 || spec.ordinalWeekday < Qt::Monday || spec.ordinalWeekday > Qt::Sunday))
            return fail(error, QStringLiteral("按年星期参数无效"));
    }
    if (error) error->clear();
    return true;
}

QString recurrenceSummary(const RecurrenceSpec& spec) {
    if (spec.kind == RecurrenceSpec::Kind::ExplicitDates) return QStringLiteral("按自选日期");
    if (spec.kind == RecurrenceSpec::Kind::LunarMonthly) return QStringLiteral("农历每月");
    if (spec.kind == RecurrenceSpec::Kind::LunarYearly) return QStringLiteral("农历每年");
    const QString basis = spec.basis == RecurrenceSpec::Basis::CompletionDate
        ? QStringLiteral("完成后") : QStringLiteral("每");
    const QString unit = spec.frequency == RecurrenceSpec::Frequency::Daily ? QStringLiteral("天")
        : spec.frequency == RecurrenceSpec::Frequency::Weekly ? QStringLiteral("周")
        : spec.frequency == RecurrenceSpec::Frequency::Monthly ? QStringLiteral("月") : QStringLiteral("年");
    if (spec.interval == 1 && spec.basis == RecurrenceSpec::Basis::DueDate) return basis + unit;
    return QStringLiteral("%1%2%3").arg(basis).arg(spec.interval).arg(unit);
}

QString recurrenceRuleText(const RecurrenceSpec& spec) {
    const QJsonObject object = recurrenceToJson(spec);
    return object.isEmpty() ? QString()
        : QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool recurrenceFromRuleText(const QString& text, RecurrenceSpec* spec, QString* error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !recurrenceFromJson(document.object(), spec, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("重复规则不是有效的 JSON 对象");
        return false;
    }
    return recurrenceRuleText(*spec) == text || fail(error, QStringLiteral("重复规则未使用规范序列化"));
}

QString recurrenceFrequencyName(const RecurrenceSpec& spec) {
    if (!isValidRecurrence(spec)) return QString();
    QString frequency;
    switch (spec.frequency) {
        case RecurrenceSpec::Frequency::Daily: frequency = QStringLiteral("DAILY"); break;
        case RecurrenceSpec::Frequency::Weekly: frequency = QStringLiteral("WEEKLY"); break;
        case RecurrenceSpec::Frequency::Monthly: frequency = QStringLiteral("MONTHLY"); break;
        case RecurrenceSpec::Frequency::Yearly: frequency = QStringLiteral("YEARLY"); break;
    }
    return QStringLiteral("FREQ=%1;INTERVAL=%2").arg(frequency).arg(spec.interval);
}

} // namespace kl
