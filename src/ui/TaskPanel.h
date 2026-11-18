#ifndef KYLINLIST_TASKPANEL_H
#define KYLINLIST_TASKPANEL_H

#include <QMainWindow>
#include <QHash>
#include <QList>
#include <QSet>
#include "../domain/ScheduleValue.h"
#include <QPointer>
#include "../domain/ViewSpec.h"
class QListWidget;
class QListView;
class QLabel;
class QLineEdit;
class QSplitter;
class QStackedWidget;
class QCloseEvent;
class QToolButton;
class QFrame;
namespace kl {
class TaskStore;
class AutostartSettings;
class TaskListModel;
class TaskListView;
class NavigationListWidget;
class TaskDetailWidget;
class RelatedDetailPopup;
class TaskDateController;
class WindowControls;
class WindowResizeArea;
class SplitterResizeArea;
class OverlayResizeArea;
class OverlayEdgeShadow;
class WorkspaceSurface;
class WindowBorderOverlay;
class ToastIsland;
class TaskPanel : public QMainWindow {
    Q_OBJECT
public:
    explicit TaskPanel(TaskStore* store, QWidget* parent = nullptr);
    ~TaskPanel() override;
    void setAutostart(AutostartSettings* autostart);
    bool flush();
    void setRestoreFrozen(bool frozen);
    void resetAfterRestore();
    void showTask(QString id = QString());
    void openRelatedDetail(QString id);
    RelatedDetailPopup* relatedDetailPopup() const { return m_relatedDetail; }
    void focusQuickAdd();
    // 托盘「置顶应用」：只改变主窗口层级，不影响桌面便签或小部件。
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    void setAlwaysOnTop(bool enabled);
    // 快速添加框中的未提交输入不属于 flush() 保存范围。
    bool hasUncommittedDrafts(QString* what = nullptr) const;
    void showUpdateNotice(const QString& message);
signals:
    void quitRequested();
    void importBackupRequested(QString path);
    void updateCheckRequested();
    void updateSettingsRequested();
    void statsRequested();
    void taskWidgetRequested();
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
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
    void syncGroupingPreference();
    void setGrouping(GroupMode mode);
    void createTask();
    void updateQuickAddKind();
    void updateQuickDateButton();
    void updateQuickDateVisibility();
    void showTaskMenu(const QPoint& position);
    void organizationMenu(const QPoint& position);
    void groupMenu();
    void createListDialog(const QString& folderId, EntityKind kind);
    void editListDialog(const TaskList& list);
    void showSettings();
    void accountMenu();
    void dataMenu();
    void workspaceMenu();
    void showError(const QString& message);
    void showNotice(const QString& message);
    void showUndoableStatus(const QString& id, TaskStatus previous);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setDetailVisible(bool visible);
    void setSidebarVisible(bool visible, bool userInitiated = false);
    void reconcileWorkspaceLayout();
    void updateDetailOverlayGeometry();
    void refreshWorkspaceIcons();
    void updateHeadingAppearance();
    void updateWindowControlsPosition();
    void raiseWindowChrome();
    void syncWindowDecoration();
    void syncColumnResizeAreas();
    void selectNavigation(ViewScope scope, const QString& listId = QString());
    TaskStore* m_store;
    QPointer<AutostartSettings> m_autostart;
    TaskListModel* m_model = nullptr;
    TaskDetailWidget* m_detail = nullptr;
    RelatedDetailPopup* m_relatedDetail = nullptr;
    TaskDateController* m_dateController = nullptr;
    NavigationListWidget* m_navigation = nullptr;
    TaskListView* m_list = nullptr;
    QLineEdit* m_quick = nullptr;
    QLabel* m_quickKind = nullptr;
    QToolButton* m_workspaceMore = nullptr;
    QLabel *m_heading = nullptr, *m_empty = nullptr, *m_error = nullptr;
    QSplitter* m_splitter = nullptr;
    QStackedWidget* m_listStack = nullptr;
    QFrame* m_detailFrame = nullptr;
    QFrame* m_sidebar = nullptr;
    QFrame* m_middle = nullptr;
    QFrame* m_activityRail = nullptr;
    QFrame* m_quickFrame = nullptr;
    WindowControls* m_windowControls = nullptr;
    WorkspaceSurface* m_surface = nullptr;
    WindowBorderOverlay* m_border = nullptr;
    QList<WindowResizeArea*> m_resizeAreas;
    QList<SplitterResizeArea*> m_columnResizeAreas;
    OverlayEdgeShadow* m_detailOverlayShadow = nullptr;
    OverlayResizeArea* m_overlayResizeArea = nullptr;
    QToolButton *m_railTasks = nullptr, *m_railSettings = nullptr;
    QToolButton* m_sidebarToggle = nullptr;
    QToolButton* m_quickDate = nullptr;
    Entity m_quickDraft;
    ScheduleValue m_quickSchedule;
    bool m_quickDateCustomized = false;
    ToastIsland* m_toast = nullptr;
    ViewSpec m_filter;
    QString m_groupingPreferenceKey;
    QSet<QString> m_archivedListIds;
    QSet<QString> m_collapsedFolders;
    QString m_selectedId = QString();
    QHash<QString, TaskStatus> m_taskStates;
    int m_sidebarWidth = 232;
    int m_detailWidth = 370;
    int m_overlayWidth = 300;
    bool m_sidebarUserHidden = false;
    bool m_detailAutoHidden = false;
    bool m_sidebarAutoHidden = false;
    bool m_preferDetailOverlay = true;
    bool m_detailOverlay = false;
    bool m_focusMode = false;
    bool m_archivedExpanded = false;
    bool m_listSectionCollapsed = false;
    bool m_keepHiddenView = false;
    bool m_layoutUpdating = false;
    bool m_restoringLayout = false;
    bool m_resetting = false, m_rebuilding = false, m_restoreFrozen = false;
    bool m_windowDragging = false;
    bool m_alwaysOnTop = false;
    QPoint m_windowDragOffset;
};
}  // namespace kl

#endif // KYLINLIST_TASKPANEL_H
