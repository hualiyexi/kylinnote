#include "ReminderRule.h"

#include "Entity.h"
#include <QTimeZone>

#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

namespace kl {
namespace {

bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}

QString anchorName(ReminderSpec::Anchor value) {
    switch (value) {
        case ReminderSpec::Anchor::Schedule: return QStringLiteral("schedule");
        case ReminderSpec::Anchor::Start: return QStringLiteral("start");
        case ReminderSpec::Anchor::End: return QStringLiteral("end");
        case ReminderSpec::Anchor::Absolute: return QStringLiteral("absolute");
    }
    return QString();
}

bool readAnchor(const QString& value, ReminderSpec::Anchor* result) {
    if (value == QLatin1String("schedule")) *result = ReminderSpec::Anchor::Schedule;
    else if (value == QLatin1String("start")) *result = ReminderSpec::Anchor::Start;
    else if (value == QLatin1String("end")) *result = ReminderSpec::Anchor::End;
    else if (value == QLatin1String("absolute")) *result = ReminderSpec::Anchor::Absolute;
    else return false;
    return true;
}

QString unitName(ReminderSpec::Unit value) {
    switch (value) {
        case ReminderSpec::Unit::Minutes: return QStringLiteral("minutes");
        case ReminderSpec::Unit::Hours: return QStringLiteral("hours");
        case ReminderSpec::Unit::Days: return QStringLiteral("days");
        case ReminderSpec::Unit::Weeks: return QStringLiteral("weeks");
    }
    return QString();
}

bool readUnit(const QString& value, ReminderSpec::Unit* result) {
    if (value == QLatin1String("minutes")) *result = ReminderSpec::Unit::Minutes;
    else if (value == QLatin1String("hours")) *result = ReminderSpec::Unit::Hours;
    else if (value == QLatin1String("days")) *result = ReminderSpec::Unit::Days;
    else if (value == QLatin1String("weeks")) *result = ReminderSpec::Unit::Weeks;
    else return false;
    return true;
}

QString timeText(const QTime& time) {
    return time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QString();
}

} // namespace

QJsonObject reminderToJson(const ReminderSpec& spec) {
    if (!isValidReminder(spec)) return QJsonObject();
    QJsonObject object;
    object.insert(QStringLiteral("version"), 1);
    object.insert(QStringLiteral("anchor"), anchorName(spec.anchor));
    object.insert(QStringLiteral("unit"), unitName(spec.unit));
    object.insert(QStringLiteral("offset"), spec.offset);
    object.insert(QStringLiteral("allDay"), spec.allDay);
    object.insert(QStringLiteral("dayOffset"), spec.dayOffset);
    object.insert(QStringLiteral("localTime"), timeText(spec.localTime));
    object.insert(QStringLiteral("absoluteAtUtcMs"), spec.absoluteAt.isValid()
        ? static_cast<double>(spec.absoluteAt.toUTC().toMSecsSinceEpoch()) : QJsonValue(QJsonValue::Null));
    return object;
}

bool reminderFromJson(const QJsonObject& object, ReminderSpec* spec, QString* error) {
    const QSet<QString> known = QSet<QString>::fromList(QStringList()
        << QStringLiteral("version") << QStringLiteral("anchor") << QStringLiteral("unit")
        << QStringLiteral("offset") << QStringLiteral("allDay") << QStringLiteral("dayOffset")
        << QStringLiteral("localTime") << QStringLiteral("absoluteAtUtcMs"));
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!known.contains(it.key())) return fail(error, QStringLiteral("提醒规则含未支持字段"));
    if (!spec || object.value(QStringLiteral("version")).toInt() != 1
        || object.value(QStringLiteral("version")).toDouble() != 1.0
        || !object.value(QStringLiteral("anchor")).isString()
        || !object.value(QStringLiteral("unit")).isString()
        || !object.value(QStringLiteral("offset")).isDouble()
        || !object.value(QStringLiteral("allDay")).isBool()
        || !object.value(QStringLiteral("dayOffset")).isDouble()
        || !object.value(QStringLiteral("localTime")).isString())
        return fail(error, QStringLiteral("提醒规则字段不完整"));
    ReminderSpec value;
    if (!readAnchor(object.value(QStringLiteral("anchor")).toString(), &value.anchor)
        || !readUnit(object.value(QStringLiteral("unit")).toString(), &value.unit))
        return fail(error, QStringLiteral("提醒锚点或单位无效"));
    value.offset = object.value(QStringLiteral("offset")).toInt(-1);
    value.dayOffset = object.value(QStringLiteral("dayOffset")).toInt(-1);
    if (value.offset != object.value(QStringLiteral("offset")).toDouble()
        || value.dayOffset != object.value(QStringLiteral("dayOffset")).toDouble())
        return fail(error, QStringLiteral("提醒偏移必须为整数"));
    value.allDay = object.value(QStringLiteral("allDay")).toBool();
    value.localTime = QTime::fromString(object.value(QStringLiteral("localTime")).toString(),
                                       QStringLiteral("HH:mm"));
    const QJsonValue absolute = object.value(QStringLiteral("absoluteAtUtcMs"));
    if (absolute.isDouble()) {
        const double raw = absolute.toDouble();
        const qint64 millis = static_cast<qint64>(raw);
        if (raw != static_cast<double>(millis)) return fail(error, QStringLiteral("提醒绝对时刻无效"));
        value.absoluteAt = QDateTime::fromMSecsSinceEpoch(millis, Qt::UTC);
    } else if (!absolute.isNull()) return fail(error, QStringLiteral("提醒绝对时刻类型无效"));
    return isValidReminder(value, error) ? (*spec = value, true) : false;
}

