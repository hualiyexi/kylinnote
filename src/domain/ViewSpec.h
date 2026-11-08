#ifndef KYLINLIST_VIEWSPEC_H
#define KYLINLIST_VIEWSPEC_H

#include "Entity.h"

namespace kl {

enum class ViewRenderer { List = 0, Time = 1 };
enum class ViewScope { AllLists = 0, List, Today, Next7Days, Overdue, Completed, Abandoned, Trash };

struct ViewSpec {
    ViewScope scope = ViewScope::AllLists;
    QString listId;
    ViewRenderer renderer = ViewRenderer::List;
    QString search;
};

QDate effectiveDate(const Entity &entity);
bool contains(const Entity &entity, const ViewSpec &spec, const QDate &today);

struct ViewSection {
    QString title;
    QVector<Entity> items;
};
QVector<ViewSection> buildSections(const QVector<Entity> &entities, const ViewSpec &spec, const QDate &today);

// 与视图同属纯展示语义，供模型、便签和提醒弹窗共享。
QString scheduleLabel(const Entity &entity);
QString priorityLabel(int priority);
QString recurrenceLabel(const QString &recurrence);

} // namespace kl

#endif // KYLINLIST_VIEWSPEC_H
