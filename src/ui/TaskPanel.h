#ifndef KYLINLIST_TASKPANEL_H
#define KYLINLIST_TASKPANEL_H

#include <QMainWindow>
#include "../domain/ViewSpec.h"
class QListWidget;
class QListView;
class QLabel;
class QLineEdit;
class QComboBox;
class QSplitter;
class QStackedWidget;
class QCloseEvent;
namespace kl {
class TaskStore;
class TaskListModel;
class TaskDetailWidget;
class TaskPanel : public QMainWindow {
    Q_OBJECT
public:
    explicit TaskPanel(TaskStore* store, QWidget* parent = nullptr);
    bool flush();
    void setRestoreFrozen(bool frozen);
    void resetAfterRestore();
    void showTask(QString id = QString());
    void focusQuickAdd();
signals:
    void quitRequested();
    void importBackupRequested(QString path);
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void buildUi();
    void rebuildNavigation();
    void applyFilter();
    void createTask();
    void updateQuickAddKind();
    void showTaskMenu(const QPoint& position);
    void organizationMenu(const QPoint& position);
    void groupMenu();
    void backupMenu();
    void showError(const QString& message);
    TaskStore* m_store;
    TaskListModel* m_model = nullptr;
    TaskDetailWidget* m_detail = nullptr;
    QListWidget* m_navigation = nullptr;
    QListView* m_list = nullptr;
    QLineEdit *m_search = nullptr, *m_quick = nullptr;
    QLabel* m_quickKind = nullptr;
    QComboBox* m_renderer = nullptr;
    QLabel *m_heading = nullptr, *m_count = nullptr, *m_empty = nullptr, *m_error = nullptr;
    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_listStack = nullptr;
    ViewSpec m_filter;
    QString m_selectedId = QString();
    bool m_resetting = false, m_rebuilding = false, m_restoreFrozen = false;
};
}  // namespace kl

#endif // KYLINLIST_TASKPANEL_H
