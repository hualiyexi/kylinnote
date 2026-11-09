#include "TaskPanel.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "../platform/Autostart.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "TaskDelegate.h"
#include "TaskDetailWidget.h"
#include "TaskListModel.h"
#include "ToastIsland.h"
#include "WindowChrome.h"
#include "WorkspaceSurface.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace kl {
namespace {
const int FolderIdRole = Qt::UserRole + 6;

QPixmap brandMark(int size) {
    return AppIcons::icon(IconId::TaskList, ds::ColorToken::Primary, size)
        .pixmap(size, size);
}

bool supportsTime(ViewScope scope) {
    return scope == ViewScope::Today || scope == ViewScope::Next7Days || scope == ViewScope::Overdue;
}

QString scopeLabel(ViewScope scope) {
    switch (scope) {
        case ViewScope::AllLists: return QStringLiteral("全部待办");
        case ViewScope::Today: return QStringLiteral("今天");
        case ViewScope::Next7Days: return QStringLiteral("七天内");
        case ViewScope::Overdue: return QStringLiteral("已逾期");
        case ViewScope::Completed: return QStringLiteral("已完成");
        case ViewScope::Abandoned: return QStringLiteral("已放弃");
        case ViewScope::Trash: return QStringLiteral("回收站");
        case ViewScope::List: break;
    }
    return QStringLiteral("清单");
}

// R3: 导航分区标签
IconId scopeIcon(ViewScope scope) {
    switch (scope) {
        case ViewScope::AllLists: return IconId::Inbox;
        case ViewScope::Today: return IconId::Today;
        case ViewScope::Next7Days: return IconId::Upcoming;
        case ViewScope::Overdue: return IconId::Overdue;
        case ViewScope::Completed: return IconId::Done;
        case ViewScope::Abandoned: return IconId::Abandoned;
        case ViewScope::Trash: return IconId::Trash;
        case ViewScope::List: break;
    }
    return IconId::TaskList;
}
}  // namespace

TaskPanel::TaskPanel(TaskStore* store, QWidget* parent) : QMainWindow(parent), m_store(store) {
    setObjectName(QStringLiteral("TaskPanel"));
    setWindowTitle(QStringLiteral("麒麟清单"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    if (auto* oldStatusBar = findChild<QStatusBar*>(QString(), Qt::FindDirectChildrenOnly))
        delete oldStatusBar;
    setMinimumSize(960, 620); resize(1240, 820);
    buildUi();
    for (const Entity& entity : m_store->entities()) m_taskStates.insert(entity.id, entity.status);
    connect(m_detail, &TaskDetailWidget::internalLinkRequested, this, &TaskPanel::showTask);
    rebuildNavigation();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    m_splitter->restoreState(settings.value(QStringLiteral("splitter")).toByteArray());
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
    connect(clock, &QTimer::timeout, m_model, &TaskListModel::refresh); clock->start();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        refreshWorkspaceIcons();
        rebuildNavigation();
    });
}

