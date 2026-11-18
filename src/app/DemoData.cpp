#include "DemoData.h"

#include "../db/Database.h"
#include "../domain/AttachmentCodec.h"
#include "../domain/BackupCodec.h"
#include "../domain/EntityCodec.h"
#include "../domain/InternalLink.h"
#include "../domain/RecurrenceEngine.h"
#include "../domain/ReminderRule.h"
#include "SingleInstanceGuard.h"
#include "TaskStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <limits>

namespace kl {
namespace demo {
namespace {

// ---------------------------------------------------------------- 共享常量
// populate 与 verify 共用的外观/锚点常量；两边必须保持一致。

struct ListExpectation {
    const char *name;
    const char *color;
    const char *icon;
    bool archived;
    bool noteKind;
    bool inFolder;      // true = 放进文件夹（前 3 个对应工作/工作/学习）
};

const ListExpectation kLists[] = {
    {"产品开发", "#3B82F6", "\U0001F680", false, false, true},   // 🚀
    {"市场调研", "#F59E0B", "\U0001F4CA", false, false, true},   // 📊
    {"家庭事务", "#10B981", "\U0001F3E0", false, false, true},   // 🏠
    {"健身计划", "#EF4444", "\U0001F3C3", false, false, false},  // 🏃
    {"阅读笔记", "#8B5CF6", "\U0001F4D6", false, false, false},  // 📖
    {"便签本", "#06B6D4", "\U0001F5D2", false, true, false},     // 🗒️
    {"旧项目归档", "#64748B", "\U0001F5C4", true, false, true},  // 🗄️
};
const int kListCount = sizeof(kLists) / sizeof(kLists[0]);
const char *const kFolders[] = {"工作", "生活", "学习"};
const char *const kFolderIcons[] = {"\U0001F4BC", "\U0001F3E1", "\U0001F4DA"}; // 💼 🏡 📚
const int kFolderCount = 3;

// 日程锚点（相对 referenceDate 的天数与本地时刻，Asia/Shanghai）。
const int kOverdueAllDayOffset = -2;
const int kOverdueTimedOffset = -1;
const int kMultidayAllDayStart = 2, kMultidayAllDayEnd = 5;
const int kMultidayTimedStart = 3, kMultidayTimedEnd = 5;
const int kFutureTimedOffset = 10;
const int kFutureAllDayOffset = 21;

// 24x16 三色条带与 16x16 单色合成 PNG（离线字节，不依赖 GUI 生成图像）。
const char *const kPngStripesBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAABgAAAAQCAIAAACDRijCAAAAJ0lEQVR42mPQK9lKFcQwahAdDfqw"
    "1o8qaNQgIgyysXGgCho1iDACAD3n/Wj7iZkjAAAAAElFTkSuQmCC";
const char *const kPngSolidBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAFklEQVR42mOImxJHEmIY1TCqYfhq"
    "AAChLlAQT4bRaQAAAABJRU5ErkJggg==";

const char *const kCsvContent =
    "维度,本产品,竞品A,竞品B\n"
    "本地离线,完全支持,部分支持,完全支持\n"
    "农历重复,支持,不支持,不支持\n"
    "附件管理,支持,支持,不支持\n";
const char *const kNotesText =
    "访谈要点（合成演示数据）：\n"
    "1. 期望离线优先，联网仅同步配置。\n"
    "2. 农历生日重复是高频需求。\n"
    "3. 桌面便签希望支持多分层。\n";
const char *const kPackingText =
    "行李清单：\n"
    "- 证件与机票\n"
    "- 充电器与转换插头\n"
    "- 常用药品\n";

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

// 演示工具进程内临时切换 KYLINLIST_DATA_DIR；任何返回路径都恢复原值（含未设置）。
class ScopedDataDirEnv
{
public:
    explicit ScopedDataDirEnv(const QString &value)
        : m_previous(qgetenv("KYLINLIST_DATA_DIR"))
    {
        qputenv("KYLINLIST_DATA_DIR", value.toUtf8());
    }
    ~ScopedDataDirEnv()
    {
        if (m_previous.isEmpty()) qunsetenv("KYLINLIST_DATA_DIR");
        else qputenv("KYLINLIST_DATA_DIR", m_previous);
    }
    ScopedDataDirEnv(const ScopedDataDirEnv &) = delete;
    ScopedDataDirEnv &operator=(const ScopedDataDirEnv &) = delete;

private:
    QByteArray m_previous;
};

// 任一返回路径都关闭单例数据库连接，锁由 SingleInstanceGuard 析构释放。
struct DatabaseCloser
{
    ~DatabaseCloser() { Database::instance().close(); }
};

QDateTime wallClock(const QDate &date, int hour, int minute)
{
    return fromAppTime(QDateTime(date, QTime(hour, minute)));
}

QByteArray decodeBase64(const char *text)
{
    return QByteArray::fromBase64(QByteArray(text));
}

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonArray toJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values) array.append(value);
    return array;
}

bool readIdArray(const QJsonObject &object, const QString &key, QStringList *out, QString *error)
{
    if (!object.value(key).isArray())
        return fail(error, QStringLiteral("manifest ids.%1 必须为字符串数组").arg(key));
    QStringList values;
    const QJsonArray array = object.value(key).toArray();
    for (const QJsonValue &value : array) {
        if (!value.isString() || !isUuid(value.toString()))
            return fail(error, QStringLiteral("manifest ids.%1 含非法 UUID").arg(key));
        values.append(value.toString());
    }
    *out = values;
    return true;
}

bool readIdValue(const QJsonObject &object, const QString &key, QString *out, QString *error)
{
    const QJsonValue value = object.value(key);
    if (!value.isString())
        return fail(error, QStringLiteral("manifest ids.%1 必须为字符串").arg(key));
    const QString text = value.toString();
    if (!text.isEmpty() && !isUuid(text))
        return fail(error, QStringLiteral("manifest ids.%1 不是 UUID").arg(key));
    *out = text;
    return true;
}

bool readCount(const QJsonObject &object, const QString &key, int *out, QString *error)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble())
        return fail(error, QStringLiteral("manifest counts.%1 必须为整数").arg(key));
    const double raw = value.toDouble();
    const int number = static_cast<int>(raw);
    if (number < 0 || raw != static_cast<double>(number))
        return fail(error, QStringLiteral("manifest counts.%1 非法").arg(key));
    *out = number;
    return true;
}

} // namespace

// ---------------------------------------------------------------- manifest

