#include "TaskPanel.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTimeZone>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include "../app/AutostartSettings.h"
#include "../app/Branding.h"
#include "../app/Settings.h"
#include "../app/TaskStore.h"
#include "../domain/TaskDrop.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "BrandingIcons.h"
#include "DisplaySettingsDialog.h"
#include "EntityCommands.h"
#include "FolderEditorDialog.h"
#include "ListAppearance.h"
#include "ListEditorDialog.h"
#include "ModalDialog.h"
#include "NavigationGeometry.h"
#include "NavigationListWidget.h"
#include "PopupMenu.h"
#include "RelatedDetailPopup.h"
#include "SettingsDialog.h"
#include "SidebarSectionHeader.h"
#include "TaskDelegate.h"
#include "TaskDateController.h"
#include "TaskDetailWidget.h"
#include "TaskListModel.h"
#include "TaskListView.h"
#include "ToastIsland.h"
#include "WindowChrome.h"
#include "WindowBorderOverlay.h"
#include "WorkspaceGeometry.h"
#include "WorkspaceSurface.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace kl {
namespace {

QString scopeLabel(ViewScope scope) {
    switch (scope) {
        case ViewScope::AllLists: return QStringLiteral("所有");
        case ViewScope::Today: return QStringLiteral("今天");
        case ViewScope::Tomorrow: return QStringLiteral("明天");
        case ViewScope::Next7Days: return QStringLiteral("最近7天");
        case ViewScope::Overdue: return QStringLiteral("已过期");
        case ViewScope::Completed: return QStringLiteral("已完成");
        case ViewScope::Abandoned: return QStringLiteral("已放弃");
        case ViewScope::Trash: return QStringLiteral("垃圾桶");
        case ViewScope::Folder:
        case ViewScope::List: break;
    }
    return QStringLiteral("清单");
}

// R3: 导航分区标签
IconId scopeIcon(ViewScope scope) {
    switch (scope) {
        case ViewScope::AllLists: return IconId::All;
        case ViewScope::Today: return IconId::Today;
        case ViewScope::Tomorrow: return IconId::Tomorrow;
        case ViewScope::Next7Days: return IconId::Upcoming;
        case ViewScope::Overdue: return IconId::Overdue;
        case ViewScope::Completed: return IconId::Done;
        case ViewScope::Abandoned: return IconId::Abandoned;
        case ViewScope::Trash: return IconId::Trash;
        case ViewScope::Folder: return IconId::Folder;
        case ViewScope::List: break;
    }
    return IconId::TaskList;
}

}  // namespace

TaskPanel::TaskPanel(TaskStore* store, QWidget* parent) : QMainWindow(parent), m_store(store) {
    setObjectName(QStringLiteral("TaskPanel"));
    setWindowTitle(Branding::displayName());
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    QSettings startupSettings(QSettings::IniFormat, QSettings::UserScope,
                              QStringLiteral("kylinlist"), QStringLiteral("panel"));
    m_alwaysOnTop = startupSettings.value(QStringLiteral("alwaysOnTop"), false).toBool();
    if (m_alwaysOnTop) setWindowFlag(Qt::WindowStaysOnTopHint, true);
    if (auto* oldStatusBar = findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly))
        delete oldStatusBar;
    setMinimumSize(620, 560); resize(1240, 820);
    buildUi();
#ifdef Q_OS_WIN
    // Qt 5.12 的 FramelessWindowHint 使用 WS_POPUP；仅补回可缩放边框样式，
    // 系统标题栏仍由 WM_NCCALCSIZE 移除，透明背景和应用自绘边框保持不变。
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    for (const Entity& entity : m_store->entities()) m_taskStates.insert(entity.id, entity.status);
    connect(m_detail, &TaskDetailWidget::internalLinkRequested, this, &TaskPanel::showTask);
    connect(m_detail, &TaskDetailWidget::relatedDetailRequested, this, &TaskPanel::openRelatedDetail);
    rebuildNavigation();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    m_sidebarWidth = qBound(180, settings.value(QStringLiteral("sidebarWidth"), AppStyle::metric(AppStyle::Metric::SidebarWidth)).toInt(), 360);
    m_detailWidth = qBound(300, settings.value(QStringLiteral("detailWidth"), 370).toInt(), 560);
    m_sidebarUserHidden = settings.value(QStringLiteral("sidebarHidden"), false).toBool();
    m_filter.showCompleted = settings.value(QStringLiteral("showCompleted"), false).toBool();
    m_filter.showDetails = settings.value(QStringLiteral("showDetails"), false).toBool();
    m_filter.countdownDates = settings.value(QStringLiteral("countdownDates"), false).toBool();
    m_preferDetailOverlay = settings.value(QStringLiteral("detailPopup"), true).toBool();
    m_model->setFilter(m_filter);
    m_restoringLayout = true;
    if (m_splitter->count() == 3) {
        const QList<int> restored = m_splitter->sizes();
        if (restored.value(0) >= 180) m_sidebarWidth = qMin(360, restored.at(0));
        if (restored.value(2) >= 300) m_detailWidth = restored.at(2);
    }
    m_restoringLayout = false;
    if (QGuiApplication::primaryScreen()) {
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        resize(size().boundedTo(available.size()));
        if (!available.intersects(frameGeometry())) move(available.topLeft() + QPoint(20, 20));
    }
    connect(store, &TaskStore::taskChanged, this, [this](const QString& id) {
        m_error->hide(); if (QWidget* host = m_error->parentWidget()) host->hide();
        rebuildNavigation();
        const Entity current = m_store->entity(id);
        const auto previous = m_taskStates.constFind(id);
        if (previous != m_taskStates.constEnd() && previous.value() != current.status)
            showUndoableStatus(id, previous.value());
        if (!current.id.isEmpty()) m_taskStates.insert(id, current.status);
        else m_taskStates.remove(id);
    });
    connect(store, &TaskStore::desktopNoteChanged, this, [this]() { rebuildNavigation(); });
    connect(store, &TaskStore::groupsChanged, this, &TaskPanel::rebuildNavigation);
    connect(store, &TaskStore::tasksReset, this, [this]() {
        rebuildNavigation();
        m_taskStates.clear();
        for (const Entity& entity : m_store->entities()) m_taskStates.insert(entity.id, entity.status);
    });
    connect(store, &TaskStore::errorOccurred, this, &TaskPanel::showError);
    auto* clock = new QTimer(this); clock->setInterval(60000);
    connect(clock, &QTimer::timeout, this, [this, lastDate = QDate()]() mutable {
        const QDate today = toAppTime(appNow()).date();
        if (!lastDate.isValid() || lastDate != today) {
            lastDate = today;
            rebuildNavigation();
        }
        m_model->refresh();
    });
    clock->start();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        refreshWorkspaceIcons();
        rebuildNavigation();
        applyFilter();
    });
}

TaskPanel::~TaskPanel() {
    // QWidget 销毁子控件时还会发 focusChanged，须先停用依赖这些控件的回调。
    disconnect(qApp, nullptr, this, nullptr);
    delete m_relatedDetail;
    m_relatedDetail = nullptr;
}