void TaskPanel::buildUi() {
    auto* central = new WorkspaceSurface(this);
    m_surface = central;
    central->setObjectName(QStringLiteral("WorkspaceRoot"));
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 16, 16, 16);
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

    // 第一列：窄功能按钮列。顶部头像区域同时是窗口拖动区。
    m_activityRail = new QFrame(central);
    m_activityRail->setObjectName(QStringLiteral("ActivityRail"));
    m_activityRail->setFixedWidth(52);
    auto* rail = new QVBoxLayout(m_activityRail);
    rail->setContentsMargins(6, 44, 6, 10);
    rail->setSpacing(8);
    auto* account = new QToolButton(m_activityRail);
    account->setObjectName(QStringLiteral("AccountButton"));
    account->setFixedSize(36, 36);
    account->setIconSize(QSize(30, 30));
    account->setToolTip(QStringLiteral("本机工作区"));
    account->setAccessibleName(QStringLiteral("本机工作区"));
    account->setCursor(Qt::PointingHandCursor);
    connect(account, &QToolButton::clicked, this, &TaskPanel::backupMenu);
    rail->addWidget(account, 0, Qt::AlignHCenter);
    rail->addSpacing(12);
    const auto railButton = [this, rail](QToolButton*& target, const QString& name,
                                        IconId icon, const QString& tooltip) {
        target = new QToolButton(m_activityRail);
        target->setObjectName(name);
        target->setFixedSize(40, 40);
        target->setIconSize(QSize(20, 20));
        target->setAutoRaise(true);
        target->setToolTip(tooltip);
        target->setAccessibleName(tooltip);
        target->setCursor(Qt::PointingHandCursor);
        rail->addWidget(target, 0, Qt::AlignHCenter);
    };
    railButton(m_railTasks, QStringLiteral("RailTasks"), IconId::Checklist, QStringLiteral("任务"));
    m_railTasks->setCheckable(true);
    m_railTasks->setChecked(true);
    connect(m_railTasks, &QToolButton::clicked, this, [this]() {
        selectNavigation(ViewScope::AllLists);
        focusQuickAdd();
    });
    railButton(m_railSearch, QStringLiteral("RailSearch"), IconId::Search, QStringLiteral("搜索"));
    connect(m_railSearch, &QToolButton::clicked, this, [this]() {
        m_search->setVisible(true);
        m_search->setFocus();
        m_search->selectAll();
    });
    auto* railNotes = new QToolButton(m_activityRail);
    railNotes->setObjectName(QStringLiteral("RailNotes"));
    railNotes->setFixedSize(40, 40);
    railNotes->setIconSize(QSize(20, 20));
    railNotes->setAutoRaise(true);
    railNotes->setToolTip(QStringLiteral("笔记清单"));
    railNotes->setAccessibleName(QStringLiteral("笔记清单"));
    connect(railNotes, &QToolButton::clicked, this, [this]() {
        for (const TaskList& list : m_store->lists()) {
            if (list.kind == EntityKind::Note) {
                selectNavigation(ViewScope::List, list.id);
                return;
            }
        }
        showNotice(QStringLiteral("可在文件夹菜单中新建笔记清单。"));
    });
    rail->addWidget(railNotes, 0, Qt::AlignHCenter);
    rail->addStretch(1);
    auto* sync = new QToolButton(m_activityRail);
    sync->setObjectName(QStringLiteral("RailSync"));
    sync->setFixedSize(40, 40);
    sync->setAutoRaise(true);
    sync->setToolTip(QStringLiteral("数据仅保存在本机"));
    sync->setAccessibleName(QStringLiteral("本机数据说明"));
    connect(sync, &QToolButton::clicked, this, [this]() {
        showNotice(QStringLiteral("任务、笔记与附件仅保存在本机，无需联网。"));
    });
    rail->addWidget(sync, 0, Qt::AlignHCenter);
    railButton(m_railSettings, QStringLiteral("RailSettings"), IconId::Settings, QStringLiteral("备份与设置"));
    connect(m_railSettings, &QToolButton::clicked, this, &TaskPanel::backupMenu);
    body->addWidget(m_activityRail);

    // 第二列：筛选与组织。顶部没有独立标题条。
    m_sidebar = new QFrame(central);
    m_sidebar->setObjectName(QStringLiteral("Sidebar"));
    m_sidebar->setFixedWidth(AppStyle::metric(AppStyle::Metric::SidebarWidth));
    auto* side = new QVBoxLayout(m_sidebar);
    side->setContentsMargins(8, 36, 8, 8);
    side->setSpacing(2);
    auto* sideTop = new QHBoxLayout;
    sideTop->setContentsMargins(0, 0, 0, 0);
    sideTop->addStretch(1);
    auto* addList = new QToolButton(m_sidebar);
    addList->setObjectName(QStringLiteral("GroupMenuButton"));
    addList->setFixedSize(32, 32);
    addList->setToolTip(QStringLiteral("新建文件夹或清单"));
    addList->setAccessibleName(QStringLiteral("文件夹与清单管理"));
    addList->setAutoRaise(true);
    connect(addList, &QToolButton::clicked, this, &TaskPanel::groupMenu);
    sideTop->addWidget(addList, 0, Qt::AlignVCenter);
    side->addLayout(sideTop);

    m_navigation = new QListWidget(m_sidebar);
    m_navigation->setObjectName(QStringLiteral("Navigation"));
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation));
    m_navigation->setMouseTracking(true);
    m_navigation->viewport()->setAttribute(Qt::WA_Hover, true);
    m_navigation->setAccessibleName(QStringLiteral("任务导航"));
    m_navigation->setContextMenuPolicy(Qt::CustomContextMenu);
    side->addWidget(m_navigation, 1);
    auto* backup = new QToolButton(m_sidebar);
    backup->setObjectName(QStringLiteral("BackupMenuButton"));
    backup->setFixedSize(32, 32);
    backup->setToolTip(QStringLiteral("备份与设置"));
    backup->setAccessibleName(QStringLiteral("备份与设置"));
    backup->setAutoRaise(true);
    backup->hide();
    connect(backup, &QToolButton::clicked, this, &TaskPanel::backupMenu);
    side->addWidget(backup, 0, Qt::AlignLeft);
    body->addWidget(m_sidebar);

    // 第三列与可选第四列共用分隔器；详情隐藏时清单自然占满余宽。
    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setObjectName(QStringLiteral("WorkspaceSplitter"));
    m_splitter->setHandleWidth(1);
    body->addWidget(m_splitter, 1);
    m_middle = new QFrame(m_splitter);
    m_middle->setObjectName(QStringLiteral("ListPane"));
    m_middle->setMinimumWidth(320);
    auto* center = new QVBoxLayout(m_middle);
    center->setContentsMargins(0, 36, 0, 0);
    center->setSpacing(0);

    auto* listHeader = new QFrame(m_middle);
    listHeader->setObjectName(QStringLiteral("ListHeader"));
    listHeader->setFixedHeight(40);
    auto* header = new QHBoxLayout(listHeader);
    header->setContentsMargins(22, 0, 10, 0);
    header->setSpacing(4);
    auto* headingIcon = new QLabel(listHeader);
    headingIcon->setObjectName(QStringLiteral("ListHeadingIcon"));
    headingIcon->setFixedSize(20, 24);
    header->addWidget(headingIcon, 0, Qt::AlignVCenter);
    m_heading = new QLabel(QStringLiteral("全部待办"), listHeader);
    m_heading->setFont(ds::qfont(ds::FontScene::Section));
    header->addWidget(m_heading, 0, Qt::AlignVCenter);
    m_count = new QLabel(listHeader);
    m_count->setObjectName(QStringLiteral("ListCount"));
    m_count->setProperty("muted", true);
    m_count->setFont(AppStyle::font(AppStyle::Font::Small));
    header->addWidget(m_count, 0, Qt::AlignVCenter);
    header->addStretch(1);
    m_renderer = new QComboBox(listHeader);
    m_renderer->setObjectName(QStringLiteral("ViewRenderer"));
    m_renderer->setAccessibleName(QStringLiteral("视图呈现方式"));
    m_renderer->addItem(QStringLiteral("清单视图"), static_cast<int>(ViewRenderer::List));
    m_renderer->addItem(QStringLiteral("时间视图"), static_cast<int>(ViewRenderer::Time));
    m_renderer->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_renderer->setFixedHeight(32);
    m_renderer->hide();
    header->addWidget(m_renderer);
    auto* sort = new QToolButton(listHeader);
    sort->setObjectName(QStringLiteral("ListSort"));
    sort->setFixedSize(32, 32);
    sort->setAutoRaise(true);
    sort->setToolTip(QStringLiteral("排序与视图"));
    sort->setAccessibleName(QStringLiteral("排序与视图"));
    connect(sort, &QToolButton::clicked, this, &TaskPanel::workspaceMenu);
    header->addWidget(sort);
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
    content->setContentsMargins(22, 16, 22, 12);
    content->setSpacing(12);
    center->addLayout(content, 1);

    // 快速添加是一整条轻边框输入行，不再附加搜索框和独立添加按钮。
    auto* quickFrame = new QFrame(m_middle);
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
    m_quickDate->setToolTip(QStringLiteral("新任务默认安排到今天"));
    m_quickDate->setAccessibleName(QStringLiteral("切换新任务默认日期"));
    m_quickDate->setCheckable(true);
    m_quickDate->setAutoRaise(true);
    m_quickDate->setFixedSize(68, 30);
    quick->addWidget(m_quickDate);
    auto* add = new QPushButton(QStringLiteral("添加"), quickFrame);
    add->setObjectName(QStringLiteral("QuickAddButton"));
    add->hide();
    connect(add, &QPushButton::clicked, this, &TaskPanel::createTask);
    connect(m_quick, &QLineEdit::returnPressed, this, &TaskPanel::createTask);
    content->addWidget(quickFrame);

    m_search = new QLineEdit(m_middle);
    m_search->setObjectName(QStringLiteral("TaskSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索标题、说明、标签和清单条目"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(AppStyle::metric(AppStyle::Metric::ControlHeight));
    m_search->setAccessibleName(QStringLiteral("搜索任务与笔记"));
    m_search->hide();
    content->addWidget(m_search);

    m_model = new TaskListModel(m_store, this);
    m_list = new QListView(m_middle);
    m_list->setObjectName(QStringLiteral("TaskList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new TaskDelegate(m_list));
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover, true);
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(QStringLiteral("任务与笔记列表"));
    m_empty = new QLabel(QStringLiteral("这里还没有内容\n在上方输入一件要做的事或一条笔记。"), m_middle);
    m_empty->setProperty("muted", true);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_listStack = new QStackedWidget(m_middle);
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_empty);
    content->addWidget(m_listStack, 1);
    m_splitter->addWidget(m_middle);

    m_detailFrame = new QFrame(m_splitter);
    m_detailFrame->setObjectName(QStringLiteral("DetailFrame"));
    m_detailFrame->setMinimumWidth(370);
    auto* detailLayout = new QVBoxLayout(m_detailFrame);
    detailLayout->setContentsMargins(0, 36, 0, 0);
    m_detail = new TaskDetailWidget(m_store, m_detailFrame);
    detailLayout->addWidget(m_detail);
    m_splitter->addWidget(m_detailFrame);
    m_splitter->setSizes({560, 440});
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    m_splitter->setCollapsible(1, true);

    // 窗口按钮悬浮在最右列顶部；三列时属于清单，四列时属于详情。
    m_windowControls = new WindowControls(central);
    m_windowControls->setAttribute(Qt::WA_StyledBackground, false);
    m_windowControls->raise();
    connect(m_detail, &TaskDetailWidget::focusModeRequested, this,
            [this](bool enabled) {
        if (enabled) {
            m_activityRail->hide();
            m_sidebar->hide();
            m_middle->hide();
            m_detailFrame->show();
            if (m_windowControls) m_windowControls->raise();
        } else {
            m_activityRail->show();
            m_sidebar->show();
            m_middle->show();
            setDetailVisible(!m_selectedId.isEmpty());
        }
    });
    connect(m_detail, &TaskDetailWidget::taskRemoved, this, [this]() {
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
        m_count->setText(m_model->entityCount() > 0 ? QString::number(m_model->entityCount()) : QString());
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
    connect(m_search, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
    connect(m_renderer, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { applyFilter(); });
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_rebuilding || row < 0) return;
        if (!m_detail->flush()) { rebuildNavigation(); return; }
        auto* item = m_navigation->item(row);
        if (!item || item->data(NavigationDelegate::SectionRole).toBool()) return;
        m_filter.scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
        m_filter.listId = item->data(Qt::UserRole + 1).toString();
        if (!supportsTime(m_filter.scope)) {
            m_filter.renderer = ViewRenderer::List;
            const QSignalBlocker blocker(m_renderer);
            m_renderer->setCurrentIndex(0);
        }
        m_heading->setText(item->data(Qt::UserRole + 2).toString().trimmed());
        applyFilter();
    });

    auto* newShortcut = new QShortcut(QKeySequence::New, this);
    connect(newShortcut, &QShortcut::activated, this, &TaskPanel::focusQuickAdd);
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, m_railSearch, &QToolButton::click);
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, m_detail, &TaskDetailWidget::flush);
    auto* closeDetail = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    closeDetail->setContext(Qt::WindowShortcut);
    connect(closeDetail, &QShortcut::activated, this, [this]() {
        if (m_search->hasFocus() && !m_search->text().isEmpty()) {
            m_search->clear();
            return;
        }
        if (m_detail->entityId().isEmpty() || !m_detail->flush()) return;
        m_detail->setTask(QString());
        m_selectedId.clear();
        m_list->clearSelection();
        m_list->setCurrentIndex(QModelIndex());
        setDetailVisible(false);
    });

    refreshWorkspaceIcons();
    setDetailVisible(false);
    applyFilter();
}