QString manifestToJson(const Manifest &value)
{
    QJsonObject root;
    root.insert(QStringLiteral("format"), value.format);
    root.insert(QStringLiteral("version"), value.version);
    QJsonObject ownership;
    ownership.insert(QStringLiteral("marker"), value.marker);
    ownership.insert(QStringLiteral("owned"), value.owned);
    ownership.insert(QStringLiteral("canReset"), value.canReset);
    ownership.insert(QStringLiteral("policy"), ownershipPolicy());
    root.insert(QStringLiteral("ownership"), ownership);
    QJsonObject generator;
    generator.insert(QStringLiteral("product"), QStringLiteral("kylinlist"));
    generator.insert(QStringLiteral("version"), value.generatorVersion);
    generator.insert(QStringLiteral("mode"), value.mode);
    root.insert(QStringLiteral("generator"), generator);
    root.insert(QStringLiteral("generatedAtUtcMs"), static_cast<double>(utcMs(value.generatedAtUtc)));
    root.insert(QStringLiteral("referenceDate"), value.referenceDate.toString(Qt::ISODate));
    root.insert(QStringLiteral("dataDir"), value.dataDir);
    QJsonObject counts;
    counts.insert(QStringLiteral("folders"), value.folders);
    counts.insert(QStringLiteral("lists"), value.lists);
    counts.insert(QStringLiteral("sections"), value.sections);
    counts.insert(QStringLiteral("tasks"), value.tasks);
    counts.insert(QStringLiteral("notes"), value.notes);
    counts.insert(QStringLiteral("entities"), value.entities);
    counts.insert(QStringLiteral("attachments"), value.attachments);
    counts.insert(QStringLiteral("reminders"), value.reminders);
    counts.insert(QStringLiteral("recurrenceSeries"), value.recurrenceSeries);
    counts.insert(QStringLiteral("desktopNotes"), value.desktopNotes);
    counts.insert(QStringLiteral("events"), value.events);
    root.insert(QStringLiteral("counts"), counts);
    QJsonObject ids;
    ids.insert(QStringLiteral("inboxListId"), value.inboxListId);
    ids.insert(QStringLiteral("folderIds"), toJsonArray(value.folderIds));
    ids.insert(QStringLiteral("listIds"), toJsonArray(value.listIds));
    ids.insert(QStringLiteral("sectionIds"), toJsonArray(value.sectionIds));
    ids.insert(QStringLiteral("taskIds"), toJsonArray(value.taskIds));
    ids.insert(QStringLiteral("noteIds"), toJsonArray(value.noteIds));
    ids.insert(QStringLiteral("archivedListIds"), toJsonArray(value.archivedListIds));
    ids.insert(QStringLiteral("completedTaskIds"), toJsonArray(value.completedTaskIds));
    ids.insert(QStringLiteral("abandonedTaskIds"), toJsonArray(value.abandonedTaskIds));
    ids.insert(QStringLiteral("trashedTaskIds"), toJsonArray(value.trashedTaskIds));
    ids.insert(QStringLiteral("recurringSourceId"), value.recurringSourceId);
    ids.insert(QStringLiteral("recurringNextId"), value.recurringNextId);
    QJsonObject recurring;
    recurring.insert(QStringLiteral("daily"), value.recurringDailyId);
    recurring.insert(QStringLiteral("weekly"), value.recurringWeeklyId);
    recurring.insert(QStringLiteral("monthly"), value.recurringMonthlyId);
    recurring.insert(QStringLiteral("yearly"), value.recurringYearlyId);
    recurring.insert(QStringLiteral("lunarMonthly"), value.recurringLunarMonthlyId);
    recurring.insert(QStringLiteral("lunarYearly"), value.recurringLunarYearlyId);
    ids.insert(QStringLiteral("recurring"), recurring);
    QJsonObject hierarchy;
    hierarchy.insert(QStringLiteral("parent"), value.hierarchyParentId);
    hierarchy.insert(QStringLiteral("child"), value.hierarchyChildId);
    hierarchy.insert(QStringLiteral("grandchild"), value.hierarchyGrandchildId);
    ids.insert(QStringLiteral("hierarchy"), hierarchy);
    QJsonObject links;
    links.insert(QStringLiteral("markdownLinkId"), value.markdownLinkId);
    links.insert(QStringLiteral("markdownTargetId"), value.markdownTargetId);
    links.insert(QStringLiteral("noteLinkId"), value.noteLinkId);
    links.insert(QStringLiteral("noteLinkTargetId"), value.noteLinkTargetId);
    ids.insert(QStringLiteral("links"), links);
    QJsonObject reminderCases;
    reminderCases.insert(QStringLiteral("relative"), value.reminderRelativeId);
    reminderCases.insert(QStringLiteral("absolute"), value.reminderAbsoluteId);
    reminderCases.insert(QStringLiteral("allDay"), value.reminderAllDayId);
    reminderCases.insert(QStringLiteral("note"), value.reminderNoteId);
    reminderCases.insert(QStringLiteral("scheduleAnchor"), value.reminderScheduleAnchorId);
    ids.insert(QStringLiteral("reminders"), reminderCases);
    ids.insert(QStringLiteral("attachmentHeavyEntityId"), value.attachmentHeavyEntityId);
    ids.insert(QStringLiteral("attachmentEntityIds"), toJsonArray(value.attachmentEntityIds));
    ids.insert(QStringLiteral("desktopEntityIds"), toJsonArray(value.desktopEntityIds));
    ids.insert(QStringLiteral("pinnedTaskId"), value.pinnedTaskId);
    QJsonObject schedules;
    schedules.insert(QStringLiteral("overdueAllDay"), value.scheduleOverdueAllDayId);
    schedules.insert(QStringLiteral("overdueTimed"), value.scheduleOverdueTimedId);
    schedules.insert(QStringLiteral("todayRange"), value.scheduleTodayRangeId);
    schedules.insert(QStringLiteral("overnight"), value.scheduleOvernightId);
    schedules.insert(QStringLiteral("midnight"), value.scheduleMidnightId);
    schedules.insert(QStringLiteral("tomorrowAllDay"), value.scheduleTomorrowAllDayId);
    schedules.insert(QStringLiteral("multidayAllDay"), value.scheduleMultidayAllDayId);
    schedules.insert(QStringLiteral("sameDayAllDay"), value.scheduleSameDayAllDayId);
    schedules.insert(QStringLiteral("multidayTimed"), value.scheduleMultidayTimedId);
    schedules.insert(QStringLiteral("futureTimed"), value.scheduleFutureTimedId);
    schedules.insert(QStringLiteral("futureAllDay"), value.scheduleFutureAllDayId);
    ids.insert(QStringLiteral("schedules"), schedules);
    root.insert(QStringLiteral("ids"), ids);
    QJsonObject backup;
    backup.insert(QStringLiteral("file"), value.backupFile);
    backup.insert(QStringLiteral("bytes"), static_cast<double>(value.backupBytes));
    backup.insert(QStringLiteral("sha256"), value.backupSha256);
    root.insert(QStringLiteral("backup"), backup);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool manifestFromJson(const QByteArray &json, Manifest *out, QString *error)
{
    if (!out) return fail(error, QStringLiteral("manifest 输出不能为空"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("demo-manifest.json 不是有效 JSON 对象"));
    const QJsonObject root = document.object();
    Manifest value;
    value.format = root.value(QStringLiteral("format")).toString();
    if (value.format != manifestFormat())
        return fail(error, QStringLiteral("manifest format 不是 %1").arg(manifestFormat()));
    qint64 version = 0;
    if (!entitycodec::readInteger(root.value(QStringLiteral("version")), kManifestVersion, kManifestVersion, &version))
        return fail(error, QStringLiteral("manifest version 必须为 %1").arg(kManifestVersion));
    value.version = static_cast<int>(version);
    const QJsonObject ownership = root.value(QStringLiteral("ownership")).toObject();
    value.marker = ownership.value(QStringLiteral("marker")).toString();
    if (value.marker != ownershipMarker() || !ownership.value(QStringLiteral("owned")).isBool()
        || !ownership.value(QStringLiteral("canReset")).isBool())
        return fail(error, QStringLiteral("manifest 所有权字段缺失或不匹配"));
    value.owned = ownership.value(QStringLiteral("owned")).toBool();
    value.canReset = ownership.value(QStringLiteral("canReset")).toBool();
    if (!value.owned)
        return fail(error, QStringLiteral("manifest 声明目录不属于演示数据生成器"));
    const QJsonObject generator = root.value(QStringLiteral("generator")).toObject();
    value.generatorVersion = generator.value(QStringLiteral("version")).toString();
    value.mode = generator.value(QStringLiteral("mode")).toString();
    if (value.mode != QStringLiteral("seed-demo"))
        return fail(error, QStringLiteral("manifest generator.mode 非法"));
    qint64 stamp = 0;
    if (!entitycodec::readInteger(root.value(QStringLiteral("generatedAtUtcMs")), 1,
                                  std::numeric_limits<qint64>::max(), &stamp))
        return fail(error, QStringLiteral("manifest 生成时间非法"));
    value.generatedAtUtc = fromUtcMs(stamp);
    value.referenceDate = QDate::fromString(root.value(QStringLiteral("referenceDate")).toString(), Qt::ISODate);
    if (!value.referenceDate.isValid())
        return fail(error, QStringLiteral("manifest referenceDate 非法"));
    value.dataDir = root.value(QStringLiteral("dataDir")).toString();
    const QJsonObject counts = root.value(QStringLiteral("counts")).toObject();
    const char *const countKeys[] = {"folders", "lists", "sections", "tasks", "notes", "entities",
        "attachments", "reminders", "recurrenceSeries", "desktopNotes", "events"};
    int *const countFields[] = {&value.folders, &value.lists, &value.sections, &value.tasks,
        &value.notes, &value.entities, &value.attachments, &value.reminders,
        &value.recurrenceSeries, &value.desktopNotes, &value.events};
    for (int i = 0; i < 11; ++i)
        if (!readCount(counts, QString::fromLatin1(countKeys[i]), countFields[i], error)) return false;
    const QJsonObject ids = root.value(QStringLiteral("ids")).toObject();
    if (!readIdValue(ids, QStringLiteral("inboxListId"), &value.inboxListId, error)) return false;
    const struct { const char *key; QStringList *target; } idArrays[] = {
        {"folderIds", &value.folderIds}, {"listIds", &value.listIds}, {"sectionIds", &value.sectionIds},
        {"taskIds", &value.taskIds}, {"noteIds", &value.noteIds}, {"archivedListIds", &value.archivedListIds},
        {"completedTaskIds", &value.completedTaskIds}, {"abandonedTaskIds", &value.abandonedTaskIds},
        {"trashedTaskIds", &value.trashedTaskIds}, {"attachmentEntityIds", &value.attachmentEntityIds},
        {"desktopEntityIds", &value.desktopEntityIds}};
    for (const auto &entry : idArrays)
        if (!readIdArray(ids, QString::fromLatin1(entry.key), entry.target, error)) return false;
    if (!readIdValue(ids, QStringLiteral("recurringSourceId"), &value.recurringSourceId, error)) return false;
    if (!readIdValue(ids, QStringLiteral("recurringNextId"), &value.recurringNextId, error)) return false;
    const QJsonObject recurring = ids.value(QStringLiteral("recurring")).toObject();
    if (!readIdValue(recurring, QStringLiteral("daily"), &value.recurringDailyId, error)) return false;
    if (!readIdValue(recurring, QStringLiteral("weekly"), &value.recurringWeeklyId, error)) return false;
    if (!readIdValue(recurring, QStringLiteral("monthly"), &value.recurringMonthlyId, error)) return false;
    if (!readIdValue(recurring, QStringLiteral("yearly"), &value.recurringYearlyId, error)) return false;
    if (!readIdValue(recurring, QStringLiteral("lunarMonthly"), &value.recurringLunarMonthlyId, error)) return false;
    if (!readIdValue(recurring, QStringLiteral("lunarYearly"), &value.recurringLunarYearlyId, error)) return false;
    const QJsonObject hierarchy = ids.value(QStringLiteral("hierarchy")).toObject();
    if (!readIdValue(hierarchy, QStringLiteral("parent"), &value.hierarchyParentId, error)) return false;
    if (!readIdValue(hierarchy, QStringLiteral("child"), &value.hierarchyChildId, error)) return false;
    if (!readIdValue(hierarchy, QStringLiteral("grandchild"), &value.hierarchyGrandchildId, error)) return false;
    const QJsonObject links = ids.value(QStringLiteral("links")).toObject();
    if (!readIdValue(links, QStringLiteral("markdownLinkId"), &value.markdownLinkId, error)) return false;
    if (!readIdValue(links, QStringLiteral("markdownTargetId"), &value.markdownTargetId, error)) return false;
    if (!readIdValue(links, QStringLiteral("noteLinkId"), &value.noteLinkId, error)) return false;
    if (!readIdValue(links, QStringLiteral("noteLinkTargetId"), &value.noteLinkTargetId, error)) return false;
    const QJsonObject reminderCases = ids.value(QStringLiteral("reminders")).toObject();
    if (!readIdValue(reminderCases, QStringLiteral("relative"), &value.reminderRelativeId, error)) return false;
    if (!readIdValue(reminderCases, QStringLiteral("absolute"), &value.reminderAbsoluteId, error)) return false;
    if (!readIdValue(reminderCases, QStringLiteral("allDay"), &value.reminderAllDayId, error)) return false;
    if (!readIdValue(reminderCases, QStringLiteral("note"), &value.reminderNoteId, error)) return false;
    if (!readIdValue(reminderCases, QStringLiteral("scheduleAnchor"), &value.reminderScheduleAnchorId, error)) return false;
    if (!readIdValue(ids, QStringLiteral("attachmentHeavyEntityId"), &value.attachmentHeavyEntityId, error)) return false;
    if (!readIdValue(ids, QStringLiteral("pinnedTaskId"), &value.pinnedTaskId, error)) return false;
    const QJsonObject schedules = ids.value(QStringLiteral("schedules")).toObject();
    if (!readIdValue(schedules, QStringLiteral("overdueAllDay"), &value.scheduleOverdueAllDayId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("overdueTimed"), &value.scheduleOverdueTimedId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("todayRange"), &value.scheduleTodayRangeId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("overnight"), &value.scheduleOvernightId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("midnight"), &value.scheduleMidnightId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("tomorrowAllDay"), &value.scheduleTomorrowAllDayId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("multidayAllDay"), &value.scheduleMultidayAllDayId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("sameDayAllDay"), &value.scheduleSameDayAllDayId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("multidayTimed"), &value.scheduleMultidayTimedId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("futureTimed"), &value.scheduleFutureTimedId, error)) return false;
    if (!readIdValue(schedules, QStringLiteral("futureAllDay"), &value.scheduleFutureAllDayId, error)) return false;
    const QJsonObject backup = root.value(QStringLiteral("backup")).toObject();
    value.backupFile = backup.value(QStringLiteral("file")).toString();
    qint64 bytes = 0;
    if (value.backupFile != QStringLiteral("demo-backup.klb")
        || !entitycodec::readInteger(backup.value(QStringLiteral("bytes")), 1,
                                     attachmentcodec::kMaximumBackupContainerBytes, &bytes))
        return fail(error, QStringLiteral("manifest backup 字段非法"));
    value.backupBytes = bytes;
    value.backupSha256 = backup.value(QStringLiteral("sha256")).toString();
    if (!attachmentcodec::isSha256(value.backupSha256))
        return fail(error, QStringLiteral("manifest backup.sha256 非法"));
    *out = value;
    return true;
}

bool writeManifest(const QString &dataDir, const Manifest &value, QString *error)
{
    QSaveFile file(manifestPath(dataDir));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(error, QStringLiteral("无法写入 demo-manifest.json: ") + file.errorString());
    const QByteArray json = manifestToJson(value).toUtf8();
    if (file.write(json) != json.size() || !file.commit())
        return fail(error, QStringLiteral("写入 demo-manifest.json 失败: ") + file.errorString());
    return true;
}

bool readManifest(const QString &dataDir, Manifest *out, QString *error)
{
    QFile file(manifestPath(dataDir));
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法读取 demo-manifest.json（目录不是演示数据或已被移动）"));
    return manifestFromJson(file.readAll(), out, error);
}

// ---------------------------------------------------------------- 目录安全

bool validateTargetDir(const QString &dataDir, bool requireEmpty, QString *error)
{
    const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(dataDir)).trimmed();
    if (cleaned.isEmpty() || !QDir::isAbsolutePath(cleaned))
        return fail(error, QStringLiteral("--data-dir 必须是绝对路径: ") + dataDir);
    QFileInfo info(cleaned);
    if (info.isRoot())
        return fail(error, QStringLiteral("拒绝把文件系统根目录当作演示数据目录: ") + cleaned);
    // 逐级拒绝符号链接/junction 组件，防止解析后落到预期目录之外。
    QString component = cleaned;
    while (component.size() > 2) {
        QFileInfo part(component);
        if (part.isSymLink())
            return fail(error, QStringLiteral("演示数据目录路径包含符号链接/联接点: ") + component);
        const QString parent = part.absolutePath();
        if (parent == component) break;
        component = parent;
    }
    QDir dir(cleaned);
    if (dir.exists()) {
        if (requireEmpty) {
            QStringList entries = dir.entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            // 崩溃残留/外部持有的锁文件不算数据：活锁会在 SingleInstanceGuard 处被拒绝，
            // 超过 stale 时限的死锁会被清掉后重新生成。
            entries.removeOne(QStringLiteral(".kylinlist.lock"));
            if (!entries.isEmpty())
                return fail(error, QStringLiteral("演示数据目录必须为空（旧目录请用 --reset 隔离）: ") + cleaned);
        }
        if (!dir.isReadable())
            return fail(error, QStringLiteral("演示数据目录不可读: ") + cleaned);
    } else if (requireEmpty) {
        if (!QDir().mkpath(cleaned))
            return fail(error, QStringLiteral("无法创建演示数据目录: ") + cleaned);
    } else {
        return fail(error, QStringLiteral("演示数据目录不存在: ") + cleaned);
    }
    return true;
}

// ---------------------------------------------------------------- 数据集

namespace {

// 无日程任务：createTask 建立事件，再用 saveTask 补优先级/标签（全部走 TaskStore 写入口）。
QString plainTask(TaskStore &store, const QString &title, const QString &listId,
                  const QString &sectionId, int priority, const QStringList &tags, QString *error)
{
    const QString id = store.createTask(title, listId, sectionId);
    if (id.isEmpty()) {
        fail(error, QStringLiteral("创建任务失败: ") + store.lastError());
        return QString();
    }
    Entity value = store.entity(id);
    if (value.id.isEmpty()) {
        fail(error, QStringLiteral("创建任务后快照丢失: ") + title);
        return QString();
    }
    value.priority = priority;
    value.tags = tags;
    if (!store.saveTask(value)) {
        fail(error, QStringLiteral("保存任务属性失败: ") + store.lastError());
        return QString();
    }
    return id;
}

// 带日程/提醒/重复的草稿；返回新实体 id。ruleJson 由 saveSchedule 统一解析。
QString scheduledEntity(TaskStore &store, const Entity &draft, const ScheduleValue &schedule, QString *error)
{
    Entity resolved = draft;
    applyScheduleToEntity(&resolved, schedule);
    ScheduleValue withResolved = schedule;
    for (TaskReminder &reminder : withResolved.reminders) {
        if (reminder.ruleJson.isEmpty()) continue;
        ReminderSpec spec;
        if (!reminderFromRuleText(reminder.ruleJson, &spec, error)) return QString();
        if (!resolveReminder(spec, resolved, &reminder.triggerAt, error)) return QString();
    }
    QString id;
    if (!store.saveSchedule(draft, withResolved, &id) || id.isEmpty()) {
        if (error && error->isEmpty()) *error = store.lastError();
        return QString();
    }
    return id;
}

TaskReminder relativeReminder(ReminderSpec::Anchor anchor, ReminderSpec::Unit unit, int offset)
{
    ReminderSpec spec;
    spec.anchor = anchor;
    spec.unit = unit;
    spec.offset = offset;
    TaskReminder reminder;
    reminder.ruleJson = reminderRuleText(spec);
    // 先给一个合法占位；saveSchedule 内部按规则重新解析覆盖。
    reminder.triggerAt = QDateTime(QDate(2000, 1, 1), QTime(9, 0), Qt::UTC);
    return reminder;
}

TaskReminder allDayReminder(int dayOffset, int hour, int minute)
{
    ReminderSpec spec;
    spec.allDay = true;
    spec.dayOffset = dayOffset;
    spec.localTime = QTime(hour, minute);
    TaskReminder reminder;
    reminder.ruleJson = reminderRuleText(spec);
    reminder.triggerAt = QDateTime(QDate(2000, 1, 1), QTime(hour, minute), Qt::UTC);
    return reminder;
}

TaskReminder absoluteReminder(const QDateTime &utcWhen)
{
    ReminderSpec spec;
    spec.anchor = ReminderSpec::Anchor::Absolute;
    spec.absoluteAt = utcWhen;
    TaskReminder reminder;
    reminder.ruleJson = reminderRuleText(spec);
    reminder.triggerAt = utcWhen;
    return reminder;
}

RecurrenceSpec dailyRule()
{
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Daily;
    return spec;
}
RecurrenceSpec weeklyRule()
{
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Weekly;
    spec.weekdays = QVector<int>{Qt::Monday, Qt::Friday};
    return spec;
}
RecurrenceSpec monthlyLastDayRule()
{
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    spec.monthDays = QVector<int>{-1};
    return spec;
}
RecurrenceSpec yearlyRule()
{
    RecurrenceSpec spec;
    spec.frequency = RecurrenceSpec::Frequency::Yearly;
    spec.month = 5;
    spec.monthDays = QVector<int>{12};
    return spec;
}
RecurrenceSpec lunarMonthlyRule()
{
    RecurrenceSpec spec;
    spec.kind = RecurrenceSpec::Kind::LunarMonthly;
    spec.frequency = RecurrenceSpec::Frequency::Monthly;
    return spec;
}
RecurrenceSpec lunarYearlyRule()
{
    RecurrenceSpec spec;
    spec.kind = RecurrenceSpec::Kind::LunarYearly;
    spec.frequency = RecurrenceSpec::Frequency::Yearly;
    return spec;
}

QString importStaged(TaskStore &store, const QString &entityId, const QString &stagingDir,
                     const QString &fileName, const QByteArray &content, QString *error)
{
    const QString source = QDir(stagingDir).filePath(fileName);
    QFile file(source);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size()) {
        file.close();
        fail(error, QStringLiteral("写入附件源文件失败: ") + source);
        return QString();
    }
    file.close();
    const Attachment row = store.importAttachment(entityId, source);
    if (row.id.isEmpty()) {
        fail(error, QStringLiteral("导入附件失败: ") + store.lastError());
        return QString();
    }
    return row.id;
}

} // namespace