void TaskPanel::buildUi() {
    auto* central = new WorkspaceSurface(this);
    m_surface = central;
    central->setObjectName(QStringLiteral("WorkspaceRoot"));
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(21, 21, 21, 21);
    root->setSpacing(0);

    // 错误只在发生时占位，不再使用常驻状态栏。
    auto* errorHost = new QWidget(central);
    auto* errorLayout = new QHBoxLayout(errorHost);
    errorLayout->setContentsMargins(72, 8, 12, 8);
    m_error = new QLabel(errorHost);
    m_error->setTextFormat(Qt::PlainText);
    m_error->setObjectName(QStringLiteral("ErrorBanner"));
    m_error->setWordWrap(true);
    m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLayout->addWidget(m_error);
    errorHost->hide();
    root->addWidget(errorHost);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    root->addLayout(body, 1);

    // 第一列：品牌 Logo 与其余三列首行共用中线，功能图标采用网页实测的 40/28px 规格。
    m_activityRail = new QFrame(central);
    m_activityRail->setObjectName(QStringLiteral("ActivityRail"));
    m_activityRail->setFixedWidth(52);
    auto* rail = new QVBoxLayout(m_activityRail);
    rail->setContentsMargins(6, WorkspaceGeometry::RailTopMargin, 6, 10);
    rail->setSpacing(WorkspaceGeometry::RailButtonSpacing);
    auto* account = new QToolButton(m_activityRail);
    account->setObjectName(QStringLiteral("AccountButton"));
    account->setFixedSize(WorkspaceGeometry::RailAccountSize,
                          WorkspaceGeometry::RailAccountSize);
    account->setIconSize(QSize(WorkspaceGeometry::RailAccountSize,
                               WorkspaceGeometry::RailAccountSize));
    account->setToolTip(QStringLiteral("本机工作区"));
    account->setAccessibleName(QStringLiteral("本机工作区"));
    account->setCursor(Qt::PointingHandCursor);
    account->installEventFilter(this);
    connect(account, &QToolButton::clicked, this, &TaskPanel::accountMenu);
    rail->addWidget(account, 0, Qt::AlignHCenter);
    rail->addSpacing(WorkspaceGeometry::RailAfterAccountExtraSpacing);
    const auto railButton = [this](QToolButton*& target, QWidget* parent, QVBoxLayout* layout,
                                   const QString& name, const QString& tooltip) {
        target = new QToolButton(parent);
        target->setObjectName(name);
        target->setFixedSize(40, 40);
        target->setIconSize(QSize(28, 28));
        target->setAutoRaise(true);
        target->setToolTip(tooltip);
        target->setAccessibleName(tooltip);
        target->setCursor(Qt::PointingHandCursor);
        layout->addWidget(target, 0, Qt::AlignHCenter);
    };
    railButton(m_railTasks, m_activityRail, rail, QStringLiteral("RailTasks"), QStringLiteral("任务"));
    m_railTasks->setCheckable(true);
    m_railTasks->setChecked(true);
    connect(m_railTasks, &QToolButton::clicked, this, [this]() {
        m_railTasks->setChecked(true);
        selectNavigation(ViewScope::AllLists);
        focusQuickAdd();
    });
    rail->addStretch(1);
    rail->setContentsMargins(6, WorkspaceGeometry::RailTopMargin, 6, 0);
    auto* railBottom = new QWidget(m_activityRail);
    railBottom->setObjectName(QStringLiteral("RailBottomGroup"));
    auto* bottomLayout = new QVBoxLayout(railBottom);
    bottomLayout->setContentsMargins(0, 0, 0, 10);
    bottomLayout->setSpacing(WorkspaceGeometry::RailButtonSpacing);
    rail->addWidget(railBottom);
    QToolButton* sync = nullptr;
    railButton(sync, railBottom, bottomLayout, QStringLiteral("RailSync"), QStringLiteral("同步与数据"));
    connect(sync, &QToolButton::clicked, this, &TaskPanel::dataMenu);
    railButton(m_railSettings, railBottom, bottomLayout, QStringLiteral("RailSettings"), QStringLiteral("设置"));
    connect(m_railSettings, &QToolButton::clicked, this, &TaskPanel::showSettings);
    body->addWidget(m_activityRail);

    // 第二列：筛选与组织。顶部没有独立标题条。
    m_sidebar = new QFrame(central);
    m_sidebar->setObjectName(QStringLiteral("Sidebar"));
    m_sidebar->setMinimumWidth(180);
    m_sidebar->setMaximumWidth(360);
    auto* side = new QVBoxLayout(m_sidebar);
    side->setContentsMargins(4, WorkspaceGeometry::NavigationTopMargin, 4, 8);
    side->setSpacing(2);

    m_navigation = new NavigationListWidget(m_sidebar);
    m_navigation->setObjectName(QStringLiteral("Navigation"));
    auto* navigationDelegate = new NavigationDelegate(m_navigation);
    m_navigation->setItemDelegate(navigationDelegate);
    m_navigation->setMouseTracking(true);
    m_navigation->viewport()->setAttribute(Qt::WA_Hover, true);
    m_navigation->setAccessibleName(QStringLiteral("任务导航"));
    m_navigation->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(navigationDelegate, &NavigationDelegate::folderToggleActivated, this,
            [this](const QString& folderId) {
        if (m_collapsedFolders.contains(folderId)) m_collapsedFolders.remove(folderId);
        else m_collapsedFolders.insert(folderId);
        rebuildNavigation();
    });
    connect(m_navigation, &NavigationListWidget::listReorderRequested, this,
            [this](const QString& sourceId, const QString& targetFolderId,
                   const QString& targetListId, bool after) {
        if (!flush()) return;
        if (!m_store->moveList(sourceId, targetFolderId, targetListId, after))
            showError(m_store->lastError());
    });
    connect(m_navigation, &NavigationListWidget::folderMergeRequested, this,
            [this](const QString& sourceId, const QString& targetListId) {
        FolderEditorDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted) return;
        if (!flush()) return;
        const Folder draft = dialog.draft();
        const QString folderId = m_store->createFolderFromLists(
            draft.name, draft.icon, sourceId, targetListId);
        if (folderId.isEmpty()) {
            showError(m_store->lastError());
            return;
        }
        selectNavigation(ViewScope::Folder, folderId);
    });
    side->addWidget(m_navigation, 1);

    // 第二至第四列共享一个分割器，两条边界均为普通 1px 分隔线。
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setObjectName(QStringLiteral("WorkspaceSplitter"));
    m_splitter->setHandleWidth(1);
    m_splitter->addWidget(m_sidebar);
    body->addWidget(m_splitter, 1);
    m_middle = new QFrame(m_splitter);
    m_middle->setObjectName(QStringLiteral("ListPane"));
    m_middle->setMinimumWidth(320);
    auto* center = new QVBoxLayout(m_middle);
    center->setContentsMargins(0, WorkspaceGeometry::ListHeaderTopMargin, 0, 0);
    center->setSpacing(0);

    auto* listHeader = new QFrame(m_middle);
    listHeader->setObjectName(QStringLiteral("ListHeader"));
    listHeader->setFixedHeight(WorkspaceGeometry::ListHeaderHeight);
    auto* header = new QHBoxLayout(listHeader);
    header->setContentsMargins(22, 0, 10, 0);
    header->setSpacing(4);
    auto* headingIcon = new QLabel(listHeader);
    headingIcon->setObjectName(QStringLiteral("ListHeadingIcon"));
    headingIcon->setFixedSize(WorkspaceGeometry::ListHeadingIconSize,
                              WorkspaceGeometry::ListHeadingIconSize);
    header->addWidget(headingIcon, 0, Qt::AlignVCenter);
    m_heading = new QLabel(QStringLiteral("全部待办"), listHeader);
    m_heading->setObjectName(QStringLiteral("ListHeading"));
    m_heading->setFont(ds::qfont(ds::FontScene::DetailTitle));
    header->addWidget(m_heading, 0, Qt::AlignVCenter);
    header->addStretch(1);
    m_workspaceMore = new QToolButton(listHeader);
    m_workspaceMore->setToolTip(QStringLiteral("视图选项"));
    m_workspaceMore->setAccessibleName(QStringLiteral("工作区视图选项"));
    m_workspaceMore->setObjectName(QStringLiteral("WorkspaceMore"));
    m_workspaceMore->setAutoRaise(true);
    m_workspaceMore->setFixedSize(32, 32);
    m_workspaceMore->setCursor(Qt::PointingHandCursor);
    connect(m_workspaceMore, &QToolButton::clicked, this, &TaskPanel::workspaceMenu);
    header->addWidget(m_workspaceMore);
    center->addWidget(listHeader);

    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 16, 0, 12);
    content->setSpacing(12);
    center->addLayout(content, 1);

    // 快速添加是一整条轻边框输入行，不再附加搜索框和独立添加按钮。
    auto* quickFrame = new QFrame(m_middle);
    m_quickFrame = quickFrame;
    quickFrame->setObjectName(QStringLiteral("QuickAddFrame"));
    quickFrame->setFixedHeight(40);
    auto* quick = new QHBoxLayout(quickFrame);
    quick->setContentsMargins(12, 0, 8, 0);
    quick->setSpacing(8);
    m_quickKind = new QLabel(quickFrame);
    m_quickKind->setFixedSize(20, 24);
    quick->addWidget(m_quickKind);
    m_quick = new QLineEdit(quickFrame);
    m_quick->setObjectName(QStringLiteral("QuickAdd"));
    m_quick->setMaxLength(500);
    m_quick->setFixedHeight(36);
    quick->addWidget(m_quick, 1);
    m_quickDate = new QToolButton(quickFrame);
    m_quickDate->setObjectName(QStringLiteral("QuickAddDate"));
    m_quickDate->setToolTip(QStringLiteral("设置新任务日期"));
    m_quickDate->setAccessibleName(QStringLiteral("设置新任务日期"));
    m_quickDate->setAutoRaise(true);
    m_quickDate->setCursor(Qt::PointingHandCursor);
    m_quickDate->setMinimumWidth(68);
    m_quickDate->setMaximumWidth(120);
    m_quickDate->setFixedHeight(30);
    m_quickDate->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_quickDate->hide();
    quick->addWidget(m_quickDate);
    connect(m_quick, &QLineEdit::returnPressed, this, &TaskPanel::createTask);
    auto* quickRow = new QHBoxLayout;
    quickRow->setContentsMargins(22, 0, 22, 0);
    quickRow->addWidget(quickFrame);
    content->addLayout(quickRow);

    m_model = new TaskListModel(m_store, this);
    m_list = new TaskListView(m_middle);
    m_list->setObjectName(QStringLiteral("TaskList"));
    m_list->setModel(m_model);
    auto* taskDelegate = new TaskDelegate(m_list);
    m_list->setItemDelegate(taskDelegate);
    m_dateController = new TaskDateController(m_store, this);
    connect(m_dateController, &TaskDateController::saveFailed, this, &TaskPanel::showError);
    connect(m_dateController, &TaskDateController::draftSaved, this,
            [this](const Entity& draft, const ScheduleValue& schedule) {
        m_quickDraft = draft;
        m_quickSchedule = schedule;
        applyScheduleToEntity(&m_quickDraft, schedule);
        m_quickDateCustomized = true;
        updateQuickDateButton();
    });
    connect(m_dateController, &TaskDateController::closed, this, [this]() {
        QTimer::singleShot(0, this, &TaskPanel::updateQuickDateVisibility);
    });
    connect(m_quickDate, &QToolButton::clicked, this, [this]() {
        m_dateController->openDraft(m_quickDraft, m_quickSchedule,
            QRect(m_quickDate->mapToGlobal(QPoint()), m_quickDate->size()));
        updateQuickDateVisibility();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        updateQuickDateVisibility();
    });
    connect(taskDelegate, &TaskDelegate::dateActivated, this,
            [this](const QString& id, const QRect& anchor) {
        if (!flush()) return;
        m_dateController->open(id, QRect(m_list->viewport()->mapToGlobal(anchor.topLeft()),
                                         anchor.size()));
    });
    connect(taskDelegate, &TaskDelegate::listActivated, this, [this](const QString& listId) {
        for (const TaskList& list : m_store->lists()) {
            if (list.id == listId && list.archivedAtUtcMs > 0) m_archivedExpanded = true;
        }
        rebuildNavigation();
        selectNavigation(ViewScope::List, listId);
    });
    connect(taskDelegate, &TaskDelegate::postponeActivated, this, [this](const QStringList& ids) {
        if (ids.isEmpty()) return;
        if (!ConfirmDialog::prompt(QStringLiteral("顺延至今天"),
                QStringLiteral("当前清单中所有已过期任务将被顺延至今天。"),
                QStringLiteral("顺延"), false, this)) return;
        if (m_store->postponeOverdueTasks(ids, toAppTime(appNow()).date()))
            showNotice(QStringLiteral("已顺延"));
    });
    connect(taskDelegate, &TaskDelegate::childrenToggleActivated, this,
            [this](const QString& id) { m_model->toggleChildren(id); });
    connect(taskDelegate, &TaskDelegate::sectionToggleActivated, this,
            [this](const QString& key) { m_model->toggleSection(key); });
    connect(m_list, &TaskListView::entityDropRequested, this,
            [this](const TaskDrop::Intent& intent) {
        if (!flush()) return;
        if (!m_store->applyDropIntent(intent)) {
            showError(m_store->lastError());
            return;
        }
        if (intent.placement == TaskDrop::Placement::AsChild)
            m_model->expandChildren(intent.targetParentId);
    });
    connect(m_list, &TaskListView::dragBlocked, this, &TaskPanel::showNotice);
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover, true);
    m_list->setUniformItemSizes(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(QStringLiteral("任务与笔记列表"));
    m_empty = new QLabel(QStringLiteral("这里还没有内容\n在上方输入一件要做的事或一条笔记。"), m_middle);
    m_empty->setProperty("muted", true);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setContentsMargins(22, 0, 22, 0);
    m_listStack = new QStackedWidget(m_middle);
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_empty);
    content->addWidget(m_listStack, 1);
    m_splitter->addWidget(m_middle);

    m_detailFrame = new QFrame(m_splitter);
    m_detailFrame->setObjectName(QStringLiteral("DetailFrame"));
    m_detailFrame->setMinimumWidth(300);
    auto* detailLayout = new QVBoxLayout(m_detailFrame);
    detailLayout->setContentsMargins(0, WorkspaceGeometry::DetailHeaderTopMargin, 0, 0);
    m_detail = new TaskDetailWidget(m_store, m_detailFrame);
    m_detail->setObjectName(QStringLiteral("TaskDetailWidget"));
    detailLayout->addWidget(m_detail);
    m_relatedDetail = new RelatedDetailPopup(m_store);
    connect(m_relatedDetail, &RelatedDetailPopup::internalLinkRequested, this, &TaskPanel::showTask);
    m_splitter->addWidget(m_detailFrame);
    m_splitter->setSizes({m_sidebarWidth, 560, m_detailWidth});
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setCollapsible(0, true);
    m_splitter->setCollapsible(2, true);
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        if (m_layoutUpdating || m_restoringLayout || m_focusMode) return;
        const QList<int> sizes = m_splitter->sizes();
        if (sizes.size() == 3) {
            if (sizes.at(0) >= 180) m_sidebarWidth = qMin(360, sizes.at(0));
            if (sizes.at(2) >= 300) m_detailWidth = sizes.at(2);
        }
        syncColumnResizeAreas();
    });

    // 窗口按钮悬浮在最右列顶部；第二列开关位于列表标题栏。
    m_sidebarToggle = new QToolButton(listHeader);
    m_sidebarToggle->setObjectName(QStringLiteral("SidebarToggle"));
    m_sidebarToggle->setFixedSize(WorkspaceGeometry::SidebarToggleSize,
                                  WorkspaceGeometry::SidebarToggleSize);
    m_sidebarToggle->setIconSize(QSize(WorkspaceGeometry::SidebarToggleIconSize,
                                      WorkspaceGeometry::SidebarToggleIconSize));
    m_sidebarToggle->setAutoRaise(true);
    m_sidebarToggle->setCursor(Qt::PointingHandCursor);
    m_sidebarToggle->setCheckable(true);
    m_sidebarToggle->setChecked(true);
    m_sidebarToggle->setToolTip(QStringLiteral("显示或隐藏筛选栏"));
    m_sidebarToggle->setAccessibleName(QStringLiteral("显示或隐藏筛选栏"));
    connect(m_sidebarToggle, &QToolButton::clicked, this, [this](bool checked) {
        setSidebarVisible(checked, true);
    });
    header->insertWidget(0, m_sidebarToggle);
    m_windowControls = new WindowControls(central);
    connect(m_windowControls, &WindowControls::windowOperationFinished, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            if (isMinimized()) return;
            syncWindowDecoration();
            reconcileWorkspaceLayout();
        });
    });
    m_windowControls->setAttribute(Qt::WA_StyledBackground, false);
    m_border = new WindowBorderOverlay(central);
    m_border->setGeometry(21, 21, central->width() - 42, central->height() - 42);
    const struct { const char* name; Qt::Edges edges; } resizeAreas[] = {
        {"WindowResizeLeft", Qt::LeftEdge}, {"WindowResizeRight", Qt::RightEdge},
        {"WindowResizeTop", Qt::TopEdge}, {"WindowResizeBottom", Qt::BottomEdge},
        {"WindowResizeTopLeft", Qt::LeftEdge | Qt::TopEdge},
        {"WindowResizeTopRight", Qt::RightEdge | Qt::TopEdge},
        {"WindowResizeBottomLeft", Qt::LeftEdge | Qt::BottomEdge},
        {"WindowResizeBottomRight", Qt::RightEdge | Qt::BottomEdge}
    };
    for (const auto& definition : resizeAreas)
        m_resizeAreas.append(new WindowResizeArea(this, definition.edges,
                                                  QString::fromLatin1(definition.name)));
    m_columnResizeAreas.append(new SplitterResizeArea(this, m_splitter, 1,
                                                       QStringLiteral("SidebarResizeArea")));
    m_columnResizeAreas.append(new SplitterResizeArea(this, m_splitter, 2,
                                                       QStringLiteral("DetailResizeArea")));
    m_detailOverlayShadow = new OverlayEdgeShadow(central);
    m_detailOverlayShadow->hide();
    m_overlayResizeArea = new OverlayResizeArea(this, [this](int delta) {
        if (!m_detailOverlay) return;
        const int inset = (isMaximized() || isFullScreen()) ? 0 : 21;
        const int contentWidth = m_surface->width() - 2 * inset;
        const int maximum = qMax(260, qMin(480, contentWidth - 260));
        m_overlayWidth = qBound(260, m_overlayWidth - delta, maximum);
        updateDetailOverlayGeometry();
    });
    m_overlayResizeArea->hide();
    raiseWindowChrome();
    m_surface->updateContentClip();
    connect(m_detail, &TaskDetailWidget::focusModeRequested, this,
            [this](bool enabled) {
        m_focusMode = enabled;
        if (enabled) {
            m_activityRail->hide();
            m_sidebar->hide();
            m_middle->hide();
            m_detailOverlay = false;
            m_detailFrame->setMinimumWidth(0);
            m_detailFrame->clearMask();
            m_splitter->insertWidget(2, m_detailFrame);
            m_detailFrame->show();
            m_detail->setOverlayMode(false);
            raiseWindowChrome();
        } else {
            m_activityRail->show();
            m_middle->show();
            m_detailFrame->setMinimumWidth(300);
            reconcileWorkspaceLayout();
        }
    });
    connect(m_detail, &TaskDetailWidget::taskRemoved, this, [this](const QString& id) {
        if (id != m_selectedId) return;
        m_selectedId.clear();
        setDetailVisible(false);
    });
    connect(m_detail, &TaskDetailWidget::closeRequested, this, [this]() {
        m_detail->setTask(QString());
        m_selectedId.clear();
        m_list->clearSelection();
        m_list->setCurrentIndex(QModelIndex());
        setDetailVisible(false);
    });
    m_toast = new ToastIsland(m_middle);
    m_toast->setObjectName(QStringLiteral("WorkspaceToast"));
    m_middle->installEventFilter(this);
    connect(m_toast, &ToastIsland::actionFinished, this, [this](bool succeeded) {
        if (!succeeded) showNotice(QStringLiteral("撤销失败，请稍后重试。"));
    });

    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { m_resetting = true; });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        const QModelIndex index = m_model->indexForTask(m_selectedId);
        if (index.isValid()) m_list->setCurrentIndex(index);
        m_listStack->setCurrentWidget(m_model->entityCount() == 0
            ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_list));
        m_resetting = false;
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) {
                if (m_resetting) return;
                const QString id = m_model->entityId(current);
                if (m_detail->setTask(id)) {
                    m_selectedId = id;
                    setDetailVisible(!id.isEmpty());
                } else {
                    const QSignalBlocker blocker(m_list->selectionModel());
                    m_list->setCurrentIndex(m_model->indexForTask(m_selectedId));
                }
            });
    connect(m_list, &QListView::customContextMenuRequested, this, &TaskPanel::showTaskMenu);
    connect(m_navigation, &QListWidget::customContextMenuRequested, this, &TaskPanel::organizationMenu);
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_rebuilding || row < 0) return;
        if (!m_detail->flush()) { rebuildNavigation(); return; }
        auto* item = m_navigation->item(row);
        if (!item || item->data(NavigationDelegate::SectionRole).toBool()
            || item->data(NavigationDelegate::DividerRole).toBool()) return;
        m_keepHiddenView = false;
        m_filter.scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
        m_filter.listId = m_filter.scope == ViewScope::List
            ? item->data(Qt::UserRole + 1).toString() : QString();
        m_filter.folderId = m_filter.scope == ViewScope::Folder
            ? item->data(NavigationDelegate::FolderIdRole).toString() : QString();
        m_heading->setText(item->data(Qt::UserRole + 2).toString().trimmed());
        applyFilter();
    });

    auto* newShortcut = new QShortcut(QKeySequence::New, this);
    connect(newShortcut, &QShortcut::activated, this, [this]() {
        focusQuickAdd();
    });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, &TaskPanel::flush);
    auto* closeDetail = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closeDetail->setContext(Qt::WindowShortcut);
    connect(closeDetail, &QShortcut::activated, this, [this]() {
        if (m_detail->entityId().isEmpty() || !m_detail->flush()) return;
        m_detail->setTask(QString());
        m_selectedId.clear();
        m_list->clearSelection();
        m_list->setCurrentIndex(QModelIndex());
        setDetailVisible(false);
    });

    refreshWorkspaceIcons();
    reconcileWorkspaceLayout();
    applyFilter();
}