// ========== R3: 导航重建 ==========
void TaskPanel::rebuildNavigation() {
    if (m_restoreFrozen) return;
    m_rebuilding = true; const QSignalBlocker blocker(m_navigation); m_navigation->clear();
    int selected = 0;

    auto section = [this](const QString& title) {
        auto* item = new QListWidgetItem(title, m_navigation);
        item->setFlags(Qt::NoItemFlags);
        item->setData(NavigationDelegate::SectionRole, true);
    };
    auto add = [this, &selected](const QString& label, ViewScope scope, const QString& listId = QString()) {
        ViewSpec probe; probe.scope = scope; probe.listId = listId;
        int count = 0;
        for (const Entity& entity : m_store->entities())
            if (contains(entity, probe, toAppTime(appNow()).date())) ++count;
        auto* item = new QListWidgetItem(label, m_navigation);
        item->setData(Qt::UserRole, static_cast<int>(scope));
        item->setData(Qt::UserRole + 1, listId);
        item->setData(Qt::UserRole + 2, label);
        item->setData(NavigationDelegate::CountRole, count);
        item->setData(NavigationDelegate::IconRole, static_cast<int>(scopeIcon(scope)));
        item->setToolTip(label);
        if (scope == m_filter.scope && (scope != ViewScope::List || listId == m_filter.listId))
            selected = m_navigation->count() - 1;
    };
    auto folder = [this](const Folder& row) {
        auto* item = new QListWidgetItem(row.name, m_navigation);
        item->setFlags(Qt::ItemIsEnabled);
        item->setData(NavigationDelegate::SectionRole, true);
        item->setData(FolderIdRole, row.id);
        item->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::Folder));
        item->setToolTip(QStringLiteral("文件夹：%1（右键管理）").arg(row.name));
    };

    // R3: 智能列表直接置顶，不额外占用分区标题行。
    add(QStringLiteral("全部待办"), ViewScope::AllLists);
    add(QStringLiteral("今天"), ViewScope::Today);
    add(QStringLiteral("七天内"), ViewScope::Next7Days);
    for (const TaskList& list : m_store->lists())
        if (list.inbox) add(QStringLiteral("收集箱"), ViewScope::List, list.id);

    section(QStringLiteral("清单"));
    for (const TaskList& list : m_store->lists())
        if (!list.inbox && list.folderId.isEmpty())
            add(list.name, ViewScope::List, list.id);
    for (const Folder& row : m_store->folders()) {
        folder(row);
        for (const TaskList& list : m_store->lists())
            if (!list.inbox && list.folderId == row.id)
                add(QStringLiteral("    ") + list.name, ViewScope::List, list.id);
    }

    section(QStringLiteral("历史"));
    add(QStringLiteral("已完成"), ViewScope::Completed);
    add(QStringLiteral("已放弃"), ViewScope::Abandoned);
    add(QStringLiteral("已逾期"), ViewScope::Overdue);
    add(QStringLiteral("回收站"), ViewScope::Trash);

    m_navigation->setCurrentRow(selected); m_rebuilding = false;
    m_count->setText(m_model->entityCount() > 0 ? QString::number(m_model->entityCount()) : QString());
    m_listStack->setCurrentWidget(m_model->entityCount() == 0
        ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_list));
}

