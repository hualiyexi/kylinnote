#ifndef KYLINLIST_DEMODATA_H
#define KYLINLIST_DEMODATA_H

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QString>
#include <QStringList>

namespace kl {

class TaskStore;

// 合成演示数据：只通过 TaskStore 写业务数据，绝不直接执行 SQL。
// generate()/verify() 供 main.cpp 的 --seed-demo/--verify-demo 使用（QCoreApplication 阶段），
// populate() 单独暴露给测试，用调用方自己的 TaskStore 驱动同一份数据集。
namespace demo {

// manifest 的显式所有权格式：没有这份文件的目录绝不被当作演示数据处理。
const int kManifestVersion = 1;
inline QString manifestFormat() { return QStringLiteral("kylinlist.demo-manifest"); }
inline QString ownershipMarker() { return QStringLiteral("kylinlist-synthetic-demo"); }
inline QString ownershipPolicy()
{
    return QStringLiteral("This directory contains machine-generated synthetic demo data only. "
                          "tools/generate_test_data.py may quarantine (rename) it with --reset after "
                          "successful native --verify-demo. A directory without a valid manifest "
                          "must never be treated as demo data.");
}

inline QString manifestPath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("demo-manifest.json"));
}
inline QString reportPath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("demo-verify-report.txt"));
}
inline QString backupPath(const QString &dataDir)
{
    return QDir(dataDir).filePath(QStringLiteral("demo-backup.klb"));
}

struct Manifest {
    QString format;                    // manifestFormat()
    int version = kManifestVersion;
    // ownership
    QString marker;                    // ownershipMarker()
    bool owned = true;
    bool canReset = true;
    // generator
    QString generatorVersion;          // KYLINLIST_VERSION
    QString mode;                      // seed-demo
    QDateTime generatedAtUtc;
    QDate referenceDate;               // 生成时 Asia/Shanghai 的“今天”，校验锚点
    QString dataDir;                   // 绝对路径（规范化）
    // counts
    int folders = 0, lists = 0, sections = 0;
    int tasks = 0, notes = 0, entities = 0;
    int attachments = 0, reminders = 0, recurrenceSeries = 0;
    int desktopNotes = 0, events = 0;
    // ids
    QString inboxListId;
    QStringList folderIds, listIds, sectionIds, taskIds, noteIds;
    QStringList archivedListIds, completedTaskIds, abandonedTaskIds, trashedTaskIds;
    QString recurringSourceId, recurringNextId;             // 打卡完成 -> 后继实例
    QString recurringDailyId, recurringWeeklyId, recurringMonthlyId, recurringYearlyId;
    QString recurringLunarMonthlyId, recurringLunarYearlyId;
    QString hierarchyParentId, hierarchyChildId, hierarchyGrandchildId;
    QString markdownLinkId, markdownTargetId;               // 正文内部链接 -> 目标
    QString noteLinkId, noteLinkTargetId;
    QString reminderRelativeId, reminderAbsoluteId, reminderAllDayId, reminderNoteId;
    QString reminderScheduleAnchorId;       // Anchor::Schedule 的相对提醒
    QString attachmentHeavyEntityId;                        // 一条实体挂多个附件
    QStringList attachmentEntityIds, desktopEntityIds;
    QString pinnedTaskId;                                   // 置顶任务
    QString scheduleOverdueAllDayId, scheduleOverdueTimedId, scheduleTodayRangeId;
    QString scheduleOvernightId, scheduleMidnightId, scheduleTomorrowAllDayId, scheduleMultidayAllDayId;
    QString scheduleSameDayAllDayId;        // allDayDate == allDayEndDate 且显式时间段
    QString scheduleMultidayTimedId, scheduleFutureTimedId, scheduleFutureAllDayId;
    // backup
    QString backupFile;                 // demo-backup.klb
    qint64 backupBytes = 0;
    QString backupSha256;
};

QString manifestToJson(const Manifest &value);
bool manifestFromJson(const QByteArray &json, Manifest *out, QString *error);
bool writeManifest(const QString &dataDir, const Manifest &value, QString *error);
bool readManifest(const QString &dataDir, Manifest *out, QString *error);

// 目录安全检查：绝对路径、非文件系统根、无符号链接/根点组件、（seed 时）为空或不存在。
bool validateTargetDir(const QString &dataDir, bool requireEmpty, QString *error);

// 把整套演示数据写入已加载的 TaskStore；nowUtc 来自 appNow()，所有日程锚点由它推导。
// 首个失败即返回 false 并给出可读原因。
bool populate(TaskStore &store, const QDateTime &nowUtc, Manifest *out, QString *error);

// 完整种子流水线：目录检查 -> 单实例锁 -> 打开数据库 -> populate -> 导出并校验
// demo-backup.klb -> 写 demo-manifest.json。*locked 在锁被其他实例持有时置 true。
bool generate(const QString &dataDir, QString *error, bool *locked = nullptr);

// 重开全新 TaskStore 校验已生成的演示目录：数量/ID/字段/日程/提醒/重复/层级/链接/
// 附件/桌面状态/备份。逐项追加 report（首失败即止），并写 demo-verify-report.txt。
// *locked 在数据目录被其他实例持锁时置 true（与 seed 的退出码 3 对应）。
bool verify(const QString &dataDir, QString *report = nullptr, QString *error = nullptr,
            bool *locked = nullptr);

} // namespace demo
} // namespace kl

#endif // KYLINLIST_DEMODATA_H