bool populate(TaskStore &store, const QDateTime &nowUtc, Manifest *out, QString *error)
{
    Manifest m;
    m.format = manifestFormat();
    m.version = kManifestVersion;
    m.marker = ownershipMarker();
    m.owned = true;
    m.canReset = true;
    m.generatorVersion = QStringLiteral(KYLINLIST_VERSION);
    m.mode = QStringLiteral("seed-demo");
    m.generatedAtUtc = nowUtc;
    m.referenceDate = toAppTime(nowUtc).date();
    const QDate ref = m.referenceDate;

    // ---- 文件夹/清单/section ----
    for (int i = 0; i < kFolderCount; ++i) {
        const QString id = store.createFolder(QString::fromUtf8(kFolders[i]),
                                              QString::fromUtf8(kFolderIcons[i]));
        if (id.isEmpty()) return fail(error, QStringLiteral("创建文件夹失败: ") + store.lastError());
        m.folderIds.append(id);
    }
    for (int i = 0; i < kListCount; ++i) {
        const ListExpectation &expectation = kLists[i];
        const QString folderId = expectation.inFolder
            ? m.folderIds.at(expectation.archived ? kFolderCount - 1 : (i < 2 ? 0 : i == 2 ? 1 : 0))
            : QString();
        const QString id = store.createList(QString::fromUtf8(expectation.name), folderId,
            expectation.noteKind ? EntityKind::Note : EntityKind::Task,
            QString::fromUtf8(expectation.icon), QString::fromLatin1(expectation.color));
        if (id.isEmpty()) return fail(error, QStringLiteral("创建清单失败: ") + store.lastError());
        m.listIds.append(id);
    }
    const QString devList = m.listIds.at(0), researchList = m.listIds.at(1), familyList = m.listIds.at(2);
    const QString fitnessList = m.listIds.at(3), readingList = m.listIds.at(4);
    const QString noteList = m.listIds.at(5), archiveList = m.listIds.at(6);
    const QString secOngoing = store.createSection(devList, QStringLiteral("进行中"));
    const QString secPlanned = store.createSection(devList, QStringLiteral("已规划"));
    const QString secBooks = store.createSection(readingList, QStringLiteral("本周书单"));
    if (secOngoing.isEmpty() || secPlanned.isEmpty() || secBooks.isEmpty())
        return fail(error, QStringLiteral("创建 section 失败: ") + store.lastError());
    m.sectionIds << secOngoing << secPlanned << secBooks;
    m.inboxListId = store.inboxListId();
    if (m.inboxListId.isEmpty()) return fail(error, QStringLiteral("缺少系统收集箱"));

    // ---- 收集箱（优先级 0..3、标签、置顶）----
    const QString tidy = plainTask(store, QStringLiteral("整理书桌"), m.inboxListId, QString(), 1,
                                   QStringList() << QStringLiteral("日常"), error);
    if (tidy.isEmpty()) return false;
    const QString gift = plainTask(store, QStringLiteral("买生日礼物"), m.inboxListId, QString(), 3,
                                   QStringList() << QStringLiteral("生活"), error);
    if (gift.isEmpty()) return false;
    const QString dentist = plainTask(store, QStringLiteral("预约牙医复查"), m.inboxListId, QString(), 2,
                                   QStringList(), error);
    if (dentist.isEmpty()) return false;
    const QString license = plainTask(store, QStringLiteral("更换驾照照片"), m.inboxListId, QString(), 0,
                                   QStringList(), error);
    if (license.isEmpty()) return false;
    if (!store.setPinnedAt(gift, nowUtc))
        return fail(error, QStringLiteral("置顶失败: ") + store.lastError());
    m.pinnedTaskId = gift;

    // ---- 日程形式覆盖 ----
    Entity draft;
    ScheduleValue schedule;

    // 今日时间段 + 多条相对提醒（父任务，也是内部链接目标）。
    draft = Entity();
    draft.title = QStringLiteral("完成数据层重构方案");
    draft.listId = devList; draft.sectionId = secOngoing;
    draft.priority = 3;
    draft.tags = QStringList() << QStringLiteral("项目A") << QStringLiteral("重构");
    schedule = ScheduleValue();
    schedule.startAt = wallClock(ref, 9, 30);
    schedule.dueAt = wallClock(ref, 11, 0);
    schedule.hasScheduleRange = true;
    schedule.reminders.append(relativeReminder(ReminderSpec::Anchor::Start, ReminderSpec::Unit::Minutes, 30));
    schedule.reminders.append(relativeReminder(ReminderSpec::Anchor::End, ReminderSpec::Unit::Days, 1));
    m.scheduleTodayRangeId = scheduledEntity(store, draft, schedule, error);
    m.hierarchyParentId = m.scheduleTodayRangeId;
    m.reminderRelativeId = m.scheduleTodayRangeId;
    if (m.hierarchyParentId.isEmpty()) return false;

    // 子任务与孙任务（createSubtask 继承父项日期语义）。
    m.hierarchyChildId = store.createSubtask(QStringLiteral("梳理表结构"), m.hierarchyParentId);
    if (m.hierarchyChildId.isEmpty()) return fail(error, QStringLiteral("创建子任务失败: ") + store.lastError());
    m.hierarchyGrandchildId = store.createSubtask(QStringLiteral("核对索引与外键清单"), m.hierarchyChildId);
    if (m.hierarchyGrandchildId.isEmpty()) return fail(error, QStringLiteral("创建孙任务失败: ") + store.lastError());

    // Markdown 正文 + 内部链接。
    draft = Entity();
    draft.title = QStringLiteral("需求评审纪要");
    draft.listId = devList; draft.sectionId = secPlanned;
    draft.body = QStringLiteral("## 评审结论\n- [x] 数据层方案初审通过\n- [ ] 补充回滚与迁移预案\n\n")
        + QStringLiteral("相关任务：") + entityReference(m.hierarchyParentId, QStringLiteral("完成数据层重构方案"))
        + QStringLiteral("\n参考资料：[Qt SQLite](https://doc.qt.io/qt-5/qtsql.html)");
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(1);
    m.markdownLinkId = scheduledEntity(store, draft, schedule, error);
    m.markdownTargetId = m.hierarchyParentId;
    if (m.markdownLinkId.isEmpty()) return false;

    // 全天多日区间 + 全天提醒。
    draft = Entity();
    draft.title = QStringLiteral("发布 1.4 版本");
    draft.listId = devList;
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(kMultidayAllDayStart);
    schedule.allDayEndDate = ref.addDays(kMultidayAllDayEnd);
    schedule.hasScheduleRange = true;
    schedule.reminders.append(allDayReminder(1, 9, 0));
    m.scheduleMultidayAllDayId = scheduledEntity(store, draft, schedule, error);
    m.reminderAllDayId = m.scheduleMultidayAllDayId;
    if (m.scheduleMultidayAllDayId.isEmpty()) return false;

    // 跨多日时刻区间 + 绝对提醒（重附件实体）。
    draft = Entity();
    draft.title = QStringLiteral("市场竞品分析");
    draft.listId = researchList;
    draft.priority = 2;
    schedule = ScheduleValue();
    schedule.startAt = wallClock(ref.addDays(kMultidayTimedStart), 9, 0);
    schedule.dueAt = wallClock(ref.addDays(kMultidayTimedEnd), 18, 0);
    schedule.hasScheduleRange = true;
    schedule.reminders.append(absoluteReminder(wallClock(ref.addDays(kMultidayTimedStart), 8, 45)));
    m.scheduleMultidayTimedId = scheduledEntity(store, draft, schedule, error);
    m.reminderAbsoluteId = m.scheduleMultidayTimedId;
    m.attachmentHeavyEntityId = m.scheduleMultidayTimedId;
    if (m.scheduleMultidayTimedId.isEmpty()) return false;

    // 未来单点时刻（最近7天之外）。
    draft = Entity();
    draft.title = QStringLiteral("用户访谈排期");
    draft.listId = researchList;
    schedule = ScheduleValue();
    schedule.dueAt = wallClock(ref.addDays(kFutureTimedOffset), 15, 0);
    m.scheduleFutureTimedId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleFutureTimedId.isEmpty()) return false;

    // 过期：全天与时刻两种。
    draft = Entity();
    draft.title = QStringLiteral("补交上周周报");
    draft.listId = researchList;
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(kOverdueAllDayOffset);
    m.scheduleOverdueAllDayId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleOverdueAllDayId.isEmpty()) return false;
    draft = Entity();
    draft.title = QStringLiteral("缴纳物业费");
    draft.listId = familyList;
    draft.priority = 2;
    schedule = ScheduleValue();
    schedule.dueAt = wallClock(ref.addDays(kOverdueTimedOffset), 18, 0);
    m.scheduleOverdueTimedId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleOverdueTimedId.isEmpty()) return false;

    // 明日全天。
    draft = Entity();
    draft.title = QStringLiteral("交房租");
    draft.listId = familyList;
    draft.tags = QStringList() << QStringLiteral("生活");
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(1);
    m.scheduleTomorrowAllDayId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleTomorrowAllDayId.isEmpty()) return false;

    // 家庭多日全天区间 + 多标签。
    const QString cleaning = plainTask(store, QStringLiteral("家庭大扫除"), familyList, QString(), 1,
                                       QStringList() << QStringLiteral("生活") << QStringLiteral("周末"), error);
    if (cleaning.isEmpty()) return false;
    {
        Entity value = store.entity(cleaning);
        ScheduleValue range;
        range.allDayDate = ref.addDays(2);
        range.allDayEndDate = ref.addDays(3);
        range.hasScheduleRange = true;
        if (!store.saveSchedule(value, range))
            return fail(error, QStringLiteral("保存家庭大扫除日程失败: ") + store.lastError());
    }

    // 跨午夜时刻区间（夜间航班）。
    draft = Entity();
    draft.title = QStringLiteral("夜间航班值机提醒");
    draft.listId = familyList;
    schedule = ScheduleValue();
    schedule.startAt = wallClock(ref, 23, 30);
    schedule.dueAt = wallClock(ref.addDays(1), 0, 30);
    schedule.hasScheduleRange = true;
    m.scheduleOvernightId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleOvernightId.isEmpty()) return false;

    // 午夜零点开始的时刻区间。
    draft = Entity();
    draft.title = QStringLiteral("零点抢购活动");
    draft.listId = familyList;
    schedule = ScheduleValue();
    schedule.startAt = wallClock(ref.addDays(1), 0, 0);
    schedule.dueAt = wallClock(ref.addDays(1), 0, 30);
    schedule.hasScheduleRange = true;
    m.scheduleMidnightId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleMidnightId.isEmpty()) return false;

    // 同日全天区间（起止同日但显式时间段）。
    draft = Entity();
    draft.title = QStringLiteral("全天驻场支持");
    draft.listId = researchList;
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(1);
    schedule.allDayEndDate = ref.addDays(1);
    schedule.hasScheduleRange = true;
    m.scheduleSameDayAllDayId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleSameDayAllDayId.isEmpty()) return false;

    // 今日晨间单点时刻 + Anchor::Schedule 相对提醒（单点计划锚定到截止时刻）。
    const QString morningRun = plainTask(store, QStringLiteral("晨跑 5 公里"), fitnessList, QString(), 1,
                                         QStringList() << QStringLiteral("日常"), error);
    if (morningRun.isEmpty()) return false;
    {
        Entity value = store.entity(morningRun);
        ScheduleValue run;
        run.dueAt = wallClock(ref, 7, 0);
        run.reminders.append(relativeReminder(ReminderSpec::Anchor::Schedule, ReminderSpec::Unit::Minutes, 10));
        if (!store.saveSchedule(value, run))
            return fail(error, QStringLiteral("保存晨跑日程失败: ") + store.lastError());
        m.reminderScheduleAnchorId = morningRun;
    }

    // 明日时刻区间（私教课）。
    const QString coach = plainTask(store, QStringLiteral("健身房私教课"), fitnessList, QString(), 2,
                                    QStringList(), error);
    if (coach.isEmpty()) return false;
    {
        Entity value = store.entity(coach);
        ScheduleValue lesson;
        lesson.startAt = wallClock(ref.addDays(1), 19, 0);
        lesson.dueAt = wallClock(ref.addDays(1), 20, 0);
        lesson.hasScheduleRange = true;
        if (!store.saveSchedule(value, lesson))
            return fail(error, QStringLiteral("保存私教课日程失败: ") + store.lastError());
    }

    // 未来全天（体检）。
    draft = Entity();
    draft.title = QStringLiteral("月度体检");
    draft.listId = fitnessList;
    schedule = ScheduleValue();
    schedule.allDayDate = ref.addDays(kFutureAllDayOffset);
    m.scheduleFutureAllDayId = scheduledEntity(store, draft, schedule, error);
    if (m.scheduleFutureAllDayId.isEmpty()) return false;

    // ---- 重复规则：日/周/月/年/农历月/农历年 ----
    struct RecurringCase { QString *id; RecurrenceSpec (*build)(); const char *title; const QString &list; bool timed; };
    const RecurringCase cases[] = {
        {&m.recurringDailyId, dailyRule, "每日站立会", devList, true},
        {&m.recurringWeeklyId, weeklyRule, "周报整理", devList, false},
        {&m.recurringMonthlyId, monthlyLastDayRule, "月度复盘会议", devList, false},
        {&m.recurringYearlyId, yearlyRule, "妈妈生日", familyList, false},
        {&m.recurringLunarMonthlyId, lunarMonthlyRule, "农历初一静心", familyList, false},
        {&m.recurringLunarYearlyId, lunarYearlyRule, "祖母生日（农历）", familyList, false},
    };
    for (const RecurringCase &entry : cases) {
        Entity recurring;
        recurring.title = QString::fromUtf8(entry.title);
        recurring.listId = entry.list;
        ScheduleValue repeating;
        if (entry.timed) repeating.dueAt = wallClock(ref, 9, 30);
        else repeating.allDayDate = ref;
        repeating.repeating = true;
        repeating.recurrence = entry.build();
        *entry.id = scheduledEntity(store, recurring, repeating, error);
        if (entry.id->isEmpty()) return false;
    }

    // 每日打卡：完成后生成后继实例（明日）。
    draft = Entity();
    draft.title = QStringLiteral("每日饮水打卡");
    draft.listId = fitnessList;
    schedule = ScheduleValue();
    schedule.allDayDate = ref;
    schedule.repeating = true;
    schedule.recurrence = dailyRule();
    m.recurringSourceId = scheduledEntity(store, draft, schedule, error);
    if (m.recurringSourceId.isEmpty()) return false;
    if (!store.setCompleted(m.recurringSourceId, true))
        return fail(error, QStringLiteral("完成打卡任务失败: ") + store.lastError());
    const QString seriesId = store.entity(m.recurringSourceId).recurrenceSeriesId;
    if (seriesId.isEmpty()) return fail(error, QStringLiteral("打卡任务缺少重复 series"));
    for (const Entity &candidate : store.entities())
        if (candidate.id != m.recurringSourceId && candidate.recurrenceSeriesId == seriesId) {
            if (m.recurringNextId.isEmpty()) m.recurringNextId = candidate.id;
            else return fail(error, QStringLiteral("打卡任务生成了多个后继实例"));
        }
    if (m.recurringNextId.isEmpty())
        return fail(error, QStringLiteral("完成打卡任务未生成后继实例"));

    // ---- 完成态（普通任务；completedAt 由库按当前时刻记录，不回填历史）----
    const char *const completedTitles[] = {"搭建演示环境", "购买技术参考书", "预约用户访谈房间"};
    const QString completedLists[] = {devList, readingList, researchList};
    for (int i = 0; i < 3; ++i) {
        const QString id = plainTask(store, QString::fromUtf8(completedTitles[i]), completedLists[i],
                                     QString(), 0, QStringList(), error);
        if (id.isEmpty()) return false;
        if (!store.setCompleted(id, true))
            return fail(error, QStringLiteral("标记完成失败: ") + store.lastError());
        m.completedTaskIds.append(id);
    }
    m.completedTaskIds.append(m.recurringSourceId);

    // ---- 放弃、回收站、归档清单 ----
    const QString abandonedA = plainTask(store, QStringLiteral("调研 Flutter 桌面端方案"), devList,
                                         QString(), 1, QStringList() << QStringLiteral("项目A"), error);
    const QString abandonedB = plainTask(store, QStringLiteral("组织线下发布会"), researchList,
                                         QString(), 2, QStringList(), error);
    if (abandonedA.isEmpty() || abandonedB.isEmpty()) return false;
    if (!store.abandonTask(abandonedA) || !store.abandonTask(abandonedB))
        return fail(error, QStringLiteral("放弃任务失败: ") + store.lastError());
    m.abandonedTaskIds << abandonedA << abandonedB;

    const QString trashA = plainTask(store, QStringLiteral("临时备忘：打印文件"), familyList, QString(), 0,
                                     QStringList(), error);
    const QString trashB = plainTask(store, QStringLiteral("测试任务（已删除）"), m.inboxListId, QString(), 0,
                                     QStringList(), error);
    if (trashA.isEmpty() || trashB.isEmpty()) return false;
    if (!store.trashTask(trashA) || !store.trashTask(trashB))
        return fail(error, QStringLiteral("移入回收站失败: ") + store.lastError());
    m.trashedTaskIds << trashA << trashB;

    const QString archivedA = plainTask(store, QStringLiteral("旧官网改版 v1"), archiveList, QString(), 0,
                                        QStringList(), error);
    const QString archivedB = plainTask(store, QStringLiteral("旧数据迁移脚本整理"), archiveList, QString(), 1,
                                        QStringList(), error);
    if (archivedA.isEmpty() || archivedB.isEmpty()) return false;
    if (!store.setListArchived(archiveList, true))
        return fail(error, QStringLiteral("归档清单失败: ") + store.lastError());
    m.archivedListIds.append(archiveList);

    // ---- 笔记（便签本）----
    const QString noteIdea = store.createNote(QStringLiteral("灵感速记"), noteList, QString());
    if (noteIdea.isEmpty()) return fail(error, QStringLiteral("创建笔记失败: ") + store.lastError());
    {
        Entity value = store.entity(noteIdea);
        value.tags = QStringList() << QStringLiteral("灵感");
        if (!store.saveTask(value)) return fail(error, QStringLiteral("保存笔记失败: ") + store.lastError());
    }
    const QString noteShopping = store.createNote(QStringLiteral("购物备忘"), noteList, QString());
    if (noteShopping.isEmpty()) return fail(error, QStringLiteral("创建笔记失败: ") + store.lastError());
    {
        Entity value = store.entity(noteShopping);
        value.tags = QStringList() << QStringLiteral("生活");
        if (!store.saveTask(value)) return fail(error, QStringLiteral("保存笔记失败: ") + store.lastError());
    }
    // 带日程与绝对提醒的笔记。
    draft = Entity();
    draft.kind = EntityKind::Note;
    draft.title = QStringLiteral("会议纪要模板");
    draft.listId = noteList;
    schedule = ScheduleValue();
    schedule.dueAt = wallClock(ref.addDays(1), 12, 0);
    schedule.reminders.append(absoluteReminder(wallClock(ref.addDays(1), 11, 30)));
    m.reminderNoteId = scheduledEntity(store, draft, schedule, error);
    if (m.reminderNoteId.isEmpty()) return false;
    // Markdown 正文 + 内部链接（目标 = 夜间航班）。
    draft = Entity();
    draft.kind = EntityKind::Note;
    draft.title = QStringLiteral("出行准备清单");
    draft.listId = noteList;
    draft.body = QStringLiteral("# 出行准备\n- [ ] 护照与身份证件\n- [ ] 充电转换头\n- [ ] 旅行保险\n\n")
        + QStringLiteral("关联任务：") + entityReference(m.scheduleOvernightId, QStringLiteral("夜间航班值机提醒"));
    schedule = ScheduleValue();
    m.noteLinkId = scheduledEntity(store, draft, schedule, error);
    m.noteLinkTargetId = m.scheduleOvernightId;
    if (m.noteLinkId.isEmpty()) return false;

    // ---- 附件（真实合成文件；一条实体挂多个）----
    {
        QTemporaryDir staging;
        if (!staging.isValid()) return fail(error, QStringLiteral("无法创建附件暂存目录"));
        const QString heavy = m.attachmentHeavyEntityId;
        if (importStaged(store, heavy, staging.path(), QStringLiteral("rival-shot.png"),
                         decodeBase64(kPngStripesBase64), error).isEmpty()) return false;
        if (importStaged(store, heavy, staging.path(), QStringLiteral("rival-table.csv"),
                         QByteArray(kCsvContent), error).isEmpty()) return false;
        if (importStaged(store, heavy, staging.path(), QStringLiteral("interview-notes.txt"),
                         QByteArray(kNotesText), error).isEmpty()) return false;
        if (importStaged(store, m.hierarchyParentId, staging.path(), QStringLiteral("schema-sketch.png"),
                         decodeBase64(kPngSolidBase64), error).isEmpty()) return false;
        if (importStaged(store, m.noteLinkId, staging.path(), QStringLiteral("packing.txt"),
                         QByteArray(kPackingText), error).isEmpty()) return false;
        m.attachmentEntityIds << heavy << m.hierarchyParentId << m.noteLinkId;
    }

    // ---- 桌面状态：六色、三层，仅两个打开 ----
    struct DesktopCase { QString entityId; NoteColor color; QRect geometry; int z; NoteLayer layer; bool open; };
    const DesktopCase desktopCases[] = {
        {m.hierarchyParentId, NoteColor::Blue, QRect(120, 90, 340, 360), 1, NoteLayer::Normal, true},
        {m.scheduleTomorrowAllDayId, NoteColor::Green, QRect(520, 180, 320, 320), 6, NoteLayer::OnTop, true},
        {m.noteLinkId, NoteColor::Yellow, QRect(160, 520, 300, 300), 2, NoteLayer::OnDesktop, false},
        {m.reminderNoteId, NoteColor::Pink, QRect(600, 520, 300, 300), 3, NoteLayer::Normal, false},
        {noteIdea, NoteColor::Purple, QRect(200, 700, 300, 300), 4, NoteLayer::OnDesktop, false},
        {noteShopping, NoteColor::White, QRect(640, 700, 300, 300), 5, NoteLayer::OnTop, false},
    };
    for (const DesktopCase &entry : desktopCases) {
        if (!store.saveDesktopState(entry.entityId, entry.color, entry.geometry, entry.z, entry.layer, entry.open))
            return fail(error, QStringLiteral("保存桌面状态失败: ") + store.lastError());
        m.desktopEntityIds.append(entry.entityId);
    }

    // ---- 汇总数量与 id 清单 ----
    int tasks = 0, notes = 0, reminders = 0;
    QSet<QString> seriesSet;
    for (const Entity &value : store.entities()) {
        if (value.kind == EntityKind::Task) { ++tasks; m.taskIds.append(value.id); }
        else { ++notes; m.noteIds.append(value.id); }
        reminders += value.reminders.size();
        if (!value.recurrenceSeriesId.isEmpty()) seriesSet.insert(value.recurrenceSeriesId);
    }
    m.tasks = tasks; m.notes = notes; m.entities = tasks + notes;
    m.reminders = reminders;
    m.recurrenceSeries = seriesSet.size();
    m.folders = store.folders().size();
    m.lists = store.lists().size();
    m.sections = store.sections().size();
    m.attachments = 0;
    for (const QString &entityId : m.attachmentEntityIds)
        m.attachments += store.attachments(entityId).size();
    m.desktopNotes = m.desktopEntityIds.size();
    m.events = 0;
    for (const Entity &value : store.entities()) m.events += store.events(value.id).size();
    if (out) *out = m;
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------- generate

