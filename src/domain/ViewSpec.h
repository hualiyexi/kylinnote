#ifndef KYLINLIST_VIEWSPEC_H
#define KYLINLIST_VIEWSPEC_H

#include "Entity.h"

#include <QSet>

namespace kl {

enum class ViewRenderer { List = 0, Time = 1 };
// Tomorrow/Folder 追加在枚举尾部，保持既有取值稳定。
enum class ViewScope { AllLists = 0, List, Today, Next7Days, Overdue, Completed, Abandoned, Trash, Tomorrow, Folder };

struct ViewSpec {
    ViewScope scope = ViewScope::AllLists;
    QString listId;
    QString folderId;
    ViewRenderer renderer = ViewRenderer::List;
    QString search;
};

QDate effectiveDate(const Entity &entity);
// archivedListIds：当前已归档清单的 id 集合。AllLists/Today/Tomorrow/Next7Days/Overdue
// 借此排除归档清单的活动实体；显式 ViewScope::List 与 Completed/Abandoned/Trash 不受影响。
// 缺省空集合维持旧行为，现有调用无需修改。
bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today,
              const QSet<QString> &archivedListIds = QSet<QString>(),
              const QSet<QString> &folderListIds = QSet<QString>());

// 稳定的机器标识，供模型层区分分区，不依赖中文标题：
// pinned / overdue / upcoming / undated / notes / completed / abandoned / trash。
struct ViewSection {
    QString key;
    QString title;
    QVector<Entity> items;
};
QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today,
                                   const QSet<QString> &archivedListIds = QSet<QString>());

QSet<QString> folderListIds(const QVector<TaskList> &lists, const QString &folderId);
// 文件夹视图：置顶跨清单汇总，其余活动实体按清单分节。
QVector<ViewSection> buildFolderSections(const QVector<Entity> &entities, const QVector<TaskList> &lists,
                                         const ViewSpec &spec, const QDate &today,
                                         const QSet<QString> &archivedListIds = QSet<QString>());

// 与视图同属纯展示语义，供模型、便签和提醒弹窗共享。
QString scheduleLabel(const Entity &entity);
QString priorityLabel(int priority);
QString recurrenceLabel(const QString &recurrence);

} // namespace kl

#endif // KYLINLIST_VIEWSPEC_H