void TaskPanel::applyFilter() {
    if (m_restoreFrozen) return;
    m_filter.search = m_search->text();
    m_filter.renderer = static_cast<ViewRenderer>(m_renderer->currentData().toInt());
    if (!supportsTime(m_filter.scope)) {
        m_filter.renderer = ViewRenderer::List;
        if (m_renderer->currentIndex() != 0) {
            const QSignalBlocker blocker(m_renderer); m_renderer->setCurrentIndex(0);
        }
    }
    m_renderer->setEnabled(supportsTime(m_filter.scope));
    updateQuickAddKind(); m_model->setFilter(m_filter);
}

void TaskPanel::updateQuickAddKind() {
    EntityKind kind = EntityKind::Task;
    if (m_filter.scope == ViewScope::List)
        for (const TaskList& list : m_store->lists())
            if (list.id == m_filter.listId) kind = list.kind;
    m_quickKind->setPixmap(AppIcons::icon(IconId::Add, ds::ColorToken::TextTertiary, 18).pixmap(18, 18));
    m_quick->setPlaceholderText(kind == EntityKind::Note
        ? QStringLiteral("添加笔记")
        : QStringLiteral("添加任务"));
    m_quick->setAccessibleName(kind == EntityKind::Note
        ? QStringLiteral("快速添加笔记") : QStringLiteral("快速添加任务"));
}

void TaskPanel::createTask() {
    if (m_restoreFrozen) return;
    const QString title = m_quick->text().trimmed();
    if (title.isEmpty()) { m_quick->setFocus(); return; }
    if (!m_detail->flush()) return;
    QString id;
    if (m_filter.scope == ViewScope::List) {
        EntityKind kind = EntityKind::Task;
        for (const TaskList& list : m_store->lists())
            if (list.id == m_filter.listId) kind = list.kind;
        id = kind == EntityKind::Note ? m_store->createNote(title, m_filter.listId)
                                      : m_store->createTask(title, m_filter.listId);
    } else id = m_store->createTask(title);
    if (id.isEmpty()) return;
    if (m_quickDate->isChecked()) {
        Entity created = m_store->entity(id);
        created.allDayDate = toAppTime(appNow()).date();
        if (!m_store->saveTask(created)) return;
    }
    m_quick->clear(); showTask(id); m_detail->focusTitle();
}