bool isValidReminder(const ReminderSpec& spec, QString* error) {
    if (spec.offset < 0 || spec.offset > 100000 || spec.dayOffset < 0 || spec.dayOffset > 3650
        || !spec.localTime.isValid() || spec.localTime.second() != 0 || spec.localTime.msec() != 0
        || anchorName(spec.anchor).isEmpty() || unitName(spec.unit).isEmpty())
        return fail(error, QStringLiteral("提醒偏移或本地时刻无效"));
    if (spec.allDay && spec.offset != 0)
        return fail(error, QStringLiteral("全天提醒只使用日期偏移和本地时刻"));
    if (spec.anchor == ReminderSpec::Anchor::Absolute) {
        if (!spec.absoluteAt.isValid() || spec.allDay || spec.offset != 0 || spec.dayOffset != 0)
            return fail(error, QStringLiteral("自定义绝对提醒不能包含相对偏移"));
    } else if (spec.absoluteAt.isValid()) {
        return fail(error, QStringLiteral("相对提醒不能同时指定绝对时刻"));
    }
    if (!spec.allDay && spec.dayOffset != 0)
        return fail(error, QStringLiteral("非全天提醒不能使用天数偏移"));
    if (error) error->clear();
    return true;
}

QString reminderSummary(const ReminderSpec& spec) {
    if (spec.anchor == ReminderSpec::Anchor::Absolute) return QStringLiteral("自定义提醒");
    const QString unit = spec.unit == ReminderSpec::Unit::Minutes ? QStringLiteral("分钟")
        : spec.unit == ReminderSpec::Unit::Hours ? QStringLiteral("小时")
        : spec.unit == ReminderSpec::Unit::Days ? QStringLiteral("天") : QStringLiteral("周");
    if (spec.allDay) {
        const QString time = timeText(spec.localTime);
        if (spec.dayOffset == 0) return QStringLiteral("当天(%1)").arg(time);
        if (spec.dayOffset % 7 == 0)
            return QStringLiteral("提前%1周(%2)").arg(spec.dayOffset / 7).arg(time);
        return QStringLiteral("提前%1天(%2)").arg(spec.dayOffset).arg(time);
    }
    if (spec.offset == 0) return spec.anchor == ReminderSpec::Anchor::End
        ? QStringLiteral("结束时") : QStringLiteral("准时");
    return QStringLiteral("提前%1%2").arg(spec.offset).arg(unit);
}

QString reminderRuleText(const ReminderSpec& spec) {
    const QJsonObject object = reminderToJson(spec);
    return object.isEmpty() ? QString()
        : QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

bool reminderFromRuleText(const QString& text, ReminderSpec* spec, QString* error) {
    if (text.isEmpty()) return fail(error, QStringLiteral("提醒规则不能为空"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !reminderFromJson(document.object(), spec, error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("提醒规则不是有效的 JSON 对象");
        return false;
    }
    return reminderRuleText(*spec) == text || fail(error, QStringLiteral("提醒规则未使用规范序列化"));
}

bool resolveReminder(const ReminderSpec& spec, const Entity& entity,
                     QDateTime* triggerAt, QString* error) {
    if (!triggerAt || !isValidReminder(spec, error))
        return fail(error, QStringLiteral("提醒输出不能为空或规则无效"));
    if (spec.anchor == ReminderSpec::Anchor::Absolute) {
        *triggerAt = spec.absoluteAt.toUTC();
        if (error) error->clear();
        return true;
    }
    const QTimeZone zone(entity.timeZone.toUtf8());
    if (!zone.isValid()) return fail(error, QStringLiteral("提醒时区无效"));
    const bool start = spec.anchor == ReminderSpec::Anchor::Start
        || (spec.anchor == ReminderSpec::Anchor::Schedule && entity.hasScheduleRange);
    QDateTime local;
    if (entity.allDayDate.isValid()) {
        if (!spec.allDay) return fail(error, QStringLiteral("全天计划不能保存时刻相对提醒"));
        QDate date = start ? entity.allDayDate
            : (entity.allDayEndDate.isValid() ? entity.allDayEndDate : entity.allDayDate);
        date = date.addDays(-spec.dayOffset);
        local = QDateTime(date, spec.localTime, zone);
    } else {
        if (spec.allDay) return fail(error, QStringLiteral("时刻计划不能保存全天提醒"));
        local = (start ? entity.startAt : entity.dueAt).toTimeZone(zone);
        if (!local.isValid()) return fail(error, QStringLiteral("提醒对应的计划时刻不存在"));
        switch (spec.unit) {
            case ReminderSpec::Unit::Minutes:
            case ReminderSpec::Unit::Hours: {
                const qint64 multiplier = spec.unit == ReminderSpec::Unit::Minutes ? 60 : 3600;
                local = local.addSecs(-static_cast<qint64>(spec.offset) * multiplier);
                break;
            }
            case ReminderSpec::Unit::Days: local = local.addDays(-spec.offset); break;
            case ReminderSpec::Unit::Weeks: local = local.addDays(-spec.offset * 7); break;
        }
    }
    if (!local.isValid()) return fail(error, QStringLiteral("提醒时间超出有效范围"));
    *triggerAt = local.toUTC();
    if (error) error->clear();
    return true;
}

} // namespace kl