// ========== R3: 导航重建 ==========
void TaskPanel::rebuildNavigation() {
    if (m_restoreFrozen) return;
    m_rebuilding = true;
    const QSignalBlocker blocker(m_navigation);
    // item widget 不随 QListWidgetItem 自动复位所有权；先解除再销毁，避免隐藏标题在每次重建后累积。
    for (int row = 0; row < m_navigation->count(); ++row) {
        QListWidgetItem* item = m_navigation->item(row);
        if (QWidget* widget = m_navigation->itemWidget(item)) {
            m_navigation->removeItemWidget(item);
            widget->deleteLater();
        }
    }
    m_navigation->clear();
    QListWidgetItem* selected = nullptr;
    const QVector<TaskList> lists = m_store->lists();
    const QVector<Folder> folders = m_store->folders();
    if (m_filter.scope == ViewScope::List) {
        for (const TaskList& list : lists)
            if (list.id == m_filter.listId && !list.folderId.isEmpty())
                m_collapsedFolders.remove(list.folderId);
    }

    QSet<QString> archivedIds;
    for (const TaskList& list : lists)
        if (list.archivedAtUtcMs > 0) archivedIds.insert(list.id);
    if (m_filter.scope == ViewScope::List && archivedIds.contains(m_filter.listId))
        m_archivedExpanded = true;
    const bool archivePolicyChanged = archivedIds != m_archivedListIds;
    m_archivedListIds = archivedIds;

    auto divider = [this]() {
        auto* item = new QListWidgetItem(m_navigation);
        item->setFlags(Qt::NoItemFlags);
        item->setData(NavigationDelegate::DividerRole, true);
        item->setData(Qt::UserRole, -1); // 分隔线不是“所有”（AllLists 的枚举值为 0）。
        item->setData(Qt::DisplayRole, QString());
        item->setSizeHint(QSize(160, 33));
        return item;
    };
    auto interactiveSection = [this](const QString& title, const QString& objectName,
                                     IconId actionIcon, const QString& actionName,
                                     const QString& toolTip, bool collapseAction,
                                     std::function<void()> action) {
        auto* item = new QListWidgetItem(m_navigation);
        item->setFlags(Qt::NoItemFlags);
        item->setData(NavigationDelegate::SectionRole, true);
        item->setData(Qt::UserRole + 2, title);
        item->setSizeHint(QSize(160, 36));
        auto* header = new SidebarSectionHeader(title, objectName, m_navigation);
        header->setAction(actionIcon, toolTip, actionName, collapseAction);
        header->setExpanded(objectName == QLatin1String("ArchiveSectionHeader")
                            ? m_archivedExpanded : !m_listSectionCollapsed);
        m_navigation->setItemWidget(item, header);
        connect(header, &SidebarSectionHeader::actionTriggered, this, action);
        return header;
    };
    auto add = [this, &selected, &archivedIds](const QString& label, ViewScope scope,
                                               const QString& listId = QString(),
                                               const TaskList* appearance = nullptr,
                                               const QString& smartKey = QString(),
                                               int indent = 0) -> QListWidgetItem* {
        ViewSpec probe;
        probe.scope = scope;
        probe.listId = listId;
        int count = 0;
        for (const Entity& entity : m_store->entities())
            if (contains(entity, probe, toAppTime(appNow()).date(), archivedIds)) ++count;
        if (!smartKey.isEmpty()) {
            const auto visibility = Settings::instance().smartListVisibility(smartKey);
            if (visibility == Settings::SmartListVisibility::Hide
                || (visibility == Settings::SmartListVisibility::WhenNonEmpty && count == 0))
                return nullptr;
        }
        auto* item = new QListWidgetItem(label, m_navigation);
        item->setData(Qt::UserRole, static_cast<int>(scope));
        item->setData(Qt::UserRole + 1, listId);
        item->setData(Qt::UserRole + 2, label);
        item->setData(NavigationDelegate::CountRole,
                      count > 0 ? QString::number(count) : QString());
        item->setData(NavigationDelegate::IconRole, static_cast<int>(scopeIcon(scope)));
        item->setData(NavigationDelegate::IndentRole, indent);
        item->setData(NavigationDelegate::SmartRowRole,
                      !smartKey.isEmpty() || (appearance && appearance->inbox));
        if (!smartKey.isEmpty() || (appearance && appearance->inbox))
            item->setSizeHint(QSize(160, WorkspaceGeometry::NavigationRowHeight));
        if (appearance) {
            item->setData(NavigationDelegate::IconNameRole, ListAppearance::iconName(*appearance));
            item->setData(NavigationDelegate::ListColorRole, appearance->color);
            item->setData(NavigationDelegate::ListKindRole, static_cast<int>(appearance->kind));
            item->setData(NavigationDelegate::ParentFolderRole,
                          appearance->archivedAtUtcMs > 0 ? QStringLiteral("archive")
                                                         : appearance->folderId);
            if (!appearance->inbox && appearance->archivedAtUtcMs == 0)
                item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
        }
        if (scope == m_filter.scope && (scope != ViewScope::List || listId == m_filter.listId))
            selected = item;
        return item;
    };
    auto folder = [this, &selected, &lists, &archivedIds](const Folder& row) {
        ViewSpec probe;
        probe.scope = ViewScope::Folder;
        probe.folderId = row.id;
        const QSet<QString> members = folderListIds(lists, row.id);
        int count = 0;
        for (const Entity& entity : m_store->entities())
            if (contains(entity, probe, toAppTime(appNow()).date(), archivedIds, members)) ++count;
        auto* item = new QListWidgetItem(row.name, m_navigation);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setData(Qt::UserRole, static_cast<int>(ViewScope::Folder));
        item->setData(Qt::UserRole + 1, QString());
        item->setData(Qt::UserRole + 2, row.name);
        item->setData(NavigationDelegate::FolderIdRole, row.id);
        item->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::Folder));
        if (!row.icon.isEmpty()) item->setData(NavigationDelegate::IconNameRole, row.icon);
        item->setData(NavigationDelegate::CountRole,
                      count > 0 ? QString::number(count) : QString());
        item->setData(NavigationDelegate::FolderExpandedRole,
                      !m_collapsedFolders.contains(row.id));
        item->setToolTip(QStringLiteral("文件夹：%1（右键管理）").arg(row.name));
        if (m_filter.scope == ViewScope::Folder && m_filter.folderId == row.id) selected = item;
    };

    add(QStringLiteral("所有"), ViewScope::AllLists, QString(), nullptr, QStringLiteral("all"));
    add(QStringLiteral("今天"), ViewScope::Today, QString(), nullptr, QStringLiteral("today"));
    add(QStringLiteral("明天"), ViewScope::Tomorrow, QString(), nullptr, QStringLiteral("tomorrow"));
    add(QStringLiteral("最近7天"), ViewScope::Next7Days, QString(), nullptr,
        QStringLiteral("next7days"));
    for (const TaskList& list : m_store->lists())
        if (list.inbox)
            add(QStringLiteral("收集箱"), ViewScope::List, list.id, &list,
                QStringLiteral("inbox"));

    divider();
    auto* listSection = interactiveSection(QStringLiteral("清单"), QStringLiteral("ListSectionHeader"),
                       IconId::Add, QStringLiteral("GroupMenuButton"),
                       QStringLiteral("添加清单"), false,
                       [this]() { createListDialog(QString(), EntityKind::Task); });
    listSection->setDisclosureEnabled(true);
    connect(listSection, &SidebarSectionHeader::disclosureToggled, this, [this]() {
        m_listSectionCollapsed = !m_listSectionCollapsed;
        QTimer::singleShot(0, this, [this]() { rebuildNavigation(); });
    });
    struct NavigationContainer {
        const TaskList* list = nullptr;
        const Folder* folder = nullptr;
        int sortOrder = 0;
        QString id;
    };
    QVector<NavigationContainer> containers;
    for (const TaskList& list : lists) {
        if (list.inbox || list.archivedAtUtcMs > 0 || !list.folderId.isEmpty()) continue;
        containers.append({&list, nullptr, list.sortOrder, list.id});
    }
    for (const Folder& row : folders)
        containers.append({nullptr, &row, row.sortOrder, row.id});
    std::sort(containers.begin(), containers.end(), [](const NavigationContainer& a,
                                                       const NavigationContainer& b) {
        if (a.sortOrder != b.sortOrder) return a.sortOrder < b.sortOrder;
        // 旧全零数据保持改造前“平铺清单在前、文件夹在后”的展示顺序。
        if (!!a.list != !!b.list) return a.list != nullptr;
        return a.id < b.id;
    });
    for (const NavigationContainer& container : containers) {
        if (container.list) {
            auto* item = add(container.list->name, ViewScope::List, container.list->id,
                             container.list);
            if (item) item->setHidden(m_listSectionCollapsed);
            continue;
        }
        folder(*container.folder);
        for (int row = m_navigation->count() - 1; row >= 0; --row) {
            if (m_navigation->item(row)->data(NavigationDelegate::FolderIdRole).toString()
                == container.folder->id) {
                m_navigation->item(row)->setHidden(m_listSectionCollapsed);
                break;
            }
        }
        for (const TaskList& list : lists) {
            if (list.inbox || list.archivedAtUtcMs > 0 || list.folderId != container.folder->id) continue;
            auto* item = add(list.name, ViewScope::List, list.id, &list, QString(),
                             NavigationGeometry::FolderChildIndent);
            if (item) item->setHidden(m_listSectionCollapsed
                                      || m_collapsedFolders.contains(container.folder->id));
        }
    }

    bool hasArchived = false;
    for (const TaskList& list : m_store->lists())
        if (!list.inbox && list.archivedAtUtcMs > 0) hasArchived = true;
    if (hasArchived) {
        auto* archiveItem = new QListWidgetItem(m_navigation);
        archiveItem->setFlags(Qt::NoItemFlags);
        archiveItem->setData(NavigationDelegate::SectionRole, true);
        archiveItem->setData(Qt::UserRole, -1);
        archiveItem->setData(Qt::DisplayRole, QString());
        archiveItem->setSizeHint(QSize(160, 36));
        auto* archiveHeader = new SidebarSectionHeader(
            QStringLiteral("已归档的清单"), QStringLiteral("ArchiveSectionHeader"), m_navigation);
        archiveItem->setData(Qt::UserRole + 2, archiveHeader->objectName());
        m_navigation->setItemWidget(archiveItem, archiveHeader);
        archiveHeader->setFolderPresentation(true);
        archiveHeader->setLeadingIcon(IconId::ArchivedLists);
        archiveHeader->setDisclosureEnabled(true);
        archiveHeader->setExpanded(m_archivedExpanded);
        connect(archiveHeader, &SidebarSectionHeader::disclosureToggled, this, [this]() {
            m_archivedExpanded = !m_archivedExpanded;
            QTimer::singleShot(0, this, [this]() { rebuildNavigation(); });
        });
        archiveItem->setHidden(m_listSectionCollapsed);
        for (const TaskList& list : m_store->lists()) {
            if (list.inbox || list.archivedAtUtcMs == 0) continue;
            if (auto* item = add(list.name, ViewScope::List, list.id, &list, QString(),
                                 NavigationGeometry::FolderChildIndent))
                item->setHidden(m_listSectionCollapsed || !m_archivedExpanded);
        }
    }

    divider();
    divider()->setData(NavigationDelegate::OrganizationEndRole, true);
    add(QStringLiteral("已完成"), ViewScope::Completed, QString(), nullptr,
        QStringLiteral("completed"));
    add(QStringLiteral("已放弃"), ViewScope::Abandoned, QString(), nullptr,
        QStringLiteral("abandoned"));
    add(QStringLiteral("垃圾桶"), ViewScope::Trash, QString(), nullptr,
        QStringLiteral("trash"));

    // 智能入口可被隐藏；只保留两个可见分组之间的分隔线。
    for (int row = m_navigation->count() - 1; row >= 0; --row) {
        const auto* item = m_navigation->item(row);
        if (!item->data(NavigationDelegate::DividerRole).toBool()) continue;
        const bool redundant = row == 0 || row == m_navigation->count() - 1
            || m_navigation->item(row - 1)->data(NavigationDelegate::DividerRole).toBool();
        if (redundant) delete m_navigation->takeItem(row);
    }

    bool resetFilter = archivePolicyChanged;
    if (!selected && !m_keepHiddenView) {
        for (int row = 0; row < m_navigation->count(); ++row) {
            auto* item = m_navigation->item(row);
            if (item->isHidden() || item->data(NavigationDelegate::SectionRole).toBool()
                || item->data(NavigationDelegate::DividerRole).toBool()) continue;
            selected = item;
            m_filter.scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
            m_filter.listId = m_filter.scope == ViewScope::List
                ? item->data(Qt::UserRole + 1).toString() : QString();
            m_filter.folderId = m_filter.scope == ViewScope::Folder
                ? item->data(NavigationDelegate::FolderIdRole).toString() : QString();
            m_filter.renderer = ViewRenderer::List;
            m_heading->setText(item->data(Qt::UserRole + 2).toString());
            resetFilter = true;
            break;
        }
    }
    if (!selected && !m_keepHiddenView) {
        m_filter.scope = ViewScope::AllLists;
        m_filter.listId.clear();
        m_filter.folderId.clear();
        resetFilter = true;
    }
    if (selected) m_keepHiddenView = false;
    m_navigation->setCurrentRow(selected ? m_navigation->row(selected) : -1);
    m_rebuilding = false;
    if (resetFilter) applyFilter();
    updateQuickAddKind();
    updateHeadingAppearance();
    m_listStack->setCurrentWidget(m_model->entityCount() == 0
        ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_list));
}

