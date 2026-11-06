#ifndef KYLINNOTE_TASKCODEC_H
#define KYLINNOTE_TASKCODEC_H

#include "Task.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <cmath>
#include <limits>

namespace kn {
namespace taskcodec {

inline bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

inline bool validDate(const QDateTime &date)
{
    return date.isNull() || (date.isValid() && date.date().year() >= 1 && date.date().year() <= 9999);
}

inline bool validate(const Task &task, QString *error)
{
    if (task.id < 0 || task.groupId < 0) return fail(error, QStringLiteral("任务或分组 id 非法"));
    if (task.title.trimmed().isEmpty()) return fail(error, QStringLiteral("任务标题不能为空"));
    if (task.contentFormat != ContentFormat::Markdown && task.contentFormat != ContentFormat::Html)
        return fail(error, QStringLiteral("正文格式非法"));
    if (task.priority < 0 || task.priority > 2) return fail(error, QStringLiteral("优先级必须为 0、1 或 2"));
    if (colorToInt(task.color) < 0 || colorToInt(task.color) >= ColorCount)
        return fail(error, QStringLiteral("任务颜色非法"));
    if (layerToInt(task.layer) < 0 || layerToInt(task.layer) >= LayerCount)
        return fail(error, QStringLiteral("便签层级非法"));
    if (!isValidRecurrence(task.recurrence)) return fail(error, QStringLiteral("重复规则非法"));
    const QDateTime dates[] = {task.startAt, task.dueAt, task.reminderAt, task.notifiedAt,
                              task.createdAt, task.updatedAt, task.completedAt};
    for (const QDateTime &date : dates) {
        if (!validDate(date)) return fail(error, QStringLiteral("任务日期非法或超出 1..9999 年范围"));
    }
    if (task.startAt.isValid() && task.dueAt.isValid() && task.startAt > task.dueAt)
        return fail(error, QStringLiteral("开始时间不能晚于截止时间"));
    if (!task.reminderAt.isValid() && task.notifiedAt.isValid())
        return fail(error, QStringLiteral("没有提醒时间时不能设置已通知标记"));
    if (task.recurrence != QStringLiteral("none") && !task.dueAt.isValid() && !task.startAt.isValid())
        return fail(error, QStringLiteral("重复任务必须设置开始或截止时间"));
    if (error) error->clear();
    return true;
}

inline QString dateText(const QDateTime &date)
{
    return date.isValid() ? date.toOffsetFromUtc(date.offsetFromUtc()).toString(Qt::ISODateWithMs) : QStringLiteral("");
}

inline QJsonValue dateJson(const QDateTime &date)
{
    return date.isValid() ? QJsonValue(dateText(date)) : QJsonValue(QJsonValue::Null);
}

inline bool readDate(const QJsonValue &value, QDateTime *out, QString *error)
{
    if (value.isNull()) { *out = QDateTime(); return true; }
    if (!value.isString() || value.toString().isEmpty())
        return fail(error, QStringLiteral("时间字段必须为 ISO 8601 字符串或 null"));
    const QString text = value.toString();
    QDateTime date = QDateTime::fromString(text, Qt::ISODateWithMs);
    // 备份必须明确时区；禁止依赖导入机器的本地时区，也拒绝 QDateTime 宽松解析的短日期。
    if (!date.isValid() || !validDate(date) || text.size() < 20 || text.at(10) != QLatin1Char('T')
        || !(text.endsWith(QLatin1Char('Z')) || (text.size() >= 6
              && (text.at(text.size() - 6) == QLatin1Char('+') || text.at(text.size() - 6) == QLatin1Char('-'))
              && text.at(text.size() - 3) == QLatin1Char(':'))))
        return fail(error, QStringLiteral("备份包含无效或缺少时区的日期"));
    *out = date;
    return true;
}

// JSON 数值只接受可精确表示的整数；数据库 id 用十进制字符串序列化，避免 2^53 精度丢失。
inline bool readInteger(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)
        || number < -9007199254740991.0 || number > 9007199254740991.0) return false;
    *out = static_cast<qint64>(number);
    return true;
}

inline bool readId(const QJsonValue &value, bool allowZero, qint64 *out)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || (text.size() > 1 && text.at(0) == QLatin1Char('0'))) return false;
    for (QChar ch : text) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    bool ok = false;
    const qint64 id = text.toLongLong(&ok);
    if (!ok || id < (allowZero ? 0 : 1)) return false;
    *out = id;
    return true;
}