bool generate(const QString &dataDir, QString *error, bool *locked)
{
    if (locked) *locked = false;
    QString failure;
    if (!validateTargetDir(dataDir, true, &failure)) return fail(error, failure);
    const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(dataDir)).trimmed();
    ScopedDataDirEnv env(cleaned);

    // 独立目录先建锁再动库；锁被占用时直接失败，绝不激活其他实例。
    SingleInstanceGuard guard(lockFilePath());
    const SingleInstanceGuard::Result acquired = guard.acquire();
    if (acquired == SingleInstanceGuard::AlreadyRunning) {
        if (locked) *locked = true;
        return fail(error, QStringLiteral("演示数据目录正被其他实例使用: ") + cleaned);
    }
    if (acquired != SingleInstanceGuard::Acquired)
        return fail(error, QStringLiteral("无法锁定演示数据目录: ") + cleaned);
    DatabaseCloser closer; // 之后所有返回路径都会关库、由析构释放锁并恢复环境变量。

    if (!Database::instance().openAt(databasePath(), &failure))
        return fail(error, QStringLiteral("打开演示数据库失败: ") + failure);
    Manifest manifest;
    {
        // 内层作用域：TaskStore（及其 QSqlDatabase 句柄）必须在 DatabaseCloser 关闭连接前析构。
        TaskStore store(Database::instance().connection());
        if (!store.load(&failure))
            return fail(error, QStringLiteral("初始化演示数据库失败: ") + failure);
        if (!populate(store, appNow(), &manifest, &failure))
            return fail(error, failure.isEmpty() ? store.lastError() : failure);
        manifest.dataDir = cleaned;
        if (!store.exportBackup(backupPath(cleaned)))
            return fail(error, QStringLiteral("导出 demo-backup.klb 失败: ") + store.lastError());
    }
    QFile backupFile(backupPath(cleaned));
    if (!backupFile.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("读取 demo-backup.klb 失败"));
    const QByteArray backupBytes = backupFile.readAll();
    backupFile.close();
    manifest.backupFile = QStringLiteral("demo-backup.klb");
    manifest.backupBytes = backupBytes.size();
    manifest.backupSha256 = sha256Hex(backupBytes);
    if (!writeManifest(cleaned, manifest, &failure))
        return fail(error, failure);
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------- verify