void TaskPanel::syncGroupingPreference() {
    const QString identity = m_filter.scope == ViewScope::List ? m_filter.listId
        : m_filter.scope == ViewScope::Folder ? m_filter.folderId : QStringLiteral("all");
    const QString key = QStringLiteral("grouping/%1/%2")
        .arg(static_cast<int>(m_filter.scope)).arg(identity);
    if (m_groupingPreferenceKey == key) return;
    m_groupingPreferenceKey = key;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    const GroupMode fallback = defaultGrouping(m_filter.scope);
    m_filter.grouping = groupModeFromName(settings.value(key).toString(), fallback);
    if (m_filter.scope == ViewScope::List && m_filter.grouping == GroupMode::List)
        m_filter.grouping = GroupMode::Time;
}

void TaskPanel::setGrouping(GroupMode mode) {
    if (m_filter.scope == ViewScope::List && mode == GroupMode::List) return;
    syncGroupingPreference();
    if (m_filter.grouping == mode) return;
    m_filter.grouping = mode;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    settings.setValue(m_groupingPreferenceKey, groupModeName(mode));
    applyFilter();
}

void TaskPanel::applyFilter() {
    if (m_restoreFrozen) return;
    // 全局搜索暂未开放；保留 ViewSpec 筛选字段供后续完整搜索功能使用。
    m_filter.search.clear();
    syncGroupingPreference();
    updateHeadingAppearance();
    updateQuickAddKind();
    m_model->setFilter(m_filter);
}

void TaskPanel::updateHeadingAppearance() {
    QString heading = scopeLabel(m_filter.scope);
    TaskList selectedList;
    Folder selectedFolder;
    if (m_filter.scope == ViewScope::List) {
        for (const TaskList& list : m_store->lists()) {
            if (list.id != m_filter.listId) continue;
            selectedList = list;
            heading = list.name;
        }
    } else if (m_filter.scope == ViewScope::Folder) {
        for (const Folder& folder : m_store->folders()) {
            if (folder.id != m_filter.folderId) continue;
            selectedFolder = folder;
            heading = folder.name;
        }
    }
    m_heading->setText(heading);
    if (auto* icon = findChild<QLabel*>(QStringLiteral("ListHeadingIcon"))) {
        const int size = WorkspaceGeometry::ListHeadingIconSize;
        QIcon value;
        if (m_filter.scope == ViewScope::List) {
            value = ListAppearance::icon(selectedList, size,
                                         ThemeManager::instance().color(ds::ColorToken::Text));
        } else if (m_filter.scope == ViewScope::Folder && !selectedFolder.icon.isEmpty()) {
            TaskList appearance;
            appearance.icon = selectedFolder.icon;
            value = ListAppearance::icon(appearance, size,
                                         ThemeManager::instance().color(ds::ColorToken::Text));
        } else {
            value = AppIcons::icon(scopeIcon(m_filter.scope), ds::ColorToken::Text, size);
        }
        icon->setPixmap(value.pixmap(size, size));
    }
}

void TaskPanel::updateQuickAddKind() {
    TaskList target;
    if (m_filter.scope == ViewScope::List) {
        for (const TaskList& list : m_store->lists())
            if (list.id == m_filter.listId) target = list;
    } else if (m_filter.scope == ViewScope::Folder) {
        for (const TaskList& list : m_store->lists())
            if (list.folderId == m_filter.folderId && list.archivedAtUtcMs == 0) {
                target = list;
                break;
            }
    } else {
        for (const TaskList& list : m_store->lists())
            if (list.inbox) target = list;
    }
    const EntityKind kind = target.id.isEmpty() ? EntityKind::Task : target.kind;
    const QString prefix = target.icon.isEmpty() ? QString() : target.icon + QLatin1Char(' ');
    const QString targetName = target.name.isEmpty() ? QString() : prefix + target.name;
    const QString action = kind == EntityKind::Note ? QStringLiteral("添加笔记") : QStringLiteral("添加任务");
    const QString placeholder = targetName.isEmpty() ? action
        : QStringLiteral("%1至“%2”").arg(action, targetName);
    m_quickKind->setPixmap(AppIcons::icon(IconId::Add, ds::ColorToken::TextTertiary, 18).pixmap(18, 18));
    m_quick->setPlaceholderText(placeholder);
    m_quick->setAccessibleName(placeholder);
    m_quick->setEnabled(m_filter.scope != ViewScope::Folder || !target.id.isEmpty());
}