inline QJsonArray tagsJson(const QStringList &tags)
{
    QJsonArray result;
    for (const QString &tag : tags) result.append(tag.isNull() ? QStringLiteral("") : tag);
    return result;
}

inline QJsonArray checklistJson(const QVector<ChecklistItem> &items)
{
    QJsonArray result;
    for (const ChecklistItem &item : items) {
        QJsonObject object;
        object.insert(QStringLiteral("text"), item.text.isNull() ? QStringLiteral("") : item.text);
        object.insert(QStringLiteral("done"), item.done);
        result.append(object);
    }
    return result;
}

inline bool readLists(const QJsonValue &tagsValue, const QJsonValue &checklistValue,
                      QStringList *tags, QVector<ChecklistItem> *checklist, QString *error)
{
    if (!tagsValue.isArray() || !checklistValue.isArray())
        return fail(error, QStringLiteral("标签和检查清单必须为数组"));
    QStringList parsedTags;
    QVector<ChecklistItem> parsedItems;
    for (const QJsonValue &value : tagsValue.toArray()) {
        if (!value.isString()) return fail(error, QStringLiteral("每个标签必须是字符串"));
        parsedTags.append(value.toString());
    }
    for (const QJsonValue &value : checklistValue.toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("检查清单项必须是对象"));
        const QJsonObject item = value.toObject();
        if (!item.value(QStringLiteral("text")).isString() || !item.value(QStringLiteral("done")).isBool())
            return fail(error, QStringLiteral("检查清单项的 text/done 类型非法"));
        ChecklistItem parsed;
        parsed.text = item.value(QStringLiteral("text")).toString();
        parsed.done = item.value(QStringLiteral("done")).toBool();
        parsedItems.append(parsed);
    }
    *tags = parsedTags;
    *checklist = parsedItems;
    return true;
}

inline QJsonObject toJson(const Task &task)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::number(task.id));
    object.insert(QStringLiteral("title"), task.title);
    object.insert(QStringLiteral("description"), task.description.isNull() ? QStringLiteral("") : task.description);
    object.insert(QStringLiteral("contentFormat"), static_cast<int>(task.contentFormat));
    object.insert(QStringLiteral("groupId"), QString::number(task.groupId));
    object.insert(QStringLiteral("priority"), task.priority);
    object.insert(QStringLiteral("completed"), task.completed);
    object.insert(QStringLiteral("favorite"), task.favorite);
    object.insert(QStringLiteral("pinned"), task.pinned);
    object.insert(QStringLiteral("deleted"), task.deleted);
    object.insert(QStringLiteral("startAt"), dateJson(task.startAt));
    object.insert(QStringLiteral("dueAt"), dateJson(task.dueAt));
    object.insert(QStringLiteral("reminderAt"), dateJson(task.reminderAt));
    object.insert(QStringLiteral("notifiedAt"), dateJson(task.notifiedAt));
    object.insert(QStringLiteral("createdAt"), dateJson(task.createdAt));
    object.insert(QStringLiteral("updatedAt"), dateJson(task.updatedAt));
    object.insert(QStringLiteral("completedAt"), dateJson(task.completedAt));
    object.insert(QStringLiteral("recurrence"), task.recurrence);
    object.insert(QStringLiteral("tags"), tagsJson(task.tags));
    object.insert(QStringLiteral("checklist"), checklistJson(task.checklist));
    object.insert(QStringLiteral("color"), colorToInt(task.color));
    QJsonObject geometry;
    geometry.insert(QStringLiteral("x"), task.geometry.x());
    geometry.insert(QStringLiteral("y"), task.geometry.y());
    geometry.insert(QStringLiteral("width"), task.geometry.width());
    geometry.insert(QStringLiteral("height"), task.geometry.height());
    object.insert(QStringLiteral("geometry"), geometry);
    object.insert(QStringLiteral("zOrder"), task.zOrder);
    object.insert(QStringLiteral("layer"), layerToInt(task.layer));
    return object;
}

