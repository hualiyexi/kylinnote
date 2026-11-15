#pragma once

class QString;
class QTextDocument;

namespace kl {
class TaskListModel;
class TaskStore;

// 打印只读取当前列表的可见行；includeContent 控制是否附加正文和检查项。
void populateTaskPrintDocument(QTextDocument* document, const TaskListModel& model,
                               const TaskStore& store, const QString& title,
                               bool includeContent);

}  // namespace kl