void TaskPanel::updateQuickDateButton() {
    if (!m_quickDate) return;
    const QDate today = toAppTime(appNow()).date();
    QDate date;
    QString time;
    if (m_quickDateCustomized) {
        if (m_quickDraft.hasScheduleRange ? m_quickDraft.startAt.isValid() : m_quickDraft.dueAt.isValid()) {
            const QDateTime wall = (m_quickDraft.hasScheduleRange ? m_quickDraft.startAt : m_quickDraft.dueAt)
                .toTimeZone(QTimeZone(m_quickDraft.timeZone.toUtf8()));
            date = wall.date();
            time = wall.toString(QStringLiteral("HH:mm"));
        } else {
            date = m_quickDraft.allDayDate;
        }
    }
    QString label = m_filter.scope == ViewScope::Tomorrow ? QStringLiteral("明天")
                                                          : QStringLiteral("今天");
    if (m_quickDateCustomized) {
        if (!date.isValid()) label = QStringLiteral("无日期");
        else if (date == today) label = QStringLiteral("今天");
        else if (date == today.addDays(1)) label = QStringLiteral("明天");
        else label = date.toString(QStringLiteral("M月d日"));
        if (!time.isEmpty()) label += QLatin1Char(' ') + time;
    }
    m_quickDate->setText(label);
    m_quickDate->setAccessibleName(QStringLiteral("新任务日期：") + label);
    m_quickDate->setToolTip(m_quickDateCustomized ? scheduleLabel(m_quickDraft)
                                                  : QStringLiteral("设置新任务日期"));
    m_quickDate->setProperty("hasDate", date.isValid());
    m_quickDate->style()->unpolish(m_quickDate);
    m_quickDate->style()->polish(m_quickDate);
}

void TaskPanel::updateQuickDateVisibility() {
    if (!m_quickDate || !m_quickFrame || !m_quick) return;
    const QWidget* focus = QApplication::focusWidget();
    const bool inside = focus && m_quickFrame->isAncestorOf(focus);
    const bool popupOpen = m_dateController && m_dateController->isOpen();
    m_quickDate->setVisible(m_quick->isVisible() && m_quick->isEnabled() && (inside || popupOpen));
}

void TaskPanel::createTask() {
    if (m_restoreFrozen) return;
    const QString title = m_quick->text().trimmed();
    if (title.isEmpty()) { m_quick->setFocus(); return; }
    if (!m_detail->flush()) return;
    QString id;
    QString targetListId = m_filter.listId;
    if (m_filter.scope == ViewScope::Folder) {
        targetListId.clear();
        for (const TaskList& list : m_store->lists())
            if (list.folderId == m_filter.folderId && list.archivedAtUtcMs == 0) {
                targetListId = list.id;
                break;
            }
        if (targetListId.isEmpty()) {
            showNotice(QStringLiteral("请先在文件夹中创建清单。"));
            return;
        }
    }
    EntityKind kind = EntityKind::Task;
    for (const TaskList& list : m_store->lists()) if (list.id == targetListId) kind = list.kind;
    Entity draft = m_quickDateCustomized ? m_quickDraft : Entity();
    draft.id.clear();
    draft.kind = kind;
    draft.title = title;
    draft.listId = targetListId;
    ScheduleValue schedule;
    bool hasSchedule = false;
    if (m_quickDateCustomized) {
        schedule = m_quickSchedule;
        hasSchedule = true;
    } else if (kind == EntityKind::Task) {
        if (m_filter.scope == ViewScope::Today || m_filter.scope == ViewScope::Next7Days
            || m_filter.scope == ViewScope::Overdue)
            schedule.allDayDate = toAppTime(appNow()).date();
        else if (m_filter.scope == ViewScope::Tomorrow)
            schedule.allDayDate = toAppTime(appNow()).date().addDays(1);
        hasSchedule = schedule.allDayDate.isValid();
    }
    if (hasSchedule) {
        if (!m_store->saveSchedule(draft, schedule, &id)) {
            showError(m_store->lastError());
            return;
        }
    } else if (m_filter.scope == ViewScope::List || m_filter.scope == ViewScope::Folder) {
        id = kind == EntityKind::Note ? m_store->createNote(title, targetListId)
                                      : m_store->createTask(title, targetListId);
    } else id = m_store->createTask(title);
    if (id.isEmpty()) return;
    m_quickDraft = Entity();
    m_quickSchedule = ScheduleValue();
    m_quickDateCustomized = false;
    updateQuickDateButton();
    m_quick->clear(); showTask(id); m_detail->focusTitle();
    updateQuickDateVisibility();
}

void TaskPanel::showTask(QString id) {
    if (m_restoreFrozen) return;
    if (!m_detail->flush()) { showNormal(); raise(); activateWindow(); return; }
    if (!id.isEmpty()) {
        const Entity entity = m_store->entity(id); if (entity.id.isEmpty()) return;
        const QSet<QString> members = m_filter.scope == ViewScope::Folder
            ? folderListIds(m_store->lists(), m_filter.folderId) : QSet<QString>();
        if (!contains(entity, m_filter, toAppTime(appNow()).date(), m_archivedListIds, members)) {
            if (entity.deleted) m_filter.scope = ViewScope::Trash;
            else if (entity.kind == EntityKind::Task && entity.status == TaskStatus::Completed)
                m_filter.scope = ViewScope::Completed;
            else if (entity.kind == EntityKind::Task && entity.status == TaskStatus::Abandoned)
                m_filter.scope = ViewScope::Abandoned;
            else {
                TaskList owner;
                for (const TaskList& list : m_store->lists())
                    if (list.id == entity.listId) owner = list;
                if (owner.archivedAtUtcMs > 0) {
                    m_filter.scope = ViewScope::List;
                    m_filter.listId = owner.id;
                    m_archivedExpanded = true;
                } else {
                    m_filter.scope = ViewScope::AllLists;
                    m_filter.listId.clear();
                }
            }
            if (m_filter.scope != ViewScope::List) m_filter.listId.clear();
            if (m_filter.scope != ViewScope::Folder) m_filter.folderId.clear();
            m_filter.renderer = ViewRenderer::List;
            m_filter.search.clear();
            QString heading = scopeLabel(m_filter.scope);
            if (m_filter.scope == ViewScope::List) {
                for (const TaskList& list : m_store->lists())
                    if (list.id == m_filter.listId) heading = list.name;
            }
            m_heading->setText(heading);
            m_keepHiddenView = true;
            applyFilter(); rebuildNavigation();
        }
        m_selectedId = id; const auto index = m_model->indexForTask(id);
        m_list->setCurrentIndex(index); m_list->scrollTo(index); m_detail->setTask(id);
        setDetailVisible(true);
    }
    showNormal(); raise(); activateWindow();
    syncWindowDecoration();
    raiseWindowChrome();
}

void TaskPanel::openRelatedDetail(QString id) {
    if (m_restoreFrozen || id.isEmpty() || m_store->entity(id).id.isEmpty()) return;
    if (isMinimized()) showNormal();
    else if (!isVisible()) show();
    syncWindowDecoration();
    m_relatedDetail->setWindowIcon(windowIcon());
    m_relatedDetail->setAnchorBounds(geometry());
    if (m_relatedDetail->openEntity(id)) {
        m_relatedDetail->raise();
        m_relatedDetail->activateWindow();
    }
}

void TaskPanel::focusQuickAdd() {
    showTask();
    m_quick->setFocus();
}

void TaskPanel::setAlwaysOnTop(bool enabled) {
    if (m_alwaysOnTop == enabled) return;
    m_alwaysOnTop = enabled;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("kylinlist"), QStringLiteral("panel"));
    settings.setValue(QStringLiteral("alwaysOnTop"), enabled);
    settings.sync();
    const bool wasVisible = isVisible();
    const Qt::WindowStates state = windowState() & ~Qt::WindowActive;
    const QRect normal = isMaximized() ? normalGeometry() : geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (normal.isValid()) setGeometry(normal);
    setWindowState(state);
    if (wasVisible) show();
    syncWindowDecoration();
}

// 更新退出前的草稿保护：只检查 flush() 链路覆盖不到的未提交输入。
bool TaskPanel::hasUncommittedDrafts(QString* what) const {
    if (m_quick && !m_quick->text().trimmed().isEmpty()) {
        if (what) *what = QStringLiteral("快速添加框中有未提交内容");
        return true;
    }
    return false;
}

void TaskPanel::showUpdateNotice(const QString& message) { showNotice(message); }
bool TaskPanel::flush() {
    return m_restoreFrozen || (m_relatedDetail->finishEditing() && m_detail->finishEditing());
}
void TaskPanel::setRestoreFrozen(bool frozen) {
    m_restoreFrozen = frozen;
    setEnabled(!frozen);
    if (m_relatedDetail) m_relatedDetail->setEnabled(!frozen);
}
void TaskPanel::resetAfterRestore() {
    m_relatedDetail->clearAfterRestore();
    setRestoreFrozen(false); m_selectedId.clear(); m_filter = ViewSpec();
    m_filter.search.clear();
    { const QSignalBlocker blocker(m_quick); m_quick->clear(); }
    m_quickDraft = Entity();
    m_quickSchedule = ScheduleValue();
    m_quickDateCustomized = false;
    updateQuickDateButton();
    m_groupingPreferenceKey.clear();
    m_detail->setTask(QString()); m_heading->setText(scopeLabel(ViewScope::AllLists));
    rebuildNavigation(); applyFilter();
}
void TaskPanel::showError(const QString& message) {
    m_error->setText(message); m_error->show();
    if (QWidget* host = m_error->parentWidget()) host->show();
}

void TaskPanel::showNotice(const QString& message) {
    m_toast->showMessage(message);
    m_toast->move((m_middle->width() - m_toast->width()) / 2,
                  m_middle->height() - m_toast->height() - 28);
}

void TaskPanel::showUndoableStatus(const QString& id, TaskStatus previous) {
    const Entity before = m_store->entity(id);
    if (before.id.isEmpty() || before.kind != EntityKind::Task) return;
    const TaskStatus current = before.status;
    if (current == previous || (current != TaskStatus::Completed && current != TaskStatus::Abandoned)) return;
    const QString message = current == TaskStatus::Completed ? QStringLiteral("任务已完成")
                                                             : QStringLiteral("任务已放弃");
    m_toast->showAction(message, QStringLiteral("撤销"), [this, id, previous]() {
        return m_store->setStatus(id, previous);
    });
    m_toast->move((m_middle->width() - m_toast->width()) / 2,
                  m_middle->height() - m_toast->height() - 28);
}

bool TaskPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_middle && event->type() == QEvent::Resize && m_toast) {
        m_toast->move((m_middle->width() - m_toast->width()) / 2,
                      m_middle->height() - m_toast->height() - 28);
    }
    return QMainWindow::eventFilter(watched, event);
}

