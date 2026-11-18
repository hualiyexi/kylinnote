#pragma once

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include "../domain/ScheduleValue.h"
#include "../domain/ViewSpec.h"
#include "design/DesignTokens.h"

class QLabel;
class QLineEdit;
class QStackedWidget;
class QToolButton;

namespace kl {

class PopupMenu;
class TaskDateController;
class TaskDelegate;
class TaskListModel;
class TaskListView;
class TaskStore;
class WindowResizeArea;

// 「任务」桌面小部件：共享第三列的模型、视图、委托与实体命令，
// 只负责独立窗口、范围选择和本实例的展示偏好，不维护第二份任务数据。
class TaskWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TaskWidget(TaskStore* store, QWidget* parent = nullptr);
    ~TaskWidget() override;

    static QSize minimumPaperSize();
    QRect paperGeometry() const;
    bool canDragWindow() const { return !m_locked; }
    QPoint constrainedWindowPosition(const QPoint& raw) const;

    // 托盘「添加桌面小部件 → 任务」与窗口关闭共用同一实例。
    void showWidget();
    void closeWidget();
    bool isOpen() const { return m_open; }

    // 锁定实现保留，目前界面和快捷键均不开放。
    static constexpr bool windowLockEnabled() { return false; }
    bool isLocked() const { return m_locked; }
    void setLocked(bool locked);
    void setRestoreFrozen(bool frozen);
    bool hasUncommittedDraft() const;
    bool flush();

    // 测试与选项菜单共用的当前口径。
    ViewSpec filter() const { return m_filter; }
    TaskListModel* model() const { return m_model; }
    QString scopeTitle() const { return m_scopeTitle; }
    int appearance() const { return m_appearance; }
    void selectScope(ViewScope scope, const QString& listId = QString());
    void setGrouping(GroupMode grouping);
    void setShowCompleted(bool enabled);
    void setShowDetails(bool enabled);
    void setCountdownDates(bool enabled);
    // 0 = 跟随应用，1 = 浅色，2 = 深色。
    void setAppearance(int appearance);

signals:
    void taskOpenRequested(QString id);
    void closed();
    void lockChanged(bool locked);

protected:
    void paintEvent(QPaintEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void applyFilter();
    void updateHeader();
    void updateEmptyState();
    void updateQuickDateVisibility();
    void createTask();
    void showScopeMenu();
    void showMoreMenu();
    void applyAppearance();
    ds::Theme resolvedTheme() const;
    void presentMenu(PopupMenu* menu, QWidget* anchor);
    void closePopups();
    bool saveState();
    void syncChromeGeometry();
    void scheduleGeometrySave();

    QPointer<TaskStore> m_store;
    ViewSpec m_filter;
    QString m_scopeTitle;
    TaskListModel* m_model = nullptr;
    TaskListView* m_list = nullptr;
    TaskDelegate* m_delegate = nullptr;
    TaskDateController* m_dateController = nullptr;
    QWidget* m_paper = nullptr;
    QWidget* m_header = nullptr;
    QToolButton* m_scope = nullptr;
    QToolButton* m_more = nullptr;
    QLineEdit* m_quick = nullptr;
    QToolButton* m_quickDate = nullptr;
    QStackedWidget* m_listStack = nullptr;
    QLabel* m_empty = nullptr;
    QVector<WindowResizeArea*> m_resizeAreas;
    QPointer<PopupMenu> m_menu;
    Entity m_quickDraft;
    ScheduleValue m_quickSchedule;
    QTimer m_geometryTimer;
    QImage m_shadow;
    QColor m_shadowColor;
    QSize m_shadowSize;
    QRect m_observedGeometry;
    int m_appearance = 2;  // 实测任务小部件默认深色
    bool m_quickDateCustomized = false;
    bool m_locked = false;
    bool m_open = false;
    bool m_loading = true;
};

}  // namespace kl
