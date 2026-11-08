#ifndef KYLINLIST_ACTIVITYLOGDIALOG_H
#define KYLINLIST_ACTIVITYLOGDIALOG_H

#include <QDialog>
#include <QPointer>
#include "../domain/Entity.h"
class QTreeWidget;
class QTextEdit;
namespace kl {
class TaskStore;
class ActivityLogDialog : public QDialog {
    Q_OBJECT
public:
    explicit ActivityLogDialog(TaskStore* store, const QString& entityId, QWidget* parent = nullptr);
private:
    void showEventDetails();
    QPointer<TaskStore> m_store;
    QString m_entityId;
    QTreeWidget* m_tree = nullptr;
    QTextEdit* m_details = nullptr;
};
} // namespace kl

#endif // KYLINLIST_ACTIVITYLOGDIALOG_H