void TaskPanel::setDetailVisible(bool visible) {
    if (!m_detailFrame) return;
    // 宽窗口的第四列始终存在，无选择时只是空白；只有窄窗覆盖详情可关闭。
    if (m_detailAutoHidden && !m_focusMode) {
        m_detailOverlay = visible && !m_selectedId.isEmpty();
        if (m_detailOverlay && m_detailFrame->parentWidget() != m_surface) {
            m_detailFrame->setParent(m_surface);
            m_detailFrame->setMaximumWidth(QWIDGETSIZE_MAX);
            m_detailFrame->setProperty("overlay", true);
            m_detail->setOverlayMode(true);
            m_detailFrame->style()->unpolish(m_detailFrame);
            m_detailFrame->style()->polish(m_detailFrame);
        }
        updateDetailOverlayGeometry();
        m_detailFrame->setVisible(m_detailOverlay);
    } else if (!m_focusMode) {
        m_detailOverlay = false;
        if (m_detailFrame->parentWidget() != m_splitter || m_splitter->indexOf(m_detailFrame) != 2) {
            m_detailFrame->clearMask();
            m_splitter->insertWidget(2, m_detailFrame);
            m_detailFrame->setMinimumWidth(m_preferDetailOverlay ? 300 : 260);
            m_detailFrame->setProperty("overlay", false);
            m_detail->setOverlayMode(false);
            m_detailFrame->style()->unpolish(m_detailFrame);
            m_detailFrame->style()->polish(m_detailFrame);
        }
        m_detailFrame->show();
    }
    updateWindowControlsPosition();
    raiseWindowChrome();
    m_surface->updateContentClip();
}

void TaskPanel::setSidebarVisible(bool visible, bool userInitiated) {
    if (userInitiated) m_sidebarUserHidden = !visible;
    reconcileWorkspaceLayout();
}

void TaskPanel::reconcileWorkspaceLayout() {
    if (!m_splitter || m_layoutUpdating || m_focusMode) return;
    if (!isVisible()) {
        m_detailAutoHidden = false;
        m_sidebarAutoHidden = false;
        m_sidebar->setVisible(!m_sidebarUserHidden);
        m_detailFrame->setVisible(true);
        m_splitter->setSizes({m_sidebarWidth, 560, m_detailWidth});
        return;
    }
    m_layoutUpdating = true;
    // “侧边栏”模式在窄窗缩小双栏，保留停靠详情；“弹窗”沿用现有覆盖详情。
    const int middleMinimum = m_preferDetailOverlay ? 320 : 260;
    const int detailMinimum = m_preferDetailOverlay ? 300 : 260;
    m_middle->setMinimumWidth(middleMinimum);
    const int available = m_splitter->width();
    const bool oldDetailAutoHidden = m_detailAutoHidden;
    const bool oldSidebarAutoHidden = m_sidebarAutoHidden;
    // 先收第四列，再收第二列；拉宽时按相反顺序恢复。
    const int detailThreshold = m_preferDetailOverlay
        ? (m_sidebarUserHidden ? 621 : 820) : 0;
    const int sidebarThreshold = 620;
    m_detailAutoHidden = available < detailThreshold;
    m_sidebarAutoHidden = available < sidebarThreshold;
    const bool showSidebar = !m_sidebarUserHidden && !m_sidebarAutoHidden;
    const bool showDockedDetail = !m_detailAutoHidden;
    const bool wantOverlay = m_detailAutoHidden && !m_selectedId.isEmpty();

    if (oldDetailAutoHidden != m_detailAutoHidden || oldSidebarAutoHidden != m_sidebarAutoHidden
        || m_detailOverlay != wantOverlay
        || (!wantOverlay && m_splitter->indexOf(m_detailFrame) != 2)
        || m_detailFrame->parentWidget() != (wantOverlay ? static_cast<QWidget*>(m_surface)
                                                        : static_cast<QWidget*>(m_splitter))) {
        if (wantOverlay) {
            m_detailFrame->setParent(m_surface);
            m_detailFrame->setMaximumWidth(QWIDGETSIZE_MAX);
            m_detailFrame->setProperty("overlay", true);
            m_detail->setOverlayMode(true);
        } else {
            // 覆盖时的 surface 裁剪会随控件保留；停靠后由 splitter 统一裁剪。
            m_detailFrame->clearMask();
            m_splitter->insertWidget(2, m_detailFrame);
            m_detailFrame->setMaximumWidth(QWIDGETSIZE_MAX);
            m_detailFrame->setProperty("overlay", false);
            m_detail->setOverlayMode(false);
        }
        m_detailFrame->style()->unpolish(m_detailFrame);
        m_detailFrame->style()->polish(m_detailFrame);
    }
    if (showDockedDetail) m_detailFrame->setMinimumWidth(detailMinimum);
    m_detailOverlay = wantOverlay;
    m_sidebar->setVisible(showSidebar);
    m_middle->show();
    m_detailFrame->setVisible(showDockedDetail || wantOverlay);
    if (m_sidebarToggle) {
        const QSignalBlocker blocker(m_sidebarToggle);
        m_sidebarToggle->setChecked(showSidebar);
        m_sidebarToggle->setIcon(AppIcons::icon(
            showSidebar ? IconId::SidebarCollapse : IconId::SidebarExpand,
            WorkspaceGeometry::SidebarToggleIconSize));
        m_sidebarToggle->setToolTip(showSidebar ? QStringLiteral("隐藏筛选栏")
                                                : QStringLiteral("显示筛选栏"));
        m_sidebarToggle->setAccessibleName(m_sidebarToggle->toolTip());
    }

    QList<int> sizes{0, 0, 0};
    if (showDockedDetail) {
        const int side = showSidebar
            ? qMin(m_sidebarWidth, qMax(180, available - middleMinimum - detailMinimum - 1)) : 0;
        const int detail = qBound(detailMinimum, m_detailWidth,
                                  qMax(detailMinimum, available - side - middleMinimum - 1));
        sizes = {side, qMax(middleMinimum, available - side - detail - 2), detail};
    } else {
        const int side = showSidebar
            ? qMin(m_sidebarWidth, qMax(180, available - middleMinimum - 1)) : 0;
        sizes = {side, qMax(middleMinimum, available - side - (showSidebar ? 1 : 0)), 0};
    }
    m_splitter->setSizes(sizes);
    syncColumnResizeAreas();
    updateDetailOverlayGeometry();
    updateWindowControlsPosition();
    raiseWindowChrome();
    m_surface->updateContentClip();
    m_layoutUpdating = false;
}

void TaskPanel::syncColumnResizeAreas() {
    if (!m_splitter) return;
    for (SplitterResizeArea* area : m_columnResizeAreas) {
        const int index = area->objectName() == QLatin1String("SidebarResizeArea") ? 1 : 2;
        QWidget* handle = m_splitter->handle(index);
        const bool available = handle && !m_focusMode && m_middle->isVisible()
            && (index == 1 ? m_sidebar->isVisible()
                           : m_detailFrame->isVisible() && !m_detailAutoHidden);
        area->setVisible(available);
        if (!available) continue;
        const QPoint topLeft = handle->mapTo(this, QPoint(0, 0));
        area->setGeometry(topLeft.x() - 5, topLeft.y(), 12, handle->height());
        area->raise();
    }
}

void TaskPanel::updateDetailOverlayGeometry() {
    if (!m_detailOverlay) {
        if (m_detailOverlayShadow) m_detailOverlayShadow->hide();
        if (m_overlayResizeArea) m_overlayResizeArea->hide();
        return;
    }
    if (!m_detailFrame) return;
    const int inset = (isMaximized() || isFullScreen()) ? 0 : 21;
    const QRect content = m_surface->rect().adjusted(inset, inset, -inset, -inset);
    const int maximum = qMax(260, qMin(480, content.width() - 260));
    m_overlayWidth = qBound(260, m_overlayWidth, maximum);
    m_detailFrame->setMinimumWidth(260);
    m_detailFrame->setGeometry(content.right() - m_overlayWidth + 1, content.top(),
                               m_overlayWidth, content.height());
    if (m_detailOverlayShadow) {
        m_detailOverlayShadow->setGeometry(m_detailFrame->x() - 18, m_detailFrame->y(),
                                           18, m_detailFrame->height());
        m_detailOverlayShadow->show();
        m_detailOverlayShadow->raise();
    }
    m_detailFrame->show();
    m_detailFrame->raise();
    if (m_overlayResizeArea) {
        const QPoint edge = m_detailFrame->mapTo(this, QPoint(0, 0));
        m_overlayResizeArea->setGeometry(edge.x() - 6, edge.y(), 12, m_detailFrame->height());
        m_overlayResizeArea->show();
        m_overlayResizeArea->raise();
    }
    raiseWindowChrome();
    m_surface->updateContentClip();
}

void TaskPanel::updateWindowControlsPosition() {
    if (!m_windowControls) return;
    const int inset = (isMaximized() || isFullScreen()) ? 0 : 21;
    m_windowControls->setGeometry(width() - m_windowControls->width() - inset, inset,
                                 m_windowControls->width(), m_windowControls->height());
    if (auto* header = m_detail->findChild<QFrame*>(QStringLiteral("DetailHeader"))) {
        header->layout()->activate();
    }
}

void TaskPanel::raiseWindowChrome() {
    if (m_windowControls) m_windowControls->raise();
    // 透明边框必须最后叠放，悬浮背景不能覆盖窗口轮廓。
    if (m_border) m_border->raise();
}

void TaskPanel::refreshWorkspaceIcons() {
    if (auto* account = findChild<QToolButton*>(QStringLiteral("AccountButton")))
        account->setIcon(brandLogoIcon());
    if (m_railTasks) {
        QIcon tasks;
        tasks.addPixmap(AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::RailIcon, 28)
                            .pixmap(28, 28), QIcon::Normal, QIcon::Off);
        tasks.addPixmap(AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::RailIconHover, 28)
                            .pixmap(28, 28), QIcon::Active, QIcon::Off);
        tasks.addPixmap(AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::Primary, 28)
                            .pixmap(28, 28), QIcon::Normal, QIcon::On);
        tasks.addPixmap(AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::Primary, 28)
                            .pixmap(28, 28), QIcon::Active, QIcon::On);
        m_railTasks->setIcon(tasks);
    }
    if (m_sidebarToggle)
        m_sidebarToggle->setIcon(AppIcons::icon(
            m_sidebarToggle->isChecked() ? IconId::SidebarCollapse : IconId::SidebarExpand,
            WorkspaceGeometry::SidebarToggleIconSize));
    const auto railIcon = [](const QString& name) {
        QIcon icon;
        icon.addPixmap(AppIcons::icon(name, ds::ColorToken::RailIcon, 28).pixmap(28, 28),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(AppIcons::icon(name, ds::ColorToken::RailIconHover, 28).pixmap(28, 28),
                       QIcon::Active, QIcon::Off);
        return icon;
    };
    if (m_railSettings) m_railSettings->setIcon(railIcon(QStringLiteral("setting-sidebar")));
    if (auto* sync = findChild<QToolButton*>(QStringLiteral("RailSync")))
        sync->setIcon(railIcon(AppIcons::iconName(IconId::Sync)));
    if (auto* add = findChild<QToolButton*>(QStringLiteral("GroupMenuButton")))
        add->setIcon(AppIcons::icon(IconId::Add, 18));
    if (m_workspaceMore)
        m_workspaceMore->setIcon(AppIcons::icon(IconId::MoreTitlebar,
                                                WorkspaceGeometry::SidebarToggleIconSize));
    if (m_quickDate) {
        m_quickDate->setIcon(AppIcons::icon(IconId::Calendar, 15));
        m_quickDate->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        updateQuickDateButton();
    }
    updateHeadingAppearance();
}

void TaskPanel::selectNavigation(ViewScope scope, const QString& listId) {
    if (scope == ViewScope::List) {
        for (const TaskList& list : m_store->lists()) {
            if (list.id == listId && m_collapsedFolders.contains(list.folderId)) {
                m_collapsedFolders.remove(list.folderId);
                rebuildNavigation();
                break;
            }
        }
    }
    for (int row = 0; row < m_navigation->count(); ++row) {
        const auto* item = m_navigation->item(row);
        if (item->data(NavigationDelegate::SectionRole).toBool()
            || item->data(NavigationDelegate::DividerRole).toBool()) continue;
        if (static_cast<ViewScope>(item->data(Qt::UserRole).toInt()) != scope) continue;
        const bool matches = scope == ViewScope::Folder
            ? item->data(NavigationDelegate::FolderIdRole).toString() == listId
            : scope != ViewScope::List || item->data(Qt::UserRole + 1).toString() == listId;
        if (matches) {
            m_navigation->setCurrentRow(row);
            return;
        }
    }
}

