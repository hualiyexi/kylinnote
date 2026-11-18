// TaskDrop 的纯规则实现：分组目标语义（日期/优先级/清单）与 Intent 结构校验。
// Store 的 applyDropIntent 以这里为单一事实源，避免视图与存储各自映射漂移。
#include "TaskDrop.h"

namespace kl {
namespace TaskDrop {

QDate dateForGroup(const GroupKey &group, const QDate &today)
{
    if (!today.isValid() || group.kind != GroupKey::Time) return QDate();
    switch (group.timeBucket()) {
    case TimeBucket::Overdue: return today.addDays(-1);
    case TimeBucket::Today: return today;
    case TimeBucket::Tomorrow: return today.addDays(1);
    case TimeBucket::Next7Days: return today.addDays(2);
    case TimeBucket::Later: return today.addDays(7);   // 最近7天窗口(today+2..+6)之后的第一天
    case TimeBucket::Undated: return QDate();          // undated 语义 = 清除日期
    }
    return QDate();
}

int priorityForGroup(const GroupKey &group)
{
    if (group.kind != GroupKey::Priority) return -1;
    return qBound(0, group.bucket, 3);
}

QString listForGroup(const GroupKey &group)
{
    return group.kind == GroupKey::List ? group.listId : QString();
}

bool Intent::isValid() const
{
    if (sourceId.isEmpty() || !sourceGroup.isValid() || !targetGroup.isValid()) return false;
    if (!groupingAllowsDrag(mode)) return false;               // Created 分组禁拖
    if (orderedIds.isEmpty() || !orderedIds.contains(sourceId)) return false;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QSet<QString> uniqueIds(orderedIds.begin(), orderedIds.end());
#else
    const QSet<QString> uniqueIds = QSet<QString>::fromList(orderedIds);
#endif
    if (uniqueIds.size() != orderedIds.size()) return false;
    if (targetGroup.isTerminal()) {
        // 终态组不是拖放目标：状态切换走完成/放弃命令。
        return false;
    }
    if (targetGroup.kind == GroupKey::Notes
        && sourceGroup.kind != GroupKey::Notes && sourceGroup.kind != GroupKey::Pinned) {
        // Notes 仅接受已有笔记（同组排序，或置顶笔记取消置顶）；任务不做隐式转换。
        return false;
    }
    switch (placement) {
    case Placement::AsChild:
        if (targetParentId.isEmpty() || targetParentId == sourceId) return false;
        break;
    case Placement::Before:
    case Placement::After:
        if (anchorId.isEmpty() || anchorId == sourceId) return false;
        if (!targetParentId.isEmpty()) return false;
        break;
    case Placement::IntoGroup:
        if (!anchorId.isEmpty() || !targetParentId.isEmpty()) return false;
        break;
    }
    return true;
}

} // namespace TaskDrop
} // namespace kl
