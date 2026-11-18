#ifndef KYLINLIST_VIEWSPEC_H
#define KYLINLIST_VIEWSPEC_H

#include "Entity.h"
#include "TaskGrouping.h"

#include <QSet>

namespace kl {

enum class ViewRenderer { List = 0, Time = 1 };
// Tomorrow/Folder 追加在枚举尾部，保持既有取值稳定。
enum class ViewScope { AllLists = 0, List, Today, Next7Days, Overdue, Completed, Abandoned, Trash, Tomorrow, Folder };

struct ViewSpec {
    ViewScope scope = ViewScope::AllLists;
    QString listId;
    QString folderId;
    ViewRenderer renderer = ViewRenderer::List;   // 仅筛选口径：Time 渲染器限定时间作用域
    GroupMode grouping = GroupMode::Time;         // 分节显示分组，与 renderer 正交
    QString search;
    // 纯展示选项，不写入任务数据；同时放行 Completed 与 Abandoned 两组终态。
    bool showCompleted = false;
    bool showDetails = false;
    bool countdownDates = false;
    // 任务桌面小部件使用：只显示 EntityKind::Task，不改变主窗口的混合列表口径。
    bool tasksOnly = false;
};

// effectiveDate 是截止/逾期锚点；范围可见性与排序分别使用以下函数，不再隐式共用。
QDate effectiveDate(const Entity &entity);
QDate scheduleStartDate(const Entity &entity);
QDate scheduleEndDate(const Entity &entity);
bool scheduleIntersects(const Entity &entity, const QDate &first, const QDate &last);
// archivedListIds：当前已归档清单的 id 集合。AllLists/Today/Tomorrow/Next7Days/Overdue
// 借此排除归档清单的活动实体；显式 ViewScope::List 与 Completed/Abandoned/Trash 不受影响。
// 缺省空集合维持旧行为，现有调用无需修改。
bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today,
              const QSet<QString> &archivedListIds = QSet<QString>(),
              const QSet<QString> &folderListIds = QSet<QString>());

// 兼容视图段结构：key 为 GroupKey::stableId()，title 为组标题，
// items 为组内全部实体（含子级，按显示顺序展平）。
struct ViewSection {
    QString key;
    QString title;
    QVector<Entity> items;
};
// 按 spec.grouping 分节（委托 TaskGrouping::groupEntities），并展平父子层级。
QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today,
                                   const QSet<QString> &archivedListIds = QSet<QString>());

QSet<QString> folderListIds(const QVector<TaskList> &lists, const QString &folderId);
// 文件夹视图分节：同样按 spec.grouping 分组（文件夹默认 List 分组由调用方设置）。
QVector<ViewSection> buildFolderSections(const QVector<Entity> &entities, const QVector<TaskList> &lists,
                                         const ViewSpec &spec, const QDate &today,
                                         const QSet<QString> &archivedListIds = QSet<QString>());

// 与视图同属纯展示语义，供模型、便签和提醒弹窗共享。
QString scheduleLabel(const Entity &entity);
QString priorityLabel(int priority);
QString recurrenceLabel(const QString &recurrence);

} // namespace kl

#endif // KYLINLIST_VIEWSPEC_H