void TaskPanel::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    syncWindowDecoration();
    reconcileWorkspaceLayout();
}

void TaskPanel::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    reconcileWorkspaceLayout();
    syncWindowDecoration();
}

void TaskPanel::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && !isMinimized()) {
        syncWindowDecoration();
        reconcileWorkspaceLayout();
        QTimer::singleShot(0, this, [this]() {
            if (isMinimized()) return;
            syncWindowDecoration();
            reconcileWorkspaceLayout();
        });
        update();
    }
}

void TaskPanel::syncWindowDecoration() {
    if (!m_surface || !m_border) return;
    const bool maximized = isMaximized() || isFullScreen();
    const int inset = maximized ? 0 : 21;
    m_surface->setFramelessCorners(!maximized);
    if (auto* layout = m_surface->layout()) layout->setContentsMargins(inset, inset, inset, inset);
    m_border->setRounded(!maximized);
    m_border->setGeometry(m_surface->rect().adjusted(inset, inset, -inset, -inset));
    updateWindowControlsPosition();
    updateDetailOverlayGeometry();
    for (WindowResizeArea* area : m_resizeAreas) {
        area->setVisible(!maximized);
        if (maximized) continue;
        const QString name = area->objectName();
        if (name == QLatin1String("WindowResizeLeft"))
            area->setGeometry(inset - 6, inset, 12, height() - 2 * inset);
        else if (name == QLatin1String("WindowResizeRight"))
            area->setGeometry(width() - inset - 6, inset, 12, height() - 2 * inset);
        else if (name == QLatin1String("WindowResizeTop"))
            area->setGeometry(inset, inset - 6, width() - 2 * inset, 12);
        else if (name == QLatin1String("WindowResizeBottom"))
            area->setGeometry(inset, height() - inset - 6, width() - 2 * inset, 12);
        else if (name == QLatin1String("WindowResizeTopLeft"))
            area->setGeometry(inset - 6, inset - 6, 12, 12);
        else if (name == QLatin1String("WindowResizeTopRight"))
            area->setGeometry(width() - inset - 6, inset - 6, 12, 12);
        else if (name == QLatin1String("WindowResizeBottomLeft"))
            area->setGeometry(inset - 6, height() - inset - 6, 12, 12);
        else
            area->setGeometry(width() - inset - 6, height() - inset - 6, 12, 12);
        area->raise();
    }
    m_surface->updateContentClip();
    syncColumnResizeAreas();
    raiseWindowChrome();
}

void TaskPanel::paintEvent(QPaintEvent* event) {
    QMainWindow::paintEvent(event);
}

void TaskPanel::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->pos().y() < 48 && !isMaximized()) {
        m_windowDragOffset = event->globalPos() - frameGeometry().topLeft();
        m_windowDragging = true;
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
}

void TaskPanel::mouseMoveEvent(QMouseEvent* event) {
    if (m_windowDragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_windowDragOffset);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void TaskPanel::mouseReleaseEvent(QMouseEvent* event) {
    m_windowDragging = false;
    QMainWindow::mouseReleaseEvent(event);
}

void TaskPanel::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->pos().y() < 48) {
        setWindowState(isMaximized() ? Qt::WindowNoState : Qt::WindowMaximized);
        event->accept();
        return;
    }
    QMainWindow::mouseDoubleClickEvent(event);
}

// ========== 任务右键：统一实体命令 + 本应用已有的桌面能力 ==========
void TaskPanel::showTaskMenu(const QPoint& position) {
    const QString id = m_model->entityId(m_list->indexAt(position));
    if (id.isEmpty() || !flush()) return;
    const Entity entity = m_store->entity(id);
    if (entity.id.isEmpty()) return;

    EntityCommands commands(m_store);
    MenuContext context;
    context.kind = entity.kind;
    context.completed = entity.completed;
    context.abandoned = entity.status == TaskStatus::Abandoned;
    context.deleted = entity.deleted;
    context.pinned = entity.pinnedAt.isValid();
    context.hasDate = effectiveDate(entity).isValid();
    QMenu* menu = commands.buildEntityMenu(context, id, this);
    menu->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(&commands, &EntityCommands::errorOccurred, this, &TaskPanel::showError);
    connect(&commands, &EntityCommands::commandExecuted, this,
            [this](EntityCommand command, const QString& entityId) {
        if (command == EntityCommand::ViewActivity) {
            showTask(entityId);
            m_detail->showActivity();
        } else if (command == EntityCommand::UploadAttachment) {
            showTask(entityId);
            m_detail->importAttachments();
        }
    });
    connect(&commands, &EntityCommands::entityCreated, this, [this](const QString& created) {
        showTask(created);
        m_detail->focusTitle();
    });

    if (!entity.deleted && entity.kind == EntityKind::Task) {
        const bool desktopOpen = m_store->desktopNote(id).isOpen();
        auto* desktop = new QAction(AppIcons::icon(IconId::DesktopNote, 18),
                                    desktopOpen ? QStringLiteral("关闭桌面便签")
                                                : QStringLiteral("贴到桌面"), menu);
        desktop->setObjectName(QStringLiteral("CmdDesktopNote"));
        connect(desktop, &QAction::triggered, this, [this, id, open = !desktopOpen]() {
            const Entity current = m_store->entity(id);
            if (current.id.isEmpty() || current.deleted || current.kind != EntityKind::Task
                || m_store->desktopNote(id).isOpen() == open) return;
            if (!m_store->saveDesktopState(id, current.color, current.geometry,
                                           current.zOrder, current.layer, open))
                showError(m_store->lastError());
        });
        QAction* before = nullptr;
        for (QAction* action : menu->actions())
            if (action->objectName() == QLatin1String("delete")) before = action;
        menu->insertAction(before, desktop);
    }
    menu->exec(m_list->viewport()->mapToGlobal(position));
}

void TaskPanel::createListDialog(const QString& folderId, EntityKind kind) {
    if (!flush()) return;
    ListEditorDialog dialog(m_store->lists(), m_store->folders(), this);
    dialog.setNewListDefaults(folderId, kind);
    if (dialog.exec() != QDialog::Accepted) return;
    const TaskList draft = dialog.draft();
    const QString id = m_store->createList(draft.name, draft.folderId, kind,
                                           draft.icon, draft.color);
    if (!id.isEmpty()) selectNavigation(ViewScope::List, id);
}

void TaskPanel::editListDialog(const TaskList& list) {
    if (list.inbox || !flush()) return;
    ListEditorDialog dialog(m_store->lists(), m_store->folders(), this);
    dialog.setList(list);
    if (dialog.exec() == QDialog::Accepted) m_store->saveList(dialog.draft());
}

// ========== R3: 文件夹与清单管理菜单 ==========
void TaskPanel::groupMenu() {
    PopupMenu menu(this);
    auto* newFolder = menu.addAction(AppIcons::icon(IconId::Folder, 16), QStringLiteral("新建文件夹…"));
    newFolder->setObjectName(QStringLiteral("CmdNewFolder"));
    connect(newFolder, &QAction::triggered, this, [this]() {
        FolderEditorDialog dialog(this);
        if (dialog.exec() == QDialog::Accepted) {
            const Folder draft = dialog.draft();
            m_store->createFolder(draft.name, draft.icon);
        }
    });
    const auto createList = [this, &menu](const QString& label, EntityKind kind) {
        auto* action = menu.addAction(
            AppIcons::icon(kind == EntityKind::Note ? IconId::Note : IconId::TaskList, 16), label);
        action->setObjectName(kind == EntityKind::Note ? QStringLiteral("CmdNewNoteList")
                                                       : QStringLiteral("CmdNewTaskList"));
        connect(action, &QAction::triggered, this, [this, kind]() {
            createListDialog(QString(), kind);
        });
    };
    createList(QStringLiteral("新建任务清单…"), EntityKind::Task);
    createList(QStringLiteral("新建笔记清单…"), EntityKind::Note);
    menu.addSeparator();
    auto* hint = menu.addAction(QStringLiteral("在左侧文件夹或清单上右键可移动、重命名或删除"));
    hint->setEnabled(false);
    menu.exec(QCursor::pos());
}

// ========== R3: 侧栏对象右键菜单 ==========
void TaskPanel::organizationMenu(const QPoint& position) {
    QListWidgetItem* item = m_navigation->itemAt(position); if (!item) return;
    const QString folderId = item->data(NavigationDelegate::FolderIdRole).toString();
    const QString listId = item->data(Qt::UserRole + 1).toString();
    if (folderId.isEmpty() && listId.isEmpty()) {
        if (item->data(Qt::UserRole + 2).toString() == QLatin1String("清单")) groupMenu();
        return;
    }
    PopupMenu menu(this);
    if (!folderId.isEmpty()) {
        const auto createInFolder = [this, &menu, folderId](const QString& label, EntityKind kind) {
            auto* action = menu.addAction(label);
            connect(action, &QAction::triggered, this, [this, folderId, kind]() {
                createListDialog(folderId, kind);
            });
        };
        createInFolder(QStringLiteral("在此新建任务清单…"), EntityKind::Task);
        createInFolder(QStringLiteral("在此新建笔记清单…"), EntityKind::Note);
        menu.addSeparator();
        connect(menu.addAction(QStringLiteral("编辑文件夹…")), &QAction::triggered, this, [this, folderId]() {
            for (const Folder& folder : m_store->folders()) {
                if (folder.id != folderId) continue;
                FolderEditorDialog dialog(this);
                dialog.setFolder(folder);
                if (dialog.exec() == QDialog::Accepted) m_store->saveFolder(dialog.draft());
                break;
            }
        });
        connect(menu.addAction(QStringLiteral("删除文件夹…")), &QAction::triggered, this, [this, folderId]() {
            if (ConfirmDialog::prompt(QStringLiteral("删除文件夹"),
                    QStringLiteral("删除后，其中清单会平铺显示，清单和实体内容均保留。"),
                    QStringLiteral("删除"), true, this))
                m_store->deleteFolder(folderId);
        });
    } else {
        TaskList selected;
        for (const TaskList& list : m_store->lists()) if (list.id == listId) selected = list;
        if (selected.inbox) {
            auto* protectedAction = menu.addAction(QStringLiteral("系统收集箱不可移动或删除"));
            protectedAction->setEnabled(false);
        } else {
            const bool archived = selected.archivedAtUtcMs > 0;
            auto* archive = menu.addAction(
                AppIcons::icon(archived ? IconId::Unarchive : IconId::Archive, 16),
                archived ? QStringLiteral("取消归档") : QStringLiteral("归档清单"));
            archive->setObjectName(archived ? QStringLiteral("CmdUnarchiveList")
                                            : QStringLiteral("CmdArchiveList"));
            connect(archive, &QAction::triggered, this, [this, selected, archived]() {
                if (!archived) m_archivedExpanded = true;
                m_store->setListArchived(selected.id, !archived);
            });
            menu.addSeparator();
            connect(menu.addAction(QStringLiteral("编辑清单…")), &QAction::triggered, this, [this, selected]() {
                editListDialog(selected);
            });
            QMenu* move = menu.addMenu(QStringLiteral("归入文件夹"));
            for (const Folder& folder : m_store->folders()) {
                QAction* action = move->addAction(folder.name);
                action->setEnabled(folder.id != selected.folderId);
                connect(action, &QAction::triggered, this, [this, selected, folder]() {
                    m_store->setListFolder(selected.id, folder.id);
                });
            }
            move->setEnabled(!m_store->folders().isEmpty());
            auto* flat = menu.addAction(QStringLiteral("移出文件夹（平铺显示）"));
            flat->setEnabled(!selected.folderId.isEmpty());
            connect(flat, &QAction::triggered, this, [this, selected]() {
                m_store->setListFolder(selected.id, QString());
            });
            menu.addSeparator();
            connect(menu.addAction(QStringLiteral("删除清单…")), &QAction::triggered, this, [this, selected]() {
                if (!ConfirmDialog::prompt(QStringLiteral("删除清单“%1”？").arg(selected.name),
                        QStringLiteral("清单内的所有任务和笔记也将被移入垃圾桶。"),
                        QStringLiteral("删除"), true, this)) return;
                if (m_store->deleteList(selected.id) && m_filter.scope == ViewScope::List
                    && m_filter.listId == selected.id) {
                    m_filter = ViewSpec(); m_heading->setText(scopeLabel(ViewScope::AllLists));
                    applyFilter(); rebuildNavigation();
                }
            });
        }
    }
    menu.exec(m_navigation->viewport()->mapToGlobal(position));
}

