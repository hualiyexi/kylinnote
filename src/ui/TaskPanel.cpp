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
#include <QPainter>
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
#include "design/ThemeManager.h"

namespace kl {
namespace {
const int FolderIdRole = Qt::UserRole + 6;

QPixmap brandMark(int size) {
    return AppIcons::icon(IconId::TaskList, ds::ColorToken::Primary, size)
        .pixmap(size, size);
}

QPixmap kindIcon(EntityKind kind) {
    return AppIcons::icon(kind == EntityKind::Note ? IconId::Note : IconId::TaskList,
                          ds::ColorToken::TextSecondary, 24)
        .pixmap(24, 24);
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
    setWindowTitle(QStringLiteral("麒麟清单 · 我的待办"));
    setMinimumSize(960, 620); resize(1240, 820);
    buildUi();
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
    connect(store, &TaskStore::taskChanged, this, [this]() {
        m_error->hide(); if (QWidget* host = m_error->parentWidget()) host->hide();
        rebuildNavigation();
    });
    connect(store, &TaskStore::desktopNoteChanged, this, [this]() { rebuildNavigation(); });
    connect(store, &TaskStore::groupsChanged, this, &TaskPanel::rebuildNavigation);
    connect(store, &TaskStore::tasksReset, this, &TaskPanel::rebuildNavigation);
    connect(store, &TaskStore::errorOccurred, this, &TaskPanel::showError);
    auto* clock = new QTimer(this); clock->setInterval(60000);
    connect(clock, &QTimer::timeout, m_model, &TaskListModel::refresh); clock->start();
    // 主题切换时刷新品牌图标和导航
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        rebuildNavigation();
    });
}

