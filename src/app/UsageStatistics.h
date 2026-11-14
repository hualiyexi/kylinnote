#ifndef KYLINLIST_USAGESTATISTICS_H
#define KYLINLIST_USAGESTATISTICS_H

#include <QDateTime>
#include <QtGlobal>

namespace kl {

// 本地使用统计的纯值结构：只含聚合计数与最早任务时刻。
// 计算过程在本地用实体 id 去重，但结构本身（以及一切序列化结果）
// 绝不携带实体 id、标题、正文、路径或任何可识别个体内容。
struct UsageStatistics {
    qint64 tasksCreated = 0;    // 出现过 created 事件的 Task 实体数（含重复规则生成的实例）
    qint64 notesCreated = 0;    // 出现过 created 事件的 Note 实体数
    qint64 tasksCompleted = 0;  // 至少完成过一次的不同 Task 数；完成后再打开不重复计
    QDateTime firstTaskAt;      // 最早一个 Task 的 created 时刻；无效表示本机还没有任务

    bool hasFirstTask() const { return firstTaskAt.isValid(); }
    // now 距 firstTaskAt 的整天数（不足一天为 0）；还没有任务返回 -1。
    // 本机时钟早于首任务时间时按 0 处理，保证上报值非负。
    qint64 firstTaskDays(const QDateTime &now) const;
};

} // namespace kl

#endif // KYLINLIST_USAGESTATISTICS_H