inline bool fromJson(const QJsonObject &object, Task *out, QString *error)
{
    Task task;
    if (!readId(object.value(QStringLiteral("id")), false, &task.id)
        || !readId(object.value(QStringLiteral("groupId")), true, &task.groupId))
        return fail(error, QStringLiteral("任务 id/groupId 必须是有效的十进制字符串"));
    const char *const strings[] = {"title", "description", "recurrence"};
    for (const char *key : strings) {
        if (!object.value(QLatin1String(key)).isString()) return fail(error, QStringLiteral("任务文本字段缺失或类型错误"));
    }
    task.title = object.value(QStringLiteral("title")).toString();
    task.description = object.value(QStringLiteral("description")).toString();
    task.recurrence = object.value(QStringLiteral("recurrence")).toString();
    qint64 number = 0;
    if (!readInteger(object.value(QStringLiteral("contentFormat")), 0, 1, &number)) return fail(error, QStringLiteral("正文格式非法"));
    task.contentFormat = static_cast<ContentFormat>(number);
    if (!readInteger(object.value(QStringLiteral("priority")), 0, 2, &number)) return fail(error, QStringLiteral("优先级非法"));
    task.priority = static_cast<int>(number);
    if (!readInteger(object.value(QStringLiteral("color")), 0, ColorCount - 1, &number)) return fail(error, QStringLiteral("颜色非法"));
    task.color = static_cast<NoteColor>(number);
    if (!readInteger(object.value(QStringLiteral("layer")), 0, LayerCount - 1, &number)) return fail(error, QStringLiteral("层级非法"));
    task.layer = static_cast<NoteLayer>(number);
    if (!readInteger(object.value(QStringLiteral("zOrder")), std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), &number))
        return fail(error, QStringLiteral("zOrder 非法"));
    task.zOrder = static_cast<int>(number);
    const char *const boolKeys[] = {"completed", "favorite", "pinned", "deleted"};
    bool *const boolFields[] = {&task.completed, &task.favorite, &task.pinned, &task.deleted};
    for (int i = 0; i < 4; ++i) {
        const QJsonValue value = object.value(QLatin1String(boolKeys[i]));
        if (!value.isBool()) return fail(error, QStringLiteral("任务布尔字段缺失或类型错误"));
        *boolFields[i] = value.toBool();
    }
    const char *const dateKeys[] = {"startAt", "dueAt", "reminderAt", "notifiedAt", "createdAt", "updatedAt", "completedAt"};
    QDateTime *const dateFields[] = {&task.startAt, &task.dueAt, &task.reminderAt, &task.notifiedAt,
                                    &task.createdAt, &task.updatedAt, &task.completedAt};
    for (int i = 0; i < 7; ++i) {
        if (!readDate(object.value(QLatin1String(dateKeys[i])), dateFields[i], error)) return false;
    }
    if (!object.value(QStringLiteral("geometry")).isObject()) return fail(error, QStringLiteral("geometry 必须是对象"));
    const QJsonObject geometry = object.value(QStringLiteral("geometry")).toObject();
    const char *const geometryKeys[] = {"x", "y", "width", "height"};
    int geometryValues[4];
    for (int i = 0; i < 4; ++i) {
        if (!readInteger(geometry.value(QLatin1String(geometryKeys[i])), std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), &number))
            return fail(error, QStringLiteral("窗口几何必须是整数"));
        geometryValues[i] = static_cast<int>(number);
    }
    // QRect 端点用 int 表示，拒绝端点溢出，避免构造时发生有符号溢出。
    const qint64 right = qint64(geometryValues[0]) + geometryValues[2] - 1;
    const qint64 bottom = qint64(geometryValues[1]) + geometryValues[3] - 1;
    if (right < std::numeric_limits<int>::min() || right > std::numeric_limits<int>::max()
        || bottom < std::numeric_limits<int>::min() || bottom > std::numeric_limits<int>::max())
        return fail(error, QStringLiteral("窗口几何端点溢出"));
    task.geometry = QRect(geometryValues[0], geometryValues[1], geometryValues[2], geometryValues[3]);
    if (!readLists(object.value(QStringLiteral("tags")), object.value(QStringLiteral("checklist")), &task.tags, &task.checklist, error)
        || !validate(task, error)) return false;
    if (task.completed != task.completedAt.isValid())
        return fail(error, QStringLiteral("完成状态与完成时间不一致"));
    *out = task;
    return true;
}

} // namespace taskcodec
} // namespace kn

#endif // KYLINNOTE_TASKCODEC_H
