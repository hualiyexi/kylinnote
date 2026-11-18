#pragma once

#include <QDate>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace kl {

// 可持久化的完整重复配置；不恢复节假日/记忆法规则。
struct RecurrenceSpec {
    enum class Kind { Gregorian, LunarMonthly, LunarYearly, ExplicitDates };
    enum class Basis { DueDate, CompletionDate };
    enum class Frequency { Daily, Weekly, Monthly, Yearly };
    enum class MonthMode { Days, Weekday, Workday };
    enum class YearMode { Date, Weekday };
    enum class EndMode { Never, Date, Count };

    Kind kind = Kind::Gregorian;
    Basis basis = Basis::DueDate;
    Frequency frequency = Frequency::Weekly;
    int interval = 1;
    QVector<int> weekdays;       // Qt Monday=1 ... Sunday=7
    QVector<int> monthDays;      // 1..31, -1=月末
    MonthMode monthMode = MonthMode::Days;
    int ordinal = 1;             // 1..5, -1=最后
    int ordinalWeekday = Qt::Monday;
    bool firstWorkday = true;    // 月首/月末普通工作日，绝不调整法定节假日
    YearMode yearMode = YearMode::Date;
    int month = 1;
    QVector<QDate> explicitDates; // 有序、去重，至少一项
    EndMode endMode = EndMode::Never;
    QDate endDate;
    int endCount = 2;            // 2..200，自选日期不支持 Count
    int weekStart = Qt::Monday;
    bool lunarLeapMonthRepeat = false;
};

// 严格、确定性的 v1 JSON。未知字段/非法组合拒绝，不静默丢字段。
QJsonObject recurrenceToJson(const RecurrenceSpec& spec);
bool recurrenceFromJson(const QJsonObject& object, RecurrenceSpec* spec, QString* error = nullptr);
bool isValidRecurrence(const RecurrenceSpec& spec, QString* error = nullptr);
QString recurrenceSummary(const RecurrenceSpec& spec);
QString recurrenceRuleText(const RecurrenceSpec& spec);
bool recurrenceFromRuleText(const QString& text, RecurrenceSpec* spec, QString* error = nullptr);
// 仅给仍读取简单频率的视图提供非权威摘要；绝不参与下一次出现计算。
QString recurrenceFrequencyName(const RecurrenceSpec& spec);

} // namespace kl
