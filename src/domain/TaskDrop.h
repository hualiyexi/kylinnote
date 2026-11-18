#ifndef KYLINLIST_TASKDROP_H
#define KYLINLIST_TASKDROP_H

#include "TaskGrouping.h"

#include <QDate>
#include <QString>
#include <QStringList>

namespace kl {
namespace TaskDrop {

// 落点类型：参照行之前/之后、落入组（组头或空组/空白区）、右下 24px 设父。
enum class Placement { Before = 0, After, IntoGroup, AsChild };

// 拖放意图：视图只陈述“发生了什么”，不预设业务改写。
// 离 pin 清 pin、跨组子项脱父、日期/优先级/清单改写等原子提交全部由 TaskStore
// 依据 sourceGroup/targetGroup/orderedIds/targetParentId 对照真实实体数据完成；
// 源/目标的“显示分组”（可能来自父子继承）与实体真实属性由 Store 侧区分处理。
struct Intent {
    QString sourceId;       // 被拖实体 id
    GroupMode mode = GroupMode::None;   // 手势发生时的分组模式
    GroupKey sourceGroup;   // 拖动前源所在的显示组（可能为 Pinned）
    GroupKey targetGroup;   // 落点显示组
    Placement placement = Placement::IntoGroup;

    QString anchorId;       // Before/After 的参照实体行 id；IntoGroup 为空
    QString targetParentId; // 仅 AsChild 非空：右下 24px 设父的目标父任务

    // 目标组扁平显示顺序（已含 source 及其可见子树块）。
    // Store 按此顺序规范化目标组 sortOrder（0..n-1，只写实际变化的行）。
    QStringList orderedIds;

    // 时间组的目标日期（dateForGroup 解析）；非时间组保持无效，Store 复核一致性。
    QDate targetDate;

    bool isValid() const;   // 结构校验：sourceId/两组有效、orderedIds 含 source、
                            // AsChild 必有 targetParentId、Before/After 必有 anchorId
};

// 分组目标语义（Store 消费，保持单一事实源）：
// 时间组 → 目标日期：overdue=today-1、today=today、tomorrow=today+1、
// next7days=today+2、later=today+7（最近7天窗口之后的第一天）；
// undated 及非时间组返回无效 QDate（undated 语义 = 清除日期）。
QDate dateForGroup(const GroupKey &group, const QDate &today);

// 优先级组 → 目标优先级（0..3）；非优先级组返回 -1。
int priorityForGroup(const GroupKey &group);

// 清单组 → 目标清单 id；非清单组返回空。
QString listForGroup(const GroupKey &group);

} // namespace TaskDrop
} // namespace kl

Q_DECLARE_METATYPE(kl::TaskDrop::Intent)

#endif // KYLINLIST_TASKDROP_H
