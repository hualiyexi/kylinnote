#pragma once
#include "../domain/Task.h"
namespace kn {
enum class TaskView { All, Today, Upcoming, Overdue, Important, Pinned, Completed, Trash, Group };
enum class TaskSort { Schedule, Priority, Created, Title };
struct TaskFilter {
    TaskView view = TaskView::All;
    qint64 groupId = 0;
    QString search;
    TaskSort sort = TaskSort::Schedule;
    bool matches(const Task& task, const QDateTime& now) const;
    bool lessThan(const Task& left, const Task& right) const;
};
QString scheduleLabel(const Task& task);
QString priorityLabel(int priority);
QString recurrenceLabel(const QString& recurrence);
}  // namespace kn