void TaskPanel::showTask(QString id) {
    if (m_restoreFrozen) return;
    if (!m_detail->flush()) { showNormal(); raise(); activateWindow(); return; }
    if (!id.isEmpty()) {
        const Entity entity = m_store->entity(id); if (entity.id.isEmpty()) return;
        if (!contains(entity, m_filter, toAppTime(appNow()).date())) {
            if (entity.deleted) m_filter.scope = ViewScope::Trash;
            else if (entity.kind == EntityKind::Task && entity.status == TaskStatus::Completed)
                m_filter.scope = ViewScope::Completed;
            else if (entity.kind == EntityKind::Task && entity.status == TaskStatus::Abandoned)
                m_filter.scope = ViewScope::Abandoned;
            else m_filter.scope = ViewScope::AllLists;
            m_filter.listId.clear(); m_filter.renderer = ViewRenderer::List;
            const QSignalBlocker blocker(m_renderer); m_renderer->setCurrentIndex(0);
            m_search->clear(); m_filter.search.clear();
            m_heading->setText(scopeLabel(m_filter.scope));
            applyFilter(); rebuildNavigation();
        }
        m_selectedId = id; const auto index = m_model->indexForTask(id);
        m_list->setCurrentIndex(index); m_list->scrollTo(index); m_detail->setTask(id);
        setDetailVisible(true);
    }
    showNormal(); raise(); activateWindow();
    if (m_windowControls) {
        updateWindowControlsPosition();
        m_windowControls->raise();
    }
}

void TaskPanel::focusQuickAdd() { showTask(); m_quick->setFocus(); }
bool TaskPanel::flush() { return m_restoreFrozen ? true : m_detail->finishEditing(); }
void TaskPanel::setRestoreFrozen(bool frozen) {
    m_restoreFrozen = frozen; setEnabled(!frozen);
}
void TaskPanel::resetAfterRestore() {
    setRestoreFrozen(false); m_selectedId.clear(); m_filter = ViewSpec();
    { const QSignalBlocker blocker(m_search); m_search->clear(); }
    { const QSignalBlocker blocker(m_quick); m_quick->clear(); }
    { const QSignalBlocker blocker(m_renderer); m_renderer->setCurrentIndex(0); }
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
    m_detailFrame->setVisible(visible);
    if (visible) {
        const int detailWidth = qBound(370, m_splitter->sizes().value(1), qMax(370, m_splitter->width() - 320));
        m_splitter->setSizes({qMax(320, m_splitter->width() - detailWidth - 1), detailWidth});
        QTimer::singleShot(0, this, [this]() {
            updateWindowControlsPosition();
            if (m_windowControls) m_windowControls->raise();
        });
    } else {
        m_splitter->setSizes({m_splitter->width(), 0});
    }
    if (m_windowControls) {
        updateWindowControlsPosition();
        m_windowControls->raise();
    }
}

void TaskPanel::updateWindowControlsPosition() {
    if (!m_windowControls) return;
    const int inset = (isMaximized() || isFullScreen()) ? 0 : 16;
    m_windowControls->setGeometry(width() - m_windowControls->width() - inset, inset,
                                 m_windowControls->width(), m_windowControls->height());
    if (auto* header = m_detail->findChild<QFrame*>(QStringLiteral("DetailHeader"))) {
        if (auto* layout = qobject_cast<QHBoxLayout*>(header->layout())) {
            layout->setContentsMargins(20, 6, 16, 6);
            layout->invalidate();
            layout->activate();
        }
    }
}

void TaskPanel::refreshWorkspaceIcons() {
    if (auto* account = findChild<QToolButton*>(QStringLiteral("AccountButton")))
        account->setIcon(brandMark(30));
    if (m_railTasks) m_railTasks->setIcon(AppIcons::icon(IconId::Checklist, 20));
    if (m_railSearch) m_railSearch->setIcon(AppIcons::icon(IconId::Search, 20));
    if (m_railSettings) m_railSettings->setIcon(AppIcons::icon(IconId::Settings, 20));
    if (auto* notes = findChild<QToolButton*>(QStringLiteral("RailNotes")))
        notes->setIcon(AppIcons::icon(IconId::Note, 20));
    if (auto* sync = findChild<QToolButton*>(QStringLiteral("RailSync")))
        sync->setIcon(AppIcons::icon(IconId::Sync, 18));
    if (auto* add = findChild<QToolButton*>(QStringLiteral("GroupMenuButton")))
        add->setIcon(AppIcons::icon(IconId::Add, 18));
    if (auto* backup = findChild<QToolButton*>(QStringLiteral("BackupMenuButton")))
        backup->setIcon(AppIcons::icon(IconId::Settings, 18));
    if (m_workspaceMore) m_workspaceMore->setIcon(AppIcons::icon(IconId::More, 18));
    if (auto* sort = findChild<QToolButton*>(QStringLiteral("ListSort")))
        sort->setIcon(AppIcons::icon(IconId::Sort, 18));
    if (m_quickDate) {
        m_quickDate->setIcon(AppIcons::icon(IconId::Calendar, 15));
        m_quickDate->setText(QStringLiteral("今天"));
        m_quickDate->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    if (auto* icon = findChild<QLabel*>(QStringLiteral("ListHeadingIcon")))
        icon->setPixmap(AppIcons::icon(IconId::TaskList, ds::ColorToken::Text, 18).pixmap(18, 18));
}

void TaskPanel::selectNavigation(ViewScope scope, const QString& listId) {
    for (int row = 0; row < m_navigation->count(); ++row) {
        const auto* item = m_navigation->item(row);
        if (item->data(NavigationDelegate::SectionRole).toBool()) continue;
        if (static_cast<ViewScope>(item->data(Qt::UserRole).toInt()) == scope
            && (scope != ViewScope::List || item->data(Qt::UserRole + 1).toString() == listId)) {
            m_navigation->setCurrentRow(row);
            return;
        }
    }
}

void TaskPanel::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (m_windowControls)
        updateWindowControlsPosition();
}