void TaskPanel::buildUi() {
    auto* central = new QWidget(this); setCentralWidget(central);
    auto* root = new QVBoxLayout(central); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);

    // 错误横幅
    auto* errorHost = new QWidget(this); auto* errorLayout = new QHBoxLayout(errorHost);
    errorLayout->setContentsMargins(24, 12, 24, 0);
    m_error = new QLabel(errorHost); m_error->setTextFormat(Qt::PlainText);
    m_error->setObjectName(QStringLiteral("ErrorBanner"));
    m_error->setWordWrap(true); m_error->setTextInteractionFlags(Qt::TextSelectableByMouse);
    errorLayout->addWidget(m_error); errorHost->hide(); root->addWidget(errorHost);

    auto* body = new QHBoxLayout; body->setContentsMargins(0, 0, 0, 0); body->setSpacing(0);
    root->addLayout(body, 1);

    // ========== R3: 左侧导航与组织 ==========
    auto* sidebar = new QFrame(this); sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setFixedWidth(AppStyle::metric(AppStyle::Metric::SidebarWidth));
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(8, 14, 8, 8); side->setSpacing(2);

    // 品牌行
    auto* brandRow = new QHBoxLayout; brandRow->setContentsMargins(8, 4, 8, 14); brandRow->setSpacing(8);
    auto* mark = new QLabel(sidebar); mark->setFixedSize(22, 22); mark->setPixmap(brandMark(22));
    brandRow->addWidget(mark);
    auto* brand = new QLabel(QStringLiteral("麒麟清单"), sidebar);
    brand->setFont(AppStyle::font(AppStyle::Font::Brand));
    brandRow->addWidget(brand); brandRow->addStretch(1); side->addLayout(brandRow);

    // 导航列表
    m_navigation = new QListWidget(sidebar); m_navigation->setObjectName(QStringLiteral("Navigation"));
    m_navigation->setItemDelegate(new NavigationDelegate(m_navigation)); m_navigation->setMouseTracking(true);
    m_navigation->viewport()->setAttribute(Qt::WA_Hover, true);
    m_navigation->setAccessibleName(QStringLiteral("任务导航"));
    m_navigation->setContextMenuPolicy(Qt::CustomContextMenu);
    side->addWidget(m_navigation, 1);

    // R3: 底部按钮改为图标+溢出菜单
    auto* bottomRow = new QHBoxLayout; bottomRow->setSpacing(4);
    auto* groups = new QToolButton(sidebar);
    groups->setIcon(AppIcons::icon(IconId::Folder, 18));
    groups->setToolTip(QStringLiteral("文件夹与清单"));
    groups->setAccessibleName(QStringLiteral("文件夹与清单管理"));
    groups->setObjectName(QStringLiteral("GroupMenuButton"));
    groups->setAutoRaise(true); groups->setCursor(Qt::PointingHandCursor);
    groups->setFixedSize(36, 36);
    connect(groups, &QToolButton::clicked, this, &TaskPanel::groupMenu);
    bottomRow->addWidget(groups);

    auto* backup = new QToolButton(sidebar);
    backup->setIcon(AppIcons::icon(IconId::Settings, 18));
    backup->setToolTip(QStringLiteral("备份与设置"));
    backup->setAccessibleName(QStringLiteral("备份与设置"));
    backup->setObjectName(QStringLiteral("BackupMenuButton"));
    backup->setAutoRaise(true); backup->setCursor(Qt::PointingHandCursor);
    backup->setFixedSize(36, 36);
    connect(backup, &QToolButton::clicked, this, &TaskPanel::backupMenu);
    bottomRow->addWidget(backup);
    bottomRow->addStretch(1);
    side->addLayout(bottomRow);

    body->addWidget(sidebar);

    // ========== 中间列表区 ==========
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("WorkspaceSplitter"));
    m_splitter->setHandleWidth(1);
    body->addWidget(m_splitter, 1);
    auto* middle = new QFrame(this); middle->setObjectName(QStringLiteral("ListPane"));
    middle->setMinimumWidth(300);
    auto* center = new QVBoxLayout(middle);
    const int margin = AppStyle::metric(AppStyle::Metric::ContentMargin);
    center->setContentsMargins(margin, 16, margin, 8); center->setSpacing(10);

    // R4: 列表头部 —— 标题 + 计数 + 视图操作收进 overflow
    auto* header = new QHBoxLayout; header->setSpacing(10);
    m_heading = new QLabel(QStringLiteral("全部待办"), middle);
    m_heading->setFont(AppStyle::font(AppStyle::Font::Heading));
    header->addWidget(m_heading, 0, Qt::AlignBottom);
    m_count = new QLabel(middle); m_count->setProperty("muted", true);
    m_count->setFont(AppStyle::font(AppStyle::Font::Small));
    m_count->setContentsMargins(0, 0, 0, 5);
    header->addWidget(m_count, 0, Qt::AlignBottom); header->addStretch(1);

    // R4: 视图切换收进 overflow 菜单（考据 m04：工作区更多只管「怎么看」）
    m_renderer = new QComboBox(middle); m_renderer->setObjectName(QStringLiteral("ViewRenderer"));
    m_renderer->setAccessibleName(QStringLiteral("视图呈现方式"));
    m_renderer->addItem(QStringLiteral("清单视图"), static_cast<int>(ViewRenderer::List));
    m_renderer->addItem(QStringLiteral("时间视图"), static_cast<int>(ViewRenderer::Time));
    m_renderer->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_renderer->setMinimumWidth(112); m_renderer->setFixedHeight(32);
    header->addWidget(m_renderer, 0, Qt::AlignVCenter);

    // 工作区更多按钮（考据 m04）
    m_workspaceMore = new QToolButton(middle);
    m_workspaceMore->setIcon(AppIcons::icon(IconId::More, 18));
    m_workspaceMore->setToolTip(QStringLiteral("视图选项"));
    m_workspaceMore->setAccessibleName(QStringLiteral("工作区视图选项"));
    m_workspaceMore->setObjectName(QStringLiteral("WorkspaceMore"));
    m_workspaceMore->setAutoRaise(true); m_workspaceMore->setFixedSize(32, 32);
    m_workspaceMore->setCursor(Qt::PointingHandCursor);
    connect(m_workspaceMore, &QToolButton::clicked, this, &TaskPanel::workspaceMenu);
    header->addWidget(m_workspaceMore, 0, Qt::AlignVCenter);
    center->addLayout(header);

    // R4: 搜索框
    m_search = new QLineEdit(middle); m_search->setObjectName(QStringLiteral("TaskSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索标题、说明、标签和清单条目  Ctrl+F"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedHeight(AppStyle::metric(AppStyle::Metric::ControlHeight));
    m_search->setAccessibleName(QStringLiteral("搜索任务与笔记"));
    center->addWidget(m_search);

    // R4: 快速添加（固定入口，明确落点）
    auto* quick = new QHBoxLayout; quick->setSpacing(8);
    m_quickKind = new QLabel(middle); m_quickKind->setFixedSize(24, 24); quick->addWidget(m_quickKind);
    m_quick = new QLineEdit(middle); m_quick->setObjectName(QStringLiteral("QuickAdd"));
    m_quick->setMaxLength(500);
    m_quick->setFixedHeight(AppStyle::metric(AppStyle::Metric::ControlHeight));
    auto* add = new QPushButton(QStringLiteral("添加"), middle); add->setProperty("primary", true);
    add->setObjectName(QStringLiteral("QuickAddButton")); add->setFixedHeight(32); add->setMinimumWidth(68);
    add->setCursor(Qt::PointingHandCursor);
    quick->addWidget(m_quick, 1); quick->addWidget(add); center->addLayout(quick);
    connect(add, &QPushButton::clicked, this, &TaskPanel::createTask);
    connect(m_quick, &QLineEdit::returnPressed, this, &TaskPanel::createTask);

    // 任务列表
    m_model = new TaskListModel(m_store, this);
    m_list = new QListView(middle); m_list->setObjectName(QStringLiteral("TaskList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new TaskDelegate(m_list)); m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover, true); m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setAccessibleName(QStringLiteral("任务与笔记列表"));
    m_empty = new QLabel(QStringLiteral("这里还没有内容\n在上方输入一件要做的事或一条笔记。"), middle);
    m_empty->setProperty("muted", true); m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    m_listStack = new QStackedWidget(middle);
    m_listStack->addWidget(m_list); m_listStack->addWidget(m_empty);
    center->addWidget(m_listStack, 1); m_splitter->addWidget(middle);

    // ========== 右侧详情区 ==========
    auto* detailFrame = new QFrame(this); detailFrame->setObjectName(QStringLiteral("DetailFrame"));
    detailFrame->setMinimumWidth(370);
    auto* detailLayout = new QVBoxLayout(detailFrame); detailLayout->setContentsMargins(0, 0, 0, 0);
    m_detail = new TaskDetailWidget(m_store, detailFrame); detailLayout->addWidget(m_detail);
    m_splitter->addWidget(detailFrame); m_splitter->setSizes({560, 440});
    m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    connect(m_detail, &TaskDetailWidget::focusModeRequested, this,
            [this, sidebar, middle, normalSizes = QList<int>()](bool enabled) mutable {
        if (enabled) normalSizes = m_splitter->sizes();
        sidebar->setVisible(!enabled);
        middle->setVisible(!enabled);
        if (!enabled && !normalSizes.isEmpty()) m_splitter->setSizes(normalSizes);
    });

    // ========== 信号连接 ==========
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() { m_resetting = true; });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
        const QModelIndex index = m_model->indexForTask(m_selectedId);
        if (index.isValid()) m_list->setCurrentIndex(index);
        m_count->setText(QStringLiteral("%1 项").arg(m_model->entityCount()));
        m_listStack->setCurrentWidget(m_model->entityCount() == 0
            ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_list));
        m_resetting = false;
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current) {
                if (m_resetting) return;
                const QString id = m_model->entityId(current);
                if (m_detail->setTask(id)) m_selectedId = id;
                else {
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
        auto* item = m_navigation->item(row); if (!item) return;
        if (item->data(NavigationDelegate::SectionRole).toBool()) return;
        m_filter.scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
        m_filter.listId = item->data(Qt::UserRole + 1).toString();
        if (!supportsTime(m_filter.scope)) {
            m_filter.renderer = ViewRenderer::List;
            const QSignalBlocker blocker(m_renderer); m_renderer->setCurrentIndex(0);
        }
        m_heading->setText(item->data(Qt::UserRole + 2).toString());
        applyFilter();
    });

    // 快捷键
    auto* newShortcut = new QShortcut(QKeySequence::New, this);
    connect(newShortcut, &QShortcut::activated, this, &TaskPanel::focusQuickAdd);
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    connect(findShortcut, &QShortcut::activated, m_search, QOverload<>::of(&QWidget::setFocus));
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, m_detail, &TaskDetailWidget::flush);

    statusBar()->setSizeGripEnabled(false);
    statusBar()->setFont(AppStyle::font(AppStyle::Font::Small));
    statusBar()->showMessage(QStringLiteral("本机保存 · 无需联网"));
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

    // R3: 智能列表分区
    section(QStringLiteral("智能列表"));
    add(QStringLiteral("全部待办"), ViewScope::AllLists);
    add(QStringLiteral("今天"), ViewScope::Today);
    add(QStringLiteral("七天内"), ViewScope::Next7Days);
    add(QStringLiteral("已逾期"), ViewScope::Overdue);

    // R3: 历史与回收站分区（考据：历史与恢复操作不混在常规清单里）
    section(QStringLiteral("历史"));
    add(QStringLiteral("已完成"), ViewScope::Completed);
    add(QStringLiteral("已放弃"), ViewScope::Abandoned);
    add(QStringLiteral("回收站"), ViewScope::Trash);

    // R3: 文件夹与清单分区（单层文件夹 + 可平铺清单 + 清单 kind）
    section(QStringLiteral("文件夹与清单"));
    for (const TaskList& list : m_store->lists())
        if (list.inbox) add(QStringLiteral("收集箱"), ViewScope::List, list.id);
    for (const TaskList& list : m_store->lists())
        if (!list.inbox && list.folderId.isEmpty())
            add(list.name, ViewScope::List, list.id);
    for (const Folder& row : m_store->folders()) {
        folder(row);
        for (const TaskList& list : m_store->lists())
            if (!list.inbox && list.folderId == row.id)
                add(QStringLiteral("    ") + list.name, ViewScope::List, list.id);
    }

    m_navigation->setCurrentRow(selected); m_rebuilding = false;
    m_count->setText(QStringLiteral("%1 项").arg(m_model->entityCount()));
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
    m_quickKind->setPixmap(kindIcon(kind));
    m_quick->setPlaceholderText(kind == EntityKind::Note
        ? QStringLiteral("添加一条笔记，按回车确认")
        : QStringLiteral("添加一项待办，按回车确认"));
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
    }
    showNormal(); raise(); activateWindow();
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
    statusBar()->showMessage(QStringLiteral("覆盖恢复完成"), 8000);
}
void TaskPanel::showError(const QString& message) {
    m_error->setText(message); m_error->show();
    if (QWidget* host = m_error->parentWidget()) host->show();
    statusBar()->showMessage(message);
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
            statusBar()->showMessage(QStringLiteral("备份已保存：") + path, 10000);
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

void TaskPanel::closeEvent(QCloseEvent* event) {
    if (!flush()) { event->ignore(); return; }
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("splitter"), m_splitter->saveState());
    if (QSystemTrayIcon::isSystemTrayAvailable()) { hide(); event->ignore(); }
    else { event->accept(); emit quitRequested(); }
}
}  // namespace kl
