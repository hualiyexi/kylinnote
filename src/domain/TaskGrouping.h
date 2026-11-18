#ifndef KYLINLIST_TASKGROUPING_H
#define KYLINLIST_TASKGROUPING_H

#include "Entity.h"

#include <QDate>
#include <QHash>
#include <QMetaType>
#include <QSet>
#include <QString>
#include <QVector>

namespace kl {

struct ViewSpec;
enum class ViewScope;

// 分组模式：与 ViewSpec.renderer（筛选口径）正交，只决定任务列表按什么分节显示。
// 切换分组是纯展示行为：不改变筛选出的实体集合、不改写任何实体字段、不产生日志。
enum class GroupMode { List = 0, Time = 1, Created = 2, Priority = 3, None = 4 };

// 时间桶：overdue / today / tomorrow / next7days(today+2..today+6) / later / undated。
enum class TimeBucket { Overdue = 0, Today, Tomorrow, Next7Days, Later, Undated };

// 创建时间桶：过去7天(0..6天前) / 过去30天(7..29天前) / 更早(≥30天前)。
// 未来或缺失的 createdAt 统一落 Unknown 兜底桶。
enum class CreatedBucket { Last7Days = 0, Last30Days, Earlier, Unknown };

// 类型化分组键：一个组 = Kind + 少量字段，独立于筛选 renderer 存在。
// stableId() 是稳定的机器标识（供持久化折叠状态、跨端比对）：
//   pinned / list:<id> / time:overdue|today|tomorrow|next7days|later|undated
//   / created:last7|last30|earlier|unknown / priority:0..3 / none
//   / notes / completed / abandoned / trash
struct GroupKey {
    enum Kind {
        Invalid = 0,
        Pinned,      // 置顶（纯展示层汇总；真实字段是 pinnedAt）
        List,        // 清单组（字段 listId）
        Time,        // 时间桶（字段 bucket = TimeBucket）
        Created,     // 创建时间桶（字段 bucket = CreatedBucket）
        Priority,    // 优先级组（字段 bucket = 优先级 0..3，四级）
        Flat,        // GroupMode::None 的无标题普通任务组（stableId "none"）
        Notes,       // 笔记（所有分组模式下共用，排在普通组之后）
        Completed,   // 已完成（终态）
        Abandoned,   // 已放弃（终态）
        Trash        // 回收站（Trash 作用域单一段）
    };

    Kind kind = Invalid;
    QString listId;   // 仅 kind == List 有意义
    int bucket = 0;   // Time: TimeBucket；Created: CreatedBucket；Priority: 优先级 0..3

    static GroupKey pinned() { return GroupKey{Pinned, QString(), 0}; }
    static GroupKey forList(const QString &listId) { return GroupKey{List, listId, 0}; }
    static GroupKey forTime(TimeBucket bucket) { return GroupKey{Time, QString(), static_cast<int>(bucket)}; }
    static GroupKey forCreated(CreatedBucket bucket) { return GroupKey{Created, QString(), static_cast<int>(bucket)}; }
    static GroupKey forPriority(int priority) { return GroupKey{Priority, QString(), priority}; }
    static GroupKey flat() { return GroupKey{Flat, QString(), 0}; }
    static GroupKey notes() { return GroupKey{Notes, QString(), 0}; }
    static GroupKey completed() { return GroupKey{Completed, QString(), 0}; }
    static GroupKey abandoned() { return GroupKey{Abandoned, QString(), 0}; }
    static GroupKey trash() { return GroupKey{Trash, QString(), 0}; }
    // 解析 stableId；无法识别时返回 Invalid（不抛错、不误配）。
    static GroupKey fromStableId(const QString &id);

    bool isValid() const { return kind != Invalid; }
    bool isTerminal() const { return kind == Completed || kind == Abandoned || kind == Trash; }
    bool isModeGroup() const
    {
        return kind == List || kind == Time || kind == Created || kind == Priority || kind == Flat;
    }
    TimeBucket timeBucket() const { return static_cast<TimeBucket>(bucket); }
    CreatedBucket createdBucket() const { return static_cast<CreatedBucket>(bucket); }

    QString stableId() const;
    // 展示标题；Time 桶需要 today 补“今天 周X”。List 组在无清单表的重载下返回空，
    // 请改用带 lists 的重载解析清单名。
    QString title(const QDate &today = QDate()) const;
    QString title(const QVector<TaskList> &lists, const QDate &today = QDate()) const;

