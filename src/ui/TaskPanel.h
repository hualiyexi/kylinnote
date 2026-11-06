#pragma once
#include <QMainWindow>
#include "TaskFilter.h"
class QListWidget;
class QListView;
class QLabel;
class QLineEdit;
class QComboBox;
class QSplitter;
class QStackedWidget;
class QCloseEvent;
namespace kn {
class TaskStore;
class TaskListModel;
class TaskDetailWidget;
class TaskPanel : public QMainWindow {
    Q_OBJECT
public:
    explicit TaskPanel(TaskStore* store, QWidget* parent = nullptr);
    bool flush();
    void showTask(qint64 id = 0);
    void focusQuickAdd();
signals:
    void quitRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void rebuildNavigation();
    void applyFilter();
    void createTask();
    void showTaskMenu(const QPoint& position);
    void groupMenu();
    void backupMenu();
    void showError(const QString& message);
    TaskStore* m_store;
    TaskListModel* m_model = nullptr;
    TaskDetailWidget* m_detail = nullptr;
    QListWidget* m_navigation = nullptr;
    QListView* m_list = nullptr;
    QLineEdit *m_search = nullptr, *m_quick = nullptr;
    QComboBox* m_sort = nullptr;
    QLabel *m_heading = nullptr, *m_count = nullptr, *m_empty = nullptr, *m_error = nullptr;
    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_listStack = nullptr;
    TaskFilter m_filter;
    qint64 m_selectedId = 0;
    bool m_resetting = false, m_rebuilding = false;
};
}  // namespace kn