void TaskPanel::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        const bool maximized = isMaximized() || isFullScreen();
        if (m_surface) {
            m_surface->setFramelessCorners(!maximized);
            if (auto* layout = m_surface->layout()) {
                const int inset = maximized ? 0 : 12;
                layout->setContentsMargins(inset, inset, inset, inset);
            }
        }
        updateWindowControlsPosition();
        update();
    }
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

// ========== R6: 任务右键菜单（暂时保留现有逻辑，待 EntityCommands 集成） ==========
void TaskPanel::showTaskMenu(const QPoint& position) {
    const QString id = m_model->entityId(m_list->indexAt(position)); if (id.isEmpty()) return;
    const Entity entity = m_store->entity(id); QMenu menu(this);
    if (entity.deleted) {
        auto* restore = menu.addAction(AppIcons::icon(IconId::Restore, 16), QStringLiteral("恢复"));
        restore->setObjectName(QStringLiteral("CmdRestore"));
        connect(restore, &QAction::triggered, this, [this, id]() { m_store->restoreTask(id); });
    } else {
        auto* pin = menu.addAction(
            AppIcons::icon(IconId::Pin, 16),
            entity.pinnedAt.isValid() ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
        pin->setObjectName(QStringLiteral("CmdTogglePin"));
        connect(pin, &QAction::triggered, this, [this, id, entity]() {
            if (flush()) m_store->setPinnedAt(id, entity.pinnedAt.isValid() ? QDateTime() : appNow());
        });
        if (entity.kind == EntityKind::Task) {
            auto* done = menu.addAction(
                AppIcons::icon(IconId::Check, 16),
                entity.completed ? QStringLiteral("设为未完成") : QStringLiteral("完成任务"));
            done->setObjectName(QStringLiteral("CmdToggleComplete"));
            connect(done, &QAction::triggered, this, [this, id, entity]() {
                if (flush()) m_store->setCompleted(id, !entity.completed);
            });
        }
        const EntityKind target = entity.kind == EntityKind::Task ? EntityKind::Note : EntityKind::Task;
        auto* convert = menu.addAction(
            AppIcons::icon(IconId::Convert, 16),
            target == EntityKind::Note ? QStringLiteral("转为笔记") : QStringLiteral("转为任务"));
        convert->setObjectName(QStringLiteral("CmdConvertKind"));
        connect(convert, &QAction::triggered, this, [this, id, target]() {
            if (!flush()) return;
            const QString text = target == EntityKind::Note
                ? QStringLiteral("完成状态、日期、提醒、重复、优先级和贴桌面行为将暂停；字段、提醒、附件与清单均原样保留，转回任务后恢复。是否继续？")
                : QStringLiteral("将恢复任务状态、日期、提醒、重复、优先级及贴桌面行为；字段、提醒、附件与清单均已保留。是否继续？");
            if (QMessageBox::question(this, QStringLiteral("类型转换"), text,
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes
                && m_store->convertKind(id, target)) showTask(id);
        });
        if (entity.kind == EntityKind::Task) {
            const bool desktopOpen = m_store->desktopNote(id).open;
            auto* desktop = menu.addAction(
                AppIcons::icon(IconId::DesktopNote, 16),
                desktopOpen ? QStringLiteral("关闭桌面便签") : QStringLiteral("贴到桌面"));
            desktop->setObjectName(QStringLiteral("CmdDesktopNote"));
            connect(desktop, &QAction::triggered, this, [this, id, desktopOpen]() {
                if (!flush()) return;
                const Entity current = m_store->entity(id);
                m_store->saveDesktopState(id, current.color, current.geometry,
                                          current.zOrder, current.layer, !desktopOpen);
            });
        }
        auto* clone = menu.addAction(
            AppIcons::icon(IconId::Add, 16),
            entity.kind == EntityKind::Note ? QStringLiteral("复制为新笔记") : QStringLiteral("复制为新任务"));
        clone->setObjectName(QStringLiteral("CmdClone"));
        connect(clone, &QAction::triggered, this, [this, id]() {
            if (!flush()) return;
            const QString newId = m_store->cloneTask(id);
            if (!newId.isEmpty()) showTask(newId);
        });
        auto* abandon = menu.addAction(AppIcons::icon(IconId::Abandoned, 16), QStringLiteral("放弃任务"));
        abandon->setObjectName(QStringLiteral("CmdAbandon"));
        abandon->setEnabled(entity.status == TaskStatus::Active);
        connect(abandon, &QAction::triggered, this, [this, id]() {
            if (flush()) m_store->abandonTask(id);
        });
        menu.addSeparator();
        auto* trash = menu.addAction(AppIcons::icon(IconId::Trash, 16), QStringLiteral("移到回收站"));
        trash->setObjectName(QStringLiteral("CmdTrash"));
        connect(trash, &QAction::triggered, this, [this, id]() {
            if (flush()) m_store->trashTask(id);
        });
    }
    menu.exec(m_list->viewport()->mapToGlobal(position));
}

// ========== R3: 文件夹与清单管理菜单 ==========
void TaskPanel::groupMenu() {
    QMenu menu(this);
    auto* newFolder = menu.addAction(AppIcons::icon(IconId::Folder, 16), QStringLiteral("新建文件夹…"));
    newFolder->setObjectName(QStringLiteral("CmdNewFolder"));
    connect(newFolder, &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
            QStringLiteral("名称："), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty()) m_store->createFolder(name.trimmed());
    });
    const auto createList = [this, &menu](const QString& label, EntityKind kind) {
        auto* action = menu.addAction(
            AppIcons::icon(kind == EntityKind::Note ? IconId::Note : IconId::TaskList, 16), label);
        action->setObjectName(kind == EntityKind::Note ? QStringLiteral("CmdNewNoteList")
                                                       : QStringLiteral("CmdNewTaskList"));
        connect(action, &QAction::triggered, this, [this, kind]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this,
                kind == EntityKind::Note ? QStringLiteral("新建笔记清单") : QStringLiteral("新建任务清单"),
                QStringLiteral("名称："), QLineEdit::Normal, QString(), &ok);
            if (ok && !name.trimmed().isEmpty()) m_store->createList(name.trimmed(), QString(), kind);
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
    const QString folderId = item->data(FolderIdRole).toString();
    const QString listId = item->data(Qt::UserRole + 1).toString();
    if (folderId.isEmpty() && listId.isEmpty()) return;
    QMenu menu(this);
    if (!folderId.isEmpty()) {
        const auto createInFolder = [this, &menu, folderId](const QString& label, EntityKind kind) {
            auto* action = menu.addAction(label);
            connect(action, &QAction::triggered, this, [this, folderId, kind, label]() {
                bool ok = false;
                const QString name = QInputDialog::getText(this, label, QStringLiteral("清单名称："),
                    QLineEdit::Normal, QString(), &ok);
                if (ok && !name.trimmed().isEmpty()) m_store->createList(name.trimmed(), folderId, kind);
            });
        };
        createInFolder(QStringLiteral("在此新建任务清单…"), EntityKind::Task);
        createInFolder(QStringLiteral("在此新建笔记清单…"), EntityKind::Note);
        menu.addSeparator();
        connect(menu.addAction(QStringLiteral("重命名文件夹…")), &QAction::triggered, this, [this, folderId]() {
            QString current;
            for (const Folder& folder : m_store->folders()) if (folder.id == folderId) current = folder.name;
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("重命名文件夹"),
                QStringLiteral("新名称："), QLineEdit::Normal, current, &ok);
            if (ok && !name.trimmed().isEmpty()) m_store->renameFolder(folderId, name.trimmed());
        });
        connect(menu.addAction(QStringLiteral("删除文件夹…")), &QAction::triggered, this, [this, folderId]() {
            if (QMessageBox::question(this, QStringLiteral("删除文件夹"),
                    QStringLiteral("删除后，其中清单会平铺显示，清单和实体内容均保留。是否继续？"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                m_store->deleteFolder(folderId);
        });
    } else {
        TaskList selected;
        for (const TaskList& list : m_store->lists()) if (list.id == listId) selected = list;
        if (selected.inbox) {
            auto* protectedAction = menu.addAction(QStringLiteral("系统收集箱不可移动或删除"));
            protectedAction->setEnabled(false);
        } else {
            connect(menu.addAction(QStringLiteral("重命名清单…")), &QAction::triggered, this, [this, selected]() {
                bool ok = false;
                const QString name = QInputDialog::getText(this, QStringLiteral("重命名清单"),
                    QStringLiteral("新名称："), QLineEdit::Normal, selected.name, &ok);
                if (ok && !name.trimmed().isEmpty()) m_store->renameList(selected.id, name.trimmed());
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
                if (QMessageBox::question(this, QStringLiteral("删除清单"),
                        QStringLiteral("删除后，其中实体将移回系统收集箱，内容不会删除。是否继续？"),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
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

// ========== R10: 备份与设置菜单 ==========
void TaskPanel::backupMenu() {
    QMenu menu(this);
    auto* exportAction = menu.addAction(AppIcons::icon(IconId::Backup, 16), QStringLiteral("导出备份…"));
    exportAction->setObjectName(QStringLiteral("CmdExportBackup"));
    connect(exportAction, &QAction::triggered, this, [this]() {
        if (!flush()) return;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出备份"),
            QStringLiteral("KylinList-%1.klb").arg(toAppTime(appNow()).date().toString(Qt::ISODate)),
            QStringLiteral("麒麟清单备份（含附件） (*.klb)"));
        if (!path.isEmpty() && m_store->exportBackup(path))
            showNotice(QStringLiteral("备份已保存：") + path);
    });
    auto* importAction = menu.addAction(AppIcons::icon(IconId::Restore, 16), QStringLiteral("导入备份…"));
    importAction->setObjectName(QStringLiteral("CmdImportBackup"));
    connect(importAction, &QAction::triggered, this, [this]() {
        if (!flush()) return;
        const QString path = QFileDialog::getOpenFileName(this,
            QStringLiteral("导入备份（覆盖本机数据）"), QString(),
            QStringLiteral("麒麟清单备份 (*.klb)"));
        if (path.isEmpty()) return;
        if (QMessageBox::warning(this, QStringLiteral("确认覆盖恢复"),
                QStringLiteral("本机现有全部任务、笔记与清单都会被删除。建议先导出当前备份。\n\n此操作无法撤销，是否继续？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            emit importBackupRequested(path);
    });
    menu.addSeparator();

    // R10: 主题切换
    auto* themeMenu = menu.addMenu(AppIcons::icon(IconId::Theme, 16), QStringLiteral("主题"));
    themeMenu->setObjectName(QStringLiteral("ThemeMenu"));
    auto* lightAction = themeMenu->addAction(QStringLiteral("浅色"));
    lightAction->setCheckable(true);
    lightAction->setChecked(ThemeManager::instance().preference() == ThemeManager::Preference::Light);
    lightAction->setObjectName(QStringLiteral("ThemeLight"));
    connect(lightAction, &QAction::triggered, this, []() {
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
    });
    auto* darkAction = themeMenu->addAction(QStringLiteral("深色"));
    darkAction->setCheckable(true);
    darkAction->setChecked(ThemeManager::instance().preference() == ThemeManager::Preference::Dark);
    darkAction->setObjectName(QStringLiteral("ThemeDark"));
    connect(darkAction, &QAction::triggered, this, []() {
        ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
    });
    auto* systemAction = themeMenu->addAction(QStringLiteral("跟随系统"));
    systemAction->setCheckable(true);
    systemAction->setChecked(ThemeManager::instance().preference() == ThemeManager::Preference::System);
    systemAction->setObjectName(QStringLiteral("ThemeSystem"));
    connect(systemAction, &QAction::triggered, this, []() {
        ThemeManager::instance().setPreference(ThemeManager::Preference::System);
    });
    menu.addSeparator();

    auto* startup = menu.addAction(QStringLiteral("登录时自动启动"));
    startup->setCheckable(true);
    startup->setChecked(Autostart::isInstalled());
    startup->setObjectName(QStringLiteral("CmdAutostart"));
    connect(startup, &QAction::triggered, this, [this](bool enabled) {
        QString error;
        if (!Autostart::setEnabled(enabled, &error)) showError(error);
    });
    auto* openData = menu.addAction(QStringLiteral("打开数据位置"));
    openData->setObjectName(QStringLiteral("CmdOpenData"));
    connect(openData, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataDirPath()));
    });
    menu.addSeparator();
    auto* quit = menu.addAction(QStringLiteral("退出麒麟清单"));
    quit->setObjectName(QStringLiteral("CmdQuit"));
    connect(quit, &QAction::triggered, this, [this]() { if (flush()) emit quitRequested(); });
    menu.exec(QCursor::pos());
}

// ========== R4: 工作区更多菜单（只管「怎么看」，考据 m04/R12） ==========
void TaskPanel::workspaceMenu() {
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("WorkspaceMenu"));

    // 视图切换（考据 m04：视图图标条）
    auto* viewSection = menu.addAction(QStringLiteral("视图"));
    viewSection->setEnabled(false);
    auto* listView = menu.addAction(AppIcons::icon(IconId::List, 16), QStringLiteral("清单视图"));
    listView->setCheckable(true);
    listView->setChecked(m_renderer->currentData().toInt() == static_cast<int>(ViewRenderer::List));
    listView->setObjectName(QStringLiteral("ViewList"));
    connect(listView, &QAction::triggered, this, [this]() {
        m_renderer->setCurrentIndex(0);
    });
    auto* timeView = menu.addAction(AppIcons::icon(IconId::Calendar, 16), QStringLiteral("时间视图"));
    timeView->setCheckable(true);
    timeView->setChecked(m_renderer->currentData().toInt() == static_cast<int>(ViewRenderer::Time));
    timeView->setEnabled(supportsTime(m_filter.scope));
    timeView->setObjectName(QStringLiteral("ViewTime"));
    connect(timeView, &QAction::triggered, this, [this]() {
        if (supportsTime(m_filter.scope)) m_renderer->setCurrentIndex(1);
    });
    menu.addSeparator();

    // 显示设置（考据 R12：隐藏已完成、显示详细、显示设置）
    auto* hideCompleted = menu.addAction(QStringLiteral("隐藏已完成"));
    hideCompleted->setCheckable(true);
    hideCompleted->setObjectName(QStringLiteral("HideCompleted"));
    // TODO: 接入 ViewSpec 的 hideCompleted 字段（如果模型支持）
    menu.addSeparator();

    // 显示设置子菜单
    auto* displaySettings = menu.addMenu(QStringLiteral("显示设置"));
    displaySettings->setObjectName(QStringLiteral("DisplaySettings"));
    auto* dateDisplay = displaySettings->addAction(QStringLiteral("日期显示"));
    dateDisplay->setObjectName(QStringLiteral("DateDisplay"));
    // TODO: 接入显示设置

    menu.exec(QCursor::pos());
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
        if (msg->message == WM_NCHITTEST && !isMaximized()) {
            const qreal scale = devicePixelRatioF();
            RECT frame;
            GetWindowRect(msg->hwnd, &frame);
            const int x = int((GET_X_LPARAM(msg->lParam) - frame.left) / scale);
            const int y = int((GET_Y_LPARAM(msg->lParam) - frame.top) / scale);
            const int w = int((frame.right - frame.left) / scale);
            const int h = int((frame.bottom - frame.top) / scale);
            const int border = 6;
            const bool left = x < border;
            const bool right = x >= w - border;
            const bool top = y < border;
            const bool bottom = y >= h - border;
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
    settings.setValue(QStringLiteral("splitter"), m_splitter->saveState());
    if (QSystemTrayIcon::isSystemTrayAvailable()) { hide(); event->ignore(); }
    else { event->accept(); emit quitRequested(); }
}
}  // namespace kl