namespace {

struct Report {
    QStringList lines;
    int passed = 0;

    void pass(const QString &detail)
    {
        ++passed;
        lines.append(QStringLiteral("[%1] PASS %2").arg(passed).arg(detail));
    }
};

bool writeReport(const QString &dataDir, const Report &log, const QString &resultLine, QString *report)
{
    QStringList lines = log.lines;
    lines.append(resultLine);
    const QString text = lines.join(QLatin1Char('\n'));
    if (report) *report = text;
    QFile file(reportPath(dataDir));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(text.toUtf8());
        file.write("\n");
    }
    return true;
}

bool sameWall(const QDateTime &actual, const QDate &date, int hour, int minute)
{
    if (!actual.isValid()) return false;
    const QDateTime local = toAppTime(actual);
    return local.date() == date && local.time() == QTime(hour, minute);
}

} // namespace

bool verify(const QString &dataDir, QString *report, QString *error, bool *locked)
{
    if (locked) *locked = false;
    Report log;
    const QString cleaned = QDir::cleanPath(QDir::fromNativeSeparators(dataDir)).trimmed();
    QString failure;
    // 阶段一（usage/manifest）：目录尚未证明是本工具生成的演示数据，绝不写入任何文件。
    const auto bailEarly = [&](const QString &area, const QString &detail) {
        failure = QStringLiteral("verify: %1: %2").arg(area, detail);
        if (error) *error = failure;
        log.lines.append(QStringLiteral("FAIL %1: %2").arg(area, detail));
        log.lines.append(QStringLiteral("RESULT: FAIL (%1)").arg(area));
        if (report) *report = log.lines.join(QLatin1Char('\n'));
        return false;
    };
    if (cleaned.isEmpty() || !QDir::isAbsolutePath(cleaned))
        return bailEarly(QStringLiteral("usage"), QStringLiteral("--data-dir 必须是绝对路径"));
    if (!validateTargetDir(cleaned, false, &failure))
        return bailEarly(QStringLiteral("usage"), failure);

    Manifest manifest;
    QString manifestError;
    if (!readManifest(cleaned, &manifest, &manifestError))
        return bailEarly(QStringLiteral("manifest"),
                         manifestError.isEmpty() ? QStringLiteral("manifest 不可读") : manifestError);
    log.pass(QStringLiteral("manifest: format=%1 version=%2 owned=%3 canReset=%4")
                 .arg(manifest.format).arg(manifest.version).arg(manifest.owned).arg(manifest.canReset));
    const QDate ref = manifest.referenceDate;

    // 阶段二：manifest 已证明所有权，失败报告允许写进目录（demo-verify-report.txt）。
    const auto bail = [&](const QString &area, const QString &detail) {
        failure = QStringLiteral("verify: %1: %2").arg(area, detail);
        if (error) *error = failure;
        writeReport(cleaned, log, QStringLiteral("RESULT: FAIL (%1)").arg(area), report);
        return false;
    };
    ScopedDataDirEnv env(cleaned);
    SingleInstanceGuard guard(lockFilePath());
    const SingleInstanceGuard::Result acquired = guard.acquire();
    if (acquired != SingleInstanceGuard::Acquired) {
        if (locked) *locked = true;
        return bail(QStringLiteral("lock"), acquired == SingleInstanceGuard::AlreadyRunning
            ? QStringLiteral("数据目录正被其他实例使用") : QStringLiteral("无法锁定数据目录"));
    }
    DatabaseCloser closer; // 之后所有返回路径都会关库、释放锁并恢复环境变量。

    if (!Database::instance().openAt(databasePath(), &failure))
        return bail(QStringLiteral("database"), failure);
    {
    // 内层作用域：TaskStore（及其 QSqlDatabase 句柄）必须在 DatabaseCloser 关闭连接前析构。
    TaskStore store(Database::instance().connection());
    if (!store.load(&failure))
        return bail(QStringLiteral("database"), failure);

    // 数量。
    const QVector<Entity> entities = store.entities();
    int tasks = 0, notes = 0, reminders = 0;
    QSet<QString> seriesSet;
    for (const Entity &value : entities) {
        if (value.kind == EntityKind::Task) ++tasks; else ++notes;
        reminders += value.reminders.size();
        if (!value.recurrenceSeriesId.isEmpty()) seriesSet.insert(value.recurrenceSeriesId);
    }
    const struct { const char *area; int actual; int expected; } counts[] = {
        {"folders", store.folders().size(), manifest.folders},
        {"lists", store.lists().size(), manifest.lists},
        {"sections", store.sections().size(), manifest.sections},
        {"tasks", tasks, manifest.tasks},
        {"notes", notes, manifest.notes},
        {"entities", entities.size(), manifest.entities},
        {"reminders", reminders, manifest.reminders},
        {"recurrenceSeries", seriesSet.size(), manifest.recurrenceSeries},
        {"desktopNotes", static_cast<int>(manifest.desktopEntityIds.size()), manifest.desktopNotes},
    };
    for (const auto &entry : counts)
        if (entry.actual != entry.expected)
            return bail(QStringLiteral("counts"),
                QStringLiteral("%1 数量 %2 与 manifest %3 不一致")
                    .arg(QString::fromLatin1(entry.area)).arg(entry.actual).arg(entry.expected));
    log.pass(QStringLiteral("counts: folders=%1 lists=%2 sections=%3 tasks=%4 notes=%5 entities=%6 "
                            "reminders=%7 series=%8 desktop=%9")
                 .arg(manifest.folders).arg(manifest.lists).arg(manifest.sections).arg(manifest.tasks)
                 .arg(manifest.notes).arg(manifest.entities).arg(manifest.reminders)
                 .arg(manifest.recurrenceSeries).arg(manifest.desktopNotes));

    // 清单外观与归档。
    if (manifest.inboxListId != store.inboxListId())
        return bail(QStringLiteral("lists"), QStringLiteral("inbox id 与 manifest 不一致"));
    if (manifest.listIds.size() != kListCount)
        return bail(QStringLiteral("lists"), QStringLiteral("清单数量与期望不一致"));
    if (manifest.folderIds.size() != kFolderCount)
        return bail(QStringLiteral("lists"), QStringLiteral("文件夹数量与期望不一致"));
    for (int i = 0; i < kFolderCount; ++i) {
        Folder folder;
        for (const Folder &row : store.folders()) if (row.id == manifest.folderIds.at(i)) folder = row;
        if (folder.id.isEmpty() || folder.name != QString::fromUtf8(kFolders[i])
            || folder.icon != QString::fromUtf8(kFolderIcons[i]))
            return bail(QStringLiteral("lists"), QStringLiteral("文件夹外观与期望不一致: %1")
                              .arg(QString::fromUtf8(kFolders[i])));
    }
    for (int i = 0; i < kListCount; ++i) {
        TaskList list;
        for (const TaskList &row : store.lists()) if (row.id == manifest.listIds.at(i)) list = row;
        const ListExpectation &expectation = kLists[i];
        if (list.id.isEmpty() || list.name != QString::fromUtf8(expectation.name)
            || list.color != QString::fromLatin1(expectation.color)
            || list.icon != QString::fromUtf8(expectation.icon)
            || (list.archivedAtUtcMs > 0) != expectation.archived
            || list.kind != (expectation.noteKind ? EntityKind::Note : EntityKind::Task))
            return bail(QStringLiteral("lists"),
                QStringLiteral("清单 “%1” 外观/类型与期望不一致").arg(QString::fromUtf8(expectation.name)));
    }
    for (const QString &id : manifest.archivedListIds)
        if (!store.isListArchived(id))
            return bail(QStringLiteral("lists"), QStringLiteral("归档清单未处于归档状态"));
    log.pass(QStringLiteral("lists: inbox + %1 文件夹 + %2 清单（颜色/图标/归档/笔记清单）")
                 .arg(kFolderCount).arg(kListCount));

    // 实体存在性与 kind。
    auto entityById = [&entities](const QString &id) {
        for (const Entity &value : entities) if (value.id == id) return value;
        return Entity();
    };
    for (const QString &id : manifest.taskIds) {
        const Entity value = entityById(id);
        if (value.id != id || value.kind != EntityKind::Task)
            return bail(QStringLiteral("ids"), QStringLiteral("任务 id 缺失或类型不符: %1").arg(id));
    }
    for (const QString &id : manifest.noteIds) {
        const Entity value = entityById(id);
        if (value.id != id || value.kind != EntityKind::Note)
            return bail(QStringLiteral("ids"), QStringLiteral("笔记 id 缺失或类型不符: %1").arg(id));
    }
    log.pass(QStringLiteral("ids: %1 个任务与 %2 个笔记全部存在")
                 .arg(manifest.taskIds.size()).arg(manifest.noteIds.size()));

    // 日程形式（已过期/今日/跨午夜/明日/多日/未来）。
    const Entity overdueAllDay = entityById(manifest.scheduleOverdueAllDayId);
    const Entity overdueTimed = entityById(manifest.scheduleOverdueTimedId);
    const Entity todayRange = entityById(manifest.scheduleTodayRangeId);
    const Entity overnight = entityById(manifest.scheduleOvernightId);
    const Entity tomorrowAllDay = entityById(manifest.scheduleTomorrowAllDayId);
    const Entity multidayAllDay = entityById(manifest.scheduleMultidayAllDayId);
    const Entity multidayTimed = entityById(manifest.scheduleMultidayTimedId);
    const Entity futureTimed = entityById(manifest.scheduleFutureTimedId);
    const Entity futureAllDay = entityById(manifest.scheduleFutureAllDayId);
    if (overdueAllDay.allDayDate != ref.addDays(kOverdueAllDayOffset))
        return bail(QStringLiteral("schedules"), QStringLiteral("过期全天日期不正确"));
    if (!sameWall(overdueTimed.dueAt, ref.addDays(kOverdueTimedOffset), 18, 0) || overdueTimed.startAt.isValid())
        return bail(QStringLiteral("schedules"), QStringLiteral("过期单点时刻不正确"));
    if (!sameWall(todayRange.startAt, ref, 9, 30) || !sameWall(todayRange.dueAt, ref, 11, 0)
        || !todayRange.hasScheduleRange)
        return bail(QStringLiteral("schedules"), QStringLiteral("今日时间段不正确"));
    if (!sameWall(overnight.startAt, ref, 23, 30) || !sameWall(overnight.dueAt, ref.addDays(1), 0, 30)
        || !overnight.hasScheduleRange
        || toAppTime(overnight.startAt).date() == toAppTime(overnight.dueAt).date())
        return bail(QStringLiteral("schedules"), QStringLiteral("跨午夜时刻区间不正确"));
    const Entity midnight = entityById(manifest.scheduleMidnightId);
    if (!sameWall(midnight.startAt, ref.addDays(1), 0, 0) || !sameWall(midnight.dueAt, ref.addDays(1), 0, 30)
        || !midnight.hasScheduleRange)
        return bail(QStringLiteral("schedules"), QStringLiteral("午夜零点时刻区间不正确"));
    if (tomorrowAllDay.allDayDate != ref.addDays(1) || tomorrowAllDay.allDayEndDate.isValid())
        return bail(QStringLiteral("schedules"), QStringLiteral("明日全天生效值不正确"));
    const Entity sameDayAllDay = entityById(manifest.scheduleSameDayAllDayId);
    if (sameDayAllDay.allDayDate != ref.addDays(1) || sameDayAllDay.allDayEndDate != ref.addDays(1)
        || !sameDayAllDay.hasScheduleRange)
        return bail(QStringLiteral("schedules"), QStringLiteral("同日全天区间不正确"));
    if (multidayAllDay.allDayDate != ref.addDays(kMultidayAllDayStart)
        || multidayAllDay.allDayEndDate != ref.addDays(kMultidayAllDayEnd) || !multidayAllDay.hasScheduleRange)
        return bail(QStringLiteral("schedules"), QStringLiteral("全天多日区间不正确"));
    if (!sameWall(multidayTimed.startAt, ref.addDays(kMultidayTimedStart), 9, 0)
        || !sameWall(multidayTimed.dueAt, ref.addDays(kMultidayTimedEnd), 18, 0)
        || !multidayTimed.hasScheduleRange)
        return bail(QStringLiteral("schedules"), QStringLiteral("跨多日时刻区间不正确"));
    if (!sameWall(futureTimed.dueAt, ref.addDays(kFutureTimedOffset), 15, 0) || futureTimed.startAt.isValid())
        return bail(QStringLiteral("schedules"), QStringLiteral("未来单点时刻不正确"));
    if (futureAllDay.allDayDate != ref.addDays(kFutureAllDayOffset))
        return bail(QStringLiteral("schedules"), QStringLiteral("未来全天日期不正确"));
    log.pass(QStringLiteral("schedules: 已过期/今日/跨午夜/午夜零点/明日/同日全天/多日/未来 各日期形式一致"));

    // 提醒：多条相对、绝对、全天、笔记。
    const Entity relativeEntity = entityById(manifest.reminderRelativeId);
    if (relativeEntity.reminders.size() != 2)
        return bail(QStringLiteral("reminders"), QStringLiteral("多条相对提醒数量不正确"));
    for (const TaskReminder &reminder : relativeEntity.reminders) {
        ReminderSpec spec;
        if (reminder.ruleJson.isEmpty() || !reminderFromRuleText(reminder.ruleJson, &spec, &failure))
            return bail(QStringLiteral("reminders"), QStringLiteral("相对提醒规则无法解析"));
        QDateTime resolved;
        if (!resolveReminder(spec, relativeEntity, &resolved, &failure) || resolved != reminder.triggerAt)
            return bail(QStringLiteral("reminders"), QStringLiteral("相对提醒触发时间与规则不一致"));
    }
    const Entity absoluteEntity = entityById(manifest.reminderAbsoluteId);
    bool absoluteFound = false;
    for (const TaskReminder &reminder : absoluteEntity.reminders) {
        ReminderSpec spec;
        if (reminderFromRuleText(reminder.ruleJson, &spec, &failure)
            && spec.anchor == ReminderSpec::Anchor::Absolute
            && reminder.triggerAt == wallClock(ref.addDays(kMultidayTimedStart), 8, 45))
            absoluteFound = true;
    }
    if (!absoluteFound)
        return bail(QStringLiteral("reminders"), QStringLiteral("绝对提醒缺失或时刻不正确"));
    const Entity allDayEntity = entityById(manifest.reminderAllDayId);
    bool allDayFound = false;
    for (const TaskReminder &reminder : allDayEntity.reminders) {
        ReminderSpec spec;
        if (reminderFromRuleText(reminder.ruleJson, &spec, &failure) && spec.allDay && spec.dayOffset == 1
            && spec.localTime == QTime(9, 0))
            allDayFound = true;
    }
    if (!allDayFound)
        return bail(QStringLiteral("reminders"), QStringLiteral("全天提醒缺失或参数不正确"));
    const Entity scheduleAnchored = entityById(manifest.reminderScheduleAnchorId);
    bool scheduleAnchorFound = false;
    for (const TaskReminder &reminder : scheduleAnchored.reminders) {
        ReminderSpec spec;
        QDateTime resolved;
        if (reminderFromRuleText(reminder.ruleJson, &spec, &failure)
            && spec.anchor == ReminderSpec::Anchor::Schedule && !spec.allDay
            && resolveReminder(spec, scheduleAnchored, &resolved, &failure)
            && resolved == reminder.triggerAt
            && reminder.triggerAt == wallClock(ref, 6, 50))
            scheduleAnchorFound = true;
    }
    if (!scheduleAnchorFound)
        return bail(QStringLiteral("reminders"), QStringLiteral("Schedule 锚点相对提醒缺失或解析时刻不正确"));
    const Entity noteReminder = entityById(manifest.reminderNoteId);
    if (noteReminder.id != manifest.reminderNoteId || noteReminder.kind != EntityKind::Note
        || noteReminder.reminders.isEmpty())
        return bail(QStringLiteral("reminders"), QStringLiteral("带提醒的笔记缺失"));
    log.pass(QStringLiteral("reminders: 相对(Start/End/Schedule)/绝对/全天/笔记提醒全部一致"));

    // 重复规则与完成后继。
    const struct { QString id; RecurrenceSpec (*build)(); const char *name; } rules[] = {
        {manifest.recurringDailyId, dailyRule, "daily"},
        {manifest.recurringWeeklyId, weeklyRule, "weekly"},
        {manifest.recurringMonthlyId, monthlyLastDayRule, "monthly"},
        {manifest.recurringYearlyId, yearlyRule, "yearly"},
        {manifest.recurringLunarMonthlyId, lunarMonthlyRule, "lunar-monthly"},
        {manifest.recurringLunarYearlyId, lunarYearlyRule, "lunar-yearly"},
    };
    for (const auto &entry : rules) {
        RecurrenceSpec actual;
        if (!store.recurrenceSpec(entry.id, &actual)
            || recurrenceRuleText(actual) != recurrenceRuleText(entry.build()))
            return bail(QStringLiteral("recurrence"),
                QStringLiteral("重复规则 %1 与生成时不一致").arg(QString::fromLatin1(entry.name)));
    }
    const Entity checkinSource = entityById(manifest.recurringSourceId);
    const Entity checkinNext = entityById(manifest.recurringNextId);
    if (checkinSource.status != TaskStatus::Completed || checkinNext.status != TaskStatus::Active
        || checkinSource.recurrenceSeriesId.isEmpty()
        || checkinSource.recurrenceSeriesId != checkinNext.recurrenceSeriesId
        || checkinNext.allDayDate != ref.addDays(1))
        return bail(QStringLiteral("recurrence"), QStringLiteral("完成打卡的后继实例不正确"));
    log.pass(QStringLiteral("recurrence: 日/周/月/年/农历月/农历年 + 完成后继实例一致"));

    // 层级与 Markdown 内部链接。
    const Entity parent = entityById(manifest.hierarchyParentId);
    const Entity child = entityById(manifest.hierarchyChildId);
    const Entity grandchild = entityById(manifest.hierarchyGrandchildId);
    if (child.parentId != parent.id || grandchild.parentId != child.id
        || parent.kind != EntityKind::Task || child.kind != EntityKind::Task)
        return bail(QStringLiteral("hierarchy"), QStringLiteral("父-子-孙链不正确"));
    if (!internalTargets(entityById(manifest.markdownLinkId).body).contains(manifest.markdownTargetId))
        return bail(QStringLiteral("links"), QStringLiteral("任务正文内部链接目标缺失"));
    if (!internalTargets(entityById(manifest.noteLinkId).body).contains(manifest.noteLinkTargetId))
        return bail(QStringLiteral("links"), QStringLiteral("笔记正文内部链接目标缺失"));
    log.pass(QStringLiteral("hierarchy+links: 父子孙链与 Markdown 内部链接一致"));

    // 状态：完成/放弃/回收站/置顶/收集箱。
    for (const QString &id : manifest.completedTaskIds) {
        const Entity value = entityById(id);
        if (value.status != TaskStatus::Completed || !value.completedAt.isValid())
            return bail(QStringLiteral("states"), QStringLiteral("完成态不正确: %1").arg(id));
    }
    for (const QString &id : manifest.abandonedTaskIds) {
        const Entity value = entityById(id);
        if (value.status != TaskStatus::Abandoned || !value.abandonedAt.isValid())
            return bail(QStringLiteral("states"), QStringLiteral("放弃态不正确: %1").arg(id));
    }
    for (const QString &id : manifest.trashedTaskIds) {
        const Entity value = entityById(id);
        if (!value.deleted || !value.deletedAt.isValid())
            return bail(QStringLiteral("states"), QStringLiteral("回收站态不正确: %1").arg(id));
    }
    if (!entityById(manifest.pinnedTaskId).pinnedAt.isValid())
        return bail(QStringLiteral("states"), QStringLiteral("置顶标记丢失"));
    int inboxCount = 0;
    for (const Entity &value : entities) if (value.listId == manifest.inboxListId) ++inboxCount;
    if (inboxCount < 3)
        return bail(QStringLiteral("states"), QStringLiteral("收集箱任务数量不足"));
    log.pass(QStringLiteral("states: 完成/放弃/回收站/置顶/收集箱全部一致"));

    // 附件：内容哈希 + 多附件实体。
    int attachments = 0;
    bool sawPng = false, sawCsv = false, sawText = false;
    for (const QString &entityId : manifest.attachmentEntityIds) {
        const QVector<Attachment> rows = store.attachments(entityId);
        attachments += rows.size();
        for (const Attachment &row : rows) {
            QFile file(store.attachmentFilePath(row));
            if (!file.open(QIODevice::ReadOnly))
                return bail(QStringLiteral("attachments"),
                    QStringLiteral("附件文件缺失: %1").arg(row.relativePath));
            const QByteArray content = file.readAll();
            file.close();
            if (content.size() != row.size || sha256Hex(content) != row.sha256
                || row.localState != AttachmentFileState::Available)
                return bail(QStringLiteral("attachments"),
                    QStringLiteral("附件内容校验失败: %1").arg(row.relativePath));
            if (row.isImage && row.mime != QStringLiteral("image/png"))
                return bail(QStringLiteral("attachments"), QStringLiteral("图片附件 MIME 不正确"));
            if (row.mime == QStringLiteral("image/png")) sawPng = true;
            if (row.mime == QStringLiteral("text/csv")) sawCsv = true;
            if (row.mime == QStringLiteral("text/plain")) sawText = true;
        }
    }
    if (attachments != manifest.attachments
        || store.attachments(manifest.attachmentHeavyEntityId).size() < 2
        || !sawPng || !sawCsv || !sawText)
        return bail(QStringLiteral("attachments"),
                          QStringLiteral("附件数量/类型与 manifest 不一致或多附件实体缺失"));
    log.pass(QStringLiteral("attachments: %1 个附件（PNG/文本/CSV，含多附件实体）内容与哈希一致")
                 .arg(attachments));

    // 桌面状态：六色、三层、仅两个打开。
    QSet<int> colorSet, layerSet;
    int openCount = 0;
    const NoteColor expectedColors[] = {NoteColor::Blue, NoteColor::Green, NoteColor::Yellow,
                                        NoteColor::Pink, NoteColor::Purple, NoteColor::White};
    const NoteLayer expectedLayers[] = {NoteLayer::Normal, NoteLayer::OnTop, NoteLayer::OnDesktop,
                                        NoteLayer::Normal, NoteLayer::OnDesktop, NoteLayer::OnTop};
    const int expectedZ[] = {1, 6, 2, 3, 4, 5};
    const bool expectedOpen[] = {true, true, false, false, false, false};
    if (manifest.desktopEntityIds.size() != 6)
        return bail(QStringLiteral("desktop"), QStringLiteral("桌面便签数量与期望不一致"));
    for (int i = 0; i < 6; ++i) {
        const DesktopNote note = store.desktopNote(manifest.desktopEntityIds.at(i));
        if (note.id.isEmpty() || note.color != expectedColors[i] || note.layer != expectedLayers[i]
            || note.open != expectedOpen[i] || note.zOrder != expectedZ[i])
            return bail(QStringLiteral("desktop"), QStringLiteral("桌面便签状态不正确"));
        colorSet.insert(colorToInt(note.color));
        layerSet.insert(layerToInt(note.layer));
        if (note.open) ++openCount;
    }
    if (colorSet.size() != ColorCount || layerSet.size() != LayerCount || openCount != 2)
        return bail(QStringLiteral("desktop"), QStringLiteral("六色/三层/打开数不满足要求"));
    log.pass(QStringLiteral("desktop: 六色、三层、仅 2 个打开"));

    // 活动事件。
    int events = 0;
    int attachmentEvents = 0;
    for (const Entity &value : entities) {
        const QVector<EntityEvent> rows = store.events(value.id);
        if (rows.isEmpty())
            return bail(QStringLiteral("events"), QStringLiteral("实体缺少活动事件: %1").arg(value.id));
        events += rows.size();
        for (const EntityEvent &row : rows)
            if (row.type == QStringLiteral("attachment_added")) ++attachmentEvents;
    }
    if (events != manifest.events || attachmentEvents != manifest.attachments)
        return bail(QStringLiteral("events"), QStringLiteral("事件数量或附件事件与 manifest 不一致"));
    log.pass(QStringLiteral("events: %1 条活动事件（含 %2 条附件事件）").arg(events).arg(attachmentEvents));

    // 备份容器与内容。
    QFile backupFile(backupPath(cleaned));
    if (!backupFile.open(QIODevice::ReadOnly))
        return bail(QStringLiteral("backup"), QStringLiteral("demo-backup.klb 缺失"));
    const QByteArray backupBytes = backupFile.readAll();
    backupFile.close();
    if (backupBytes.size() != static_cast<int>(manifest.backupBytes)
        || sha256Hex(backupBytes) != manifest.backupSha256)
        return bail(QStringLiteral("backup"), QStringLiteral("备份大小或 SHA256 与 manifest 不一致"));
    QBuffer buffer;
    buffer.setData(backupBytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return bail(QStringLiteral("backup"), QStringLiteral("无法读取备份字节"));
    QByteArray metadataBytes;
    QVector<attachmentcodec::BackupFrameInfo> frames;
    if (!attachmentcodec::readBackupContainer(&buffer, &metadataBytes, &frames, &failure))
        return bail(QStringLiteral("backup"), failure);
    buffer.close();
    const QJsonDocument metadataDocument = QJsonDocument::fromJson(metadataBytes);
    EntitySnapshot snapshot;
    QVector<EntityEvent> backupEvents;
    QString timezone;
    if (!metadataDocument.isObject()
        || !backupcodec::fromJson(metadataDocument.object(), &snapshot, &backupEvents, &timezone, &failure))
        return bail(QStringLiteral("backup"), QStringLiteral("备份元数据校验失败: ") + failure);
    if (snapshot.entities.size() != entities.size() || snapshot.attachments.size() != manifest.attachments
        || frames.size() != manifest.attachments)
        return bail(QStringLiteral("backup"), QStringLiteral("备份数量与数据库不一致"));
    bool successorLinkFound = false;
    const QJsonArray links = metadataDocument.object().value(QStringLiteral("recurrenceLinks")).toArray();
    for (const QJsonValue &value : links) {
        const QJsonObject link = value.toObject();
        if (link.value(QStringLiteral("sourceId")).toString() == manifest.recurringSourceId
            && link.value(QStringLiteral("nextId")).toString() == manifest.recurringNextId)
            successorLinkFound = true;
    }
    if (!successorLinkFound)
        return bail(QStringLiteral("backup"), QStringLiteral("备份缺少完成后继的重复凭证"));
    log.pass(QStringLiteral("backup: KLB1 容器/元数据/附件帧/完成后继凭证全部有效"));

    writeReport(cleaned, log, QStringLiteral("RESULT: PASS (%1 项检查)").arg(log.passed), report);
    if (error) error->clear();
    return true;
    } // TaskStore 作用域：先于 DatabaseCloser/guard/env 析构。
}

} // namespace demo
} // namespace kl
