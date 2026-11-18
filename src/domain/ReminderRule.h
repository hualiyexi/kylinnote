#pragma once

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QTime>

namespace kl {

struct Entity;

// 正常一次性提醒的相对规则；没有持续提醒或重复投递策略。
struct ReminderSpec {
    enum class Anchor { Schedule, Start, End, Absolute };
    enum class Unit { Minutes, Hours, Days, Weeks };
    Anchor anchor = Anchor::Schedule;
    Unit unit = Unit::Minutes;
    int offset = 0;              // 提前量，非负；0=准时
    bool allDay = false;
    int dayOffset = 0;           // 全天按本地日期提前天数
    QTime localTime = QTime(9, 0);
    QDateTime absoluteAt;        // Absolute 时必填，UTC
};

QJsonObject reminderToJson(const ReminderSpec& spec);
bool reminderFromJson(const QJsonObject& object, ReminderSpec* spec, QString* error = nullptr);
bool isValidReminder(const ReminderSpec& spec, QString* error = nullptr);
QString reminderSummary(const ReminderSpec& spec);
QString reminderRuleText(const ReminderSpec& spec);
bool reminderFromRuleText(const QString& text, ReminderSpec* spec, QString* error = nullptr);
bool resolveReminder(const ReminderSpec& spec, const Entity& entity,
                     QDateTime* triggerAt, QString* error = nullptr);

} // namespace kl