    bool operator==(const GroupKey &other) const;
    bool operator!=(const GroupKey &other) const { return !(*this == other); }
    // 全局固定顺序：置顶 → 模式组（模式内见下）→ 笔记 → Completed → Abandoned → Trash。
    // Time 按 TimeBucket 序、Created 按 CreatedBucket 序、Priority 按优先级高→低、
    // List 组之间按 listId 字典序（真正的展示顺序由分组构建器用清单元数据覆盖）。
    bool operator<(const GroupKey &other) const;
};

// 视图分类（显式参数，不改实体数据、不用副本伪装状态）：
// - History（Completed/Abandoned/Trash 作用域）：contains() 照旧筛出原集合，
//   结果集内按五模式实际分节（List 可含归档清单、Time 按实体日期、Created 按创建、
//   Priority 按优先级、None 无普通标题），不再折叠为单一终态段；
//   也没有置顶/笔记/终态页脚，置顶字段不参与分组。任何模式下拖放整体禁止。
// - Active（其余作用域）：保持已批准的全局顺序 置顶→模式组→笔记→Completed→Abandoned，
//   终态实体进置底页脚，不按模式再细分。
enum class ViewClass { Active = 0, History = 1 };
ViewClass viewClassOf(ViewScope scope);

// 单个分节。items 是组内顶层实体（直接子级嵌套随父项展示，不单独出现），
// 已按组内排序规则排好；Flat 组 title 为空（无标题但可排序）。
struct GroupedSection {
    GroupKey group;
    QString title;
    QVector<Entity> items;
};

// 分组结果：sections 只包含非空组，按全局固定顺序排列；
// children 是可见实体（含子级）的直接子级映射，同样按组内排序规则排好。
// “按同状态同清单父子层级继承显示组”已在 sections 中体现：子级永远与其显示父项
// 同组，不会出现子级跨组跟随；终态（Completed/Abandoned）子级不跟随 active 父组。
struct Grouping {
    QVector<GroupedSection> sections;
    QHash<QString, QVector<Entity>> children;
};

// 核心纯规则：对 entities 应用 ViewSpec 筛选（contains，含归档清单/文件夹口径），
// 按 spec.grouping 分节，解析父子显示组继承，并完成组内排序。
// Folder 作用域自动按文件夹成员清单过滤；历史视角（Completed/Abandoned/Trash）在
// 结果集内按五模式分节（见 ViewClass），不改变筛选口径与 ID 集合。
Grouping groupEntities(const QVector<Entity> &entities, const QVector<TaskList> &lists,
                       const ViewSpec &spec, const QDate &today,
                       const QSet<QString> &archivedListIds = QSet<QString>());

// 单实体的属性分组（不考虑父子继承）。Active 视图：置顶优先，其次笔记/终态，
// 最后按模式分桶；History 视图：忽略置顶/笔记/终态区分，纯按模式分桶。
GroupKey groupOf(const Entity &entity, GroupMode mode, const QDate &today,
                 ViewClass view = ViewClass::Active);
TimeBucket timeBucketOf(const Entity &entity, const QDate &today);
CreatedBucket createdBucketOf(const Entity &entity, const QDate &today);

// 组内排序：sortOrder ASC → 模式自然字段（Time: effectiveDate 升序；Created: createdAt
// 降序；Priority/List/Flat/Pinned: effectiveDate 升序再 createdAt 降序）→ id 兜底。
// 终态组（Completed/Abandoned/Trash）例外：终态时间降序 → createdAt 降序 → id。
bool groupLessThan(const Entity &a, const Entity &b, GroupMode mode, const QDate &today);

// List 分组参与排序的清单（按清单元数据顺序：sortOrder → name → id）；
// Folder 作用域 = 文件夹内未归档清单；其余 Active 作用域 = 全部未归档清单；
// 历史视角允许包含归档清单（Completed/Abandoned/Trash 保留其历史可见性）。
QVector<TaskList> groupingLists(const QVector<TaskList> &lists, const ViewSpec &spec);

// 作用域默认分组：文件夹 List，其余 Time。List 作用域下 UI 不提供 List 选项。
GroupMode defaultGrouping(ViewScope scope);

// 模式名（Settings/ViewSpec 持久化用）：list / time / created / priority / none。
QString groupModeName(GroupMode mode);
GroupMode groupModeFromName(const QString &name, GroupMode fallback = GroupMode::Time);

// Created 分组按创建时间固定排序，禁止拖动重排；其余分组允许拖动。
inline bool groupingAllowsDrag(GroupMode mode) { return mode != GroupMode::Created; }

} // namespace kl

Q_DECLARE_METATYPE(kl::GroupKey)

#endif // KYLINLIST_TASKGROUPING_H