void TaskPanel::setAutostart(AutostartSettings* autostart) {
    m_autostart = autostart;
}

void TaskPanel::showSettings() {
    SettingsDialog dialog(m_autostart, this);
    connect(&dialog, &SettingsDialog::taskWidgetRequested, this, &TaskPanel::taskWidgetRequested);
    connect(&dialog, &SettingsDialog::smartListVisibilityChanged, this, [this]() {
        m_keepHiddenView = false;
        rebuildNavigation();
    });
    dialog.exec();
}

// 数据入口集中在同步按钮，后续云同步也可以复用同一宿主菜单。
void TaskPanel::dataMenu() {
    PopupMenu menu(this);
    menu.setObjectName(QStringLiteral("DataMenu"));
    auto* localStatus = menu.addAction(AppIcons::icon(IconId::Sync, 16),
                                       QStringLiteral("数据仅保存在本机"));
    localStatus->setObjectName(QStringLiteral("LocalDataStatus"));
    localStatus->setEnabled(false);
    menu.addSeparator();
    auto* exportAction = menu.addAction(AppIcons::icon(IconId::Backup, 16), QStringLiteral("导出备份…"));
    exportAction->setObjectName(QStringLiteral("CmdExportBackup"));
    connect(exportAction, &QAction::triggered, this, [this]() {
        if (!flush()) return;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出备份"),
            QStringLiteral("KylinList-%1.klb").arg(toAppTime(appNow()).date().toString(Qt::ISODate)),
            Branding::displayName() + QStringLiteral("备份（含附件） (*.klb)"));
        if (!path.isEmpty() && m_store->exportBackup(path))
            showNotice(QStringLiteral("备份已保存：") + path);
    });
    auto* importAction = menu.addAction(AppIcons::icon(IconId::Restore, 16), QStringLiteral("导入备份…"));
    importAction->setObjectName(QStringLiteral("CmdImportBackup"));
    connect(importAction, &QAction::triggered, this, [this]() {
        if (!flush()) return;
        const QString path = QFileDialog::getOpenFileName(this,
            QStringLiteral("导入备份（覆盖本机数据）"), QString(),
            Branding::displayName() + QStringLiteral("备份 (*.klb)"));
        if (path.isEmpty()) return;
        if (QMessageBox::warning(this, QStringLiteral("确认覆盖恢复"),
                QStringLiteral("本机现有全部任务、笔记与清单都会被删除。建议先导出当前备份。\n\n此操作无法撤销，是否继续？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            emit importBackupRequested(path);
    });
    menu.addSeparator();
    auto* openData = menu.addAction(QStringLiteral("打开数据位置"));
    openData->setObjectName(QStringLiteral("CmdOpenData"));
    connect(openData, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirPath()));
    });
    menu.exec(QCursor::pos());
}

void TaskPanel::accountMenu() {
    PopupMenu menu(this);
    menu.setObjectName(QStringLiteral("AccountMenu"));
    auto* usageStats = menu.addAction(QStringLiteral("使用统计…"));
    usageStats->setObjectName(QStringLiteral("CmdUsageStats"));
    connect(usageStats, &QAction::triggered, this, &TaskPanel::statsRequested);
    menu.addSeparator();
    auto* checkUpdate = menu.addAction(QStringLiteral("检查更新…"));
    checkUpdate->setObjectName(QStringLiteral("CmdCheckUpdate"));
    connect(checkUpdate, &QAction::triggered, this, &TaskPanel::updateCheckRequested);
    auto* updateServer = menu.addAction(QStringLiteral("更新服务器…"));
    updateServer->setObjectName(QStringLiteral("CmdUpdateServer"));
    connect(updateServer, &QAction::triggered, this, &TaskPanel::updateSettingsRequested);
    menu.addSeparator();
    auto* quit = menu.addAction(QStringLiteral("退出%1").arg(Branding::displayName()));
    quit->setObjectName(QStringLiteral("CmdQuit"));
    connect(quit, &QAction::triggered, this, [this]() { if (flush()) emit quitRequested(); });
    menu.exec(QCursor::pos());
}

// 工作区显示选项：只提供已经落地的显示能力，不展示未实现的看板/时间线入口。
void TaskPanel::workspaceMenu() {
    PopupMenu menu(this);
    menu.setObjectName(QStringLiteral("WorkspaceMenu"));
    menu.setProperty("entityMenu", true);
    // 分组父菜单容纳右侧当前值，网页约 202px；选项子菜单为 164px。
    menu.setMinimumWidth(202 + 2 * PopupMenu::shadowMargin());
    auto* grouping = menu.addMenu(AppIcons::icon(IconId::Grouping, 18), QStringLiteral("分组"));
    grouping->setObjectName(QStringLiteral("GroupingMenu"));
    grouping->setProperty("entityMenu", true);
    grouping->setMinimumWidth(164 + 2 * PopupMenu::shadowMargin());
    auto* choices = new QActionGroup(grouping);
    choices->setExclusive(true);
    const QPair<GroupMode, QString> modes[] = {
        {GroupMode::List, QStringLiteral("清单")}, {GroupMode::Time, QStringLiteral("时间")},
        {GroupMode::Created, QStringLiteral("创建时间")}, {GroupMode::Priority, QStringLiteral("优先级")},
        {GroupMode::None, QStringLiteral("无分组")}
    };
    for (const auto& mode : modes) {
        if (m_filter.scope == ViewScope::List && mode.first == GroupMode::List) continue;
        auto* action = grouping->addAction(mode.second);
        action->setObjectName(QStringLiteral("GroupMode_") + groupModeName(mode.first));
        action->setProperty("groupingMode", static_cast<int>(mode.first));
        action->setCheckable(true);
        action->setChecked(m_filter.grouping == mode.first);
        choices->addAction(action);
        if (action->isChecked()) grouping->menuAction()->setProperty("trailingText", mode.second);
        connect(action, &QAction::triggered, this, [this, mode]() { setGrouping(mode.first); });
    }
    menu.addSeparator();

    const bool canToggleCompleted = m_filter.scope != ViewScope::Completed
        && m_filter.scope != ViewScope::Abandoned && m_filter.scope != ViewScope::Trash;
    auto* toggleCompleted = menu.addAction(AppIcons::icon(IconId::HideCompleted, 18),
        m_filter.showCompleted ? QStringLiteral("隐藏已完成") : QStringLiteral("显示已完成"));
    toggleCompleted->setObjectName(QStringLiteral("HideCompleted"));
    toggleCompleted->setVisible(canToggleCompleted);
    connect(toggleCompleted, &QAction::triggered, this, [this]() {
        m_filter.showCompleted = !m_filter.showCompleted;
        applyFilter();
    });

    auto* toggleDetails = menu.addAction(AppIcons::icon(IconId::HideDetails, 18),
        m_filter.showDetails ? QStringLiteral("隐藏详细") : QStringLiteral("显示详细"));
    toggleDetails->setObjectName(QStringLiteral("HideDetails"));
    connect(toggleDetails, &QAction::triggered, this, [this]() {
        m_filter.showDetails = !m_filter.showDetails;
        applyFilter();
    });

    auto* displaySettings = menu.addAction(AppIcons::icon(IconId::DisplaySettings, 18),
                                           QStringLiteral("显示设置"));
    displaySettings->setObjectName(QStringLiteral("DisplaySettings"));
    connect(displaySettings, &QAction::triggered, this, [this]() {
        DisplaySettingsDialog dialog(m_filter.countdownDates, m_preferDetailOverlay, this);
        dialog.setCompletedVisible(m_filter.showCompleted);
        connect(&dialog, &DisplaySettingsDialog::completedVisibilityChanged, this, [this](bool visible) {
            m_filter.showCompleted = visible;
            applyFilter();
        });
        connect(&dialog, &DisplaySettingsDialog::optionsChanged, this,
                [this](bool countdownDates, bool popupDetail) {
            m_filter.countdownDates = countdownDates;
            m_preferDetailOverlay = popupDetail;
            applyFilter();
            reconcileWorkspaceLayout();
        });
        dialog.exec();
    });

    // 与网页 bottomRight 一致：卡片右缘对齐按钮右缘，而不是以鼠标位置为左上角。
    menu.ensurePolished();
    const int cardWidth = menu.sizeHint().width() - 2 * PopupMenu::shadowMargin();
    const QPoint anchor = m_workspaceMore->mapToGlobal(
        QPoint(m_workspaceMore->width() - cardWidth, m_workspaceMore->height() + 4));
    menu.exec(anchor);
}

bool TaskPanel::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG")) {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE) {
            // 去掉原生标题栏和非客户区，但保留 Windows 的边框命中与最大化管理。
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST && !isMaximized() && !isFullScreen()) {
            RECT frame;
            GetWindowRect(msg->hwnd, &frame);
            const int x = GET_X_LPARAM(msg->lParam) - frame.left;
            const int y = GET_Y_LPARAM(msg->lParam) - frame.top;
            const int w = frame.right - frame.left;
            const int h = frame.bottom - frame.top;
            const int border = qRound(6 * devicePixelRatioF());
            const int inset = qRound(21 * devicePixelRatioF());
            // 透明外边距属于阴影；在可见边框及其外侧都提供缩放命中。
            const bool left = x < border || qAbs(x - inset) < border;
            const bool right = x >= w - border || qAbs(x - (w - inset)) < border;
            const bool top = y < border || qAbs(y - inset) < border;
            const bool bottom = y >= h - border || qAbs(y - (h - inset)) < border;
            if (top && left) *result = HTTOPLEFT;
            else if (top && right) *result = HTTOPRIGHT;
            else if (bottom && left) *result = HTBOTTOMLEFT;
            else if (bottom && right) *result = HTBOTTOMRIGHT;
            else if (left) *result = HTLEFT;
            else if (right) *result = HTRIGHT;
            else if (top) *result = HTTOP;
            else if (bottom) *result = HTBOTTOM;
            else return false;
            return true;
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void TaskPanel::closeEvent(QCloseEvent* event) {
    if (!flush()) { event->ignore(); return; }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("sidebarWidth"), m_sidebarWidth);
    settings.setValue(QStringLiteral("detailWidth"), m_detailWidth);
    settings.setValue(QStringLiteral("sidebarHidden"), m_sidebarUserHidden);
    settings.setValue(QStringLiteral("showCompleted"), m_filter.showCompleted);
    settings.setValue(QStringLiteral("showDetails"), m_filter.showDetails);
    settings.setValue(QStringLiteral("countdownDates"), m_filter.countdownDates);
    settings.setValue(QStringLiteral("detailPopup"), m_preferDetailOverlay);
    if (QSystemTrayIcon::isSystemTrayAvailable()) { hide(); event->ignore(); }
    else { event->accept(); emit quitRequested(); }
}
}  // namespace kl
