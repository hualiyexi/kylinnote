#ifndef KYLINLIST_TASKPANEL_H
#define KYLINLIST_TASKPANEL_H

#include <QMainWindow>
#include <QHash>
#include "../domain/ViewSpec.h"
class QListWidget;
class QListView;
class QLabel;
class QLineEdit;
class QComboBox;
class QSplitter;
class QStackedWidget;
class QCloseEvent;
class QToolButton;
class QFrame;
namespace kl {
class TaskStore;
class TaskListModel;
class TaskDetailWidget;
class WindowControls;
class WorkspaceSurface;
class ToastIsland;
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
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
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
    void workspaceMenu();
    void showError(const QString& message);
    void showNotice(const QString& message);
    void showUndoableStatus(const QString& id, TaskStatus previous);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setDetailVisible(bool visible);
    void refreshWorkspaceIcons();
    void updateWindowControlsPosition();
    void selectNavigation(ViewScope scope, const QString& listId = QString());
    TaskStore* m_store;
    TaskListModel* m_model = nullptr;
    TaskDetailWidget* m_detail = nullptr;
    QListWidget* m_navigation = nullptr;
    QListView* m_list = nullptr;
    QLineEdit *m_search = nullptr, *m_quick = nullptr;
    QLabel* m_quickKind = nullptr;
    QComboBox* m_renderer = nullptr;
    QToolButton* m_workspaceMore = nullptr;
    QLabel *m_heading = nullptr, *m_count = nullptr, *m_empty = nullptr, *m_error = nullptr;
    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_listStack = nullptr;
    QFrame* m_detailFrame = nullptr;
    QFrame* m_sidebar = nullptr;
    QFrame* m_middle = nullptr;
    QFrame* m_activityRail = nullptr;
    WindowControls* m_windowControls = nullptr;
    WorkspaceSurface* m_surface = nullptr;
    QToolButton *m_railTasks = nullptr, *m_railSearch = nullptr, *m_railSettings = nullptr;
    QToolButton* m_quickDate = nullptr;
    ToastIsland* m_toast = nullptr;
    ViewSpec m_filter;
    QString m_selectedId = QString();
    QHash<QString, TaskStatus> m_taskStates;
    bool m_resetting = false, m_rebuilding = false, m_restoreFrozen = false;
    bool m_windowDragging = false;
    QPoint m_windowDragOffset;
};
}  // namespace kl

#endif // KYLINLIST_TASKPANEL_H
