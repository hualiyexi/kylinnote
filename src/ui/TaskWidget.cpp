#include "TaskWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDebug>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <memory>

#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "ModalDialog.h"
#include "EntityCommands.h"
#include "PopupMenu.h"
#include "PopupShadow.h"
#include "TaskDateController.h"
#include "TaskDelegate.h"
#include "TaskListModel.h"
#include "TaskListView.h"
#include "WindowChrome.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"
#include "platform/ScreenFit.h"
#include "platform/SystemShadow.h"
#include "NoteSnap.h"

namespace kl {
namespace {

constexpr int kPaperRadius = 10;
constexpr int kMinimumPaperWidth = 260;
constexpr int kMinimumPaperHeight = 240;
constexpr int kDefaultPaperWidth = 340;
constexpr int kDefaultPaperHeight = 546;

std::unique_ptr<QSettings> widgetSettings()
{
    // 与 TaskPanel 相同：main.cpp 在隔离运行时把 IniFormat/UserScope 重定向到
    // 当前 --data-dir，因此开发预览不会读写真实用户设置。
    return std::unique_ptr<QSettings>(new QSettings(QSettings::IniFormat, QSettings::UserScope,
        QStringLiteral("kylinlist"), QStringLiteral("task-widget")));
}

QIcon themedIcon(IconId id, ds::Theme theme, int size = 18,
                 ds::ColorToken token = ds::ColorToken::TextSecondary)
{
    return AppIcons::icon(AppIcons::iconName(id), ds::color(token, theme), size);
}

void popupAt(PopupMenu* menu, QWidget* anchor)
{
    const QSize card = menu->sizeHint() - QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin);
    const QRect target(anchor->mapToGlobal(QPoint()), anchor->size());
    QScreen* screen = QGuiApplication::screenAt(target.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry()
                                   : QRect(target.topLeft(), QSize(1920, 1080));
    int x = target.right() + 1 - card.width();
    int y = target.bottom() + 4;
    if (y + card.height() > available.bottom() + 1)
        y = target.top() - card.height() - 4;
    x = qBound(available.left(), x, qMax(available.left(), available.right() + 1 - card.width()));
    y = qBound(available.top(), y, qMax(available.top(), available.bottom() + 1 - card.height()));
    menu->popup(QPoint(x, y));
}

// 顶栏空白处的拖动区；子按钮仍独立处理点击。锁定只拦截窗口移动，
// 不拦截任务编辑、完成或列表内部拖放。
class TaskWidgetDragArea final : public QWidget {
public:
    explicit TaskWidgetDragArea(TaskWidget* window) : QWidget(window), m_window(window) {}

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_window->canDragWindow()) {
            m_dragging = true;
            m_pressGlobal = event->globalPos();
            m_windowPosition = m_window->pos();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging && (event->buttons() & Qt::LeftButton) && m_window->canDragWindow()) {
            m_window->move(m_window->constrainedWindowPosition(
                m_windowPosition + event->globalPos() - m_pressGlobal));
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (m_dragging && event->button() == Qt::LeftButton) {
            m_dragging = false;
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    TaskWidget* m_window;
    QPoint m_pressGlobal;
    QPoint m_windowPosition;
    bool m_dragging = false;
};

}  // namespace

TaskWidget::TaskWidget(TaskStore* store, QWidget* parent)
    : QWidget(parent), m_store(store)
{
    setObjectName(QStringLiteral("TaskWidget"));
    setWindowTitle(QStringLiteral("任务小部件"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumSize(minimumPaperSize() + QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin));

    m_filter.tasksOnly = true;
    m_filter.showCompleted = true;  // 参考小部件默认带“已完成 & 已放弃”分组
    m_filter.grouping = GroupMode::Time;

    const auto settings = widgetSettings();
    m_open = settings->value(QStringLiteral("open"), false).toBool();
    m_locked = windowLockEnabled() && settings->value(QStringLiteral("locked"), false).toBool();
    m_appearance = qBound(0, settings->value(QStringLiteral("appearance"), 2).toInt(), 2);
    m_filter.showCompleted = settings->value(QStringLiteral("showCompleted"), true).toBool();
    m_filter.showDetails = settings->value(QStringLiteral("showDetails"), false).toBool();
    m_filter.countdownDates = settings->value(QStringLiteral("countdownDates"), false).toBool();
    m_filter.scope = static_cast<ViewScope>(settings->value(QStringLiteral("scope"),
        static_cast<int>(ViewScope::List)).toInt());
    m_filter.listId = settings->value(QStringLiteral("listId")).toString();
    if (m_filter.scope != ViewScope::List && m_filter.scope != ViewScope::Today
        && m_filter.scope != ViewScope::Next7Days && m_filter.scope != ViewScope::AllLists) {
        m_filter.scope = ViewScope::List;
    }
    if (m_filter.scope == ViewScope::List && m_filter.listId.isEmpty() && m_store)
        m_filter.listId = m_store->inboxListId();
    m_filter.grouping = groupModeFromName(settings->value(QStringLiteral("grouping")).toString(),
                                          GroupMode::Time);

    buildUi();
    applyFilter();
    applyAppearance();

    const QRect defaultPaper(QPoint(100, 100), QSize(kDefaultPaperWidth, kDefaultPaperHeight));
    const QRect defaultWindow = defaultPaper.adjusted(-PopupShadowMargin, -PopupShadowMargin,
                                                       PopupShadowMargin, PopupShadowMargin);
    const QRect saved = settings->value(QStringLiteral("geometry"), defaultWindow).toRect();
    setGeometry(ScreenFit::fitPaper(saved, PopupShadowMargin, minimumPaperSize()));
    m_observedGeometry = geometry();
    syncChromeGeometry();
    m_loading = false;

    m_geometryTimer.setSingleShot(true);
    m_geometryTimer.setInterval(250);
    connect(&m_geometryTimer, &QTimer::timeout, this, &TaskWidget::saveState);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        applyAppearance();
    });
    auto* clock = new QTimer(this);
    clock->setInterval(60000);
    connect(clock, &QTimer::timeout, this, [this]() {
        if (isVisible()) m_model->refresh();
    });
    clock->start();
}

TaskWidget::~TaskWidget()
{
    m_geometryTimer.stop();
    if (m_menu) {
        disconnect(m_menu, nullptr, this, nullptr);
        m_menu->hide();
    }
    disconnect(&ThemeManager::instance(), nullptr, this, nullptr);
}

QSize TaskWidget::minimumPaperSize()
{
    return QSize(kMinimumPaperWidth, kMinimumPaperHeight);
}

QRect TaskWidget::paperGeometry() const
{
    return geometry().adjusted(PopupShadowMargin, PopupShadowMargin,
                               -PopupShadowMargin, -PopupShadowMargin);
}

void TaskWidget::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(PopupShadowMargin, PopupShadowMargin,
                             PopupShadowMargin, PopupShadowMargin);
    root->setSpacing(0);
    m_paper = new QWidget(this);
    m_paper->setObjectName(QStringLiteral("TaskWidgetPaper"));
    auto* paper = new QVBoxLayout(m_paper);
    paper->setContentsMargins(0, 0, 0, 0);
    paper->setSpacing(0);
    root->addWidget(m_paper);

    m_header = new TaskWidgetDragArea(this);
    m_header->setObjectName(QStringLiteral("TaskWidgetHeader"));
    m_header->setFixedHeight(46);
    auto* header = new QHBoxLayout(m_header);
    header->setContentsMargins(16, 0, 10, 0);
    header->setSpacing(4);
    m_scope = new QToolButton(m_header);
    m_scope->setObjectName(QStringLiteral("WidgetScope"));
    m_scope->setAccessibleName(QStringLiteral("选择小部件清单"));
    m_scope->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_scope->setLayoutDirection(Qt::RightToLeft);  // 文本在前，下拉箭头在后
    m_scope->setFont(ds::qfont(ds::FontScene::Section));
    m_scope->setCursor(Qt::PointingHandCursor);
    m_scope->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    header->addWidget(m_scope, 0, Qt::AlignVCenter);
    header->addStretch(1);
    m_more = new QToolButton(m_header);
    m_more->setObjectName(QStringLiteral("WidgetMore"));
    m_more->setAccessibleName(QStringLiteral("小部件选项"));
    m_more->setToolTip(QStringLiteral("选项"));
    m_more->setFixedSize(28, 28);
    m_more->setIconSize(QSize(18, 18));
    m_more->setCursor(Qt::PointingHandCursor);
    header->addWidget(m_more, 0, Qt::AlignVCenter);
    paper->addWidget(m_header);
    connect(m_scope, &QToolButton::clicked, this, &TaskWidget::showScopeMenu);
    connect(m_more, &QToolButton::clicked, this, &TaskWidget::showMoreMenu);

    auto* quickRow = new QHBoxLayout;
    quickRow->setContentsMargins(16, 0, 16, 0);
    auto* quickFrame = new QFrame(m_paper);
    quickFrame->setObjectName(QStringLiteral("WidgetQuickAddFrame"));
    quickFrame->setFixedHeight(40);
    auto* quick = new QHBoxLayout(quickFrame);
    quick->setContentsMargins(10, 0, 8, 0);
    quick->setSpacing(8);
    auto* plus = new QLabel(quickFrame);
    plus->setObjectName(QStringLiteral("WidgetQuickAddIcon"));
    plus->setFixedSize(18, 18);
    plus->setPixmap(themedIcon(IconId::Add, resolvedTheme(), 18).pixmap(18, 18));
    quick->addWidget(plus, 0, Qt::AlignVCenter);
    m_quick = new QLineEdit(quickFrame);
    m_quick->setObjectName(QStringLiteral("WidgetQuickAdd"));
    m_quick->setAccessibleName(QStringLiteral("添加任务"));
    m_quick->setPlaceholderText(QStringLiteral("添加任务"));
    m_quick->setMaxLength(500);
    m_quick->setFixedHeight(34);
    m_quick->setFont(ds::qfont(ds::FontScene::Body));
    quick->addWidget(m_quick, 1);
    m_quickDate = new QToolButton(quickFrame);
    m_quickDate->setObjectName(QStringLiteral("WidgetQuickAddDate"));
    m_quickDate->setAccessibleName(QStringLiteral("设置新任务日期"));
    m_quickDate->setToolTip(QStringLiteral("设置新任务日期"));
    m_quickDate->setCursor(Qt::PointingHandCursor);
    m_quickDate->setFixedHeight(28);
    m_quickDate->setMinimumWidth(52);
    m_quickDate->setMaximumWidth(110);
    m_quickDate->hide();
    quick->addWidget(m_quickDate, 0, Qt::AlignVCenter);
    quickRow->addWidget(quickFrame);
    paper->addLayout(quickRow);
    connect(m_quick, &QLineEdit::returnPressed, this, &TaskWidget::createTask);
    connect(m_quick, &QLineEdit::textChanged, this, [this]() { updateQuickDateVisibility(); });

    m_model = new TaskListModel(m_store, this);
    m_list = new TaskListView(m_paper);
    m_list->setObjectName(QStringLiteral("TaskList"));
    m_list->setModel(m_model);
    m_list->setAccessibleName(QStringLiteral("任务小部件列表"));
    m_list->setMouseTracking(true);
    m_list->viewport()->setAttribute(Qt::WA_Hover, true);
    m_list->setUniformItemSizes(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_delegate = new TaskDelegate(m_list);
    m_list->setItemDelegate(m_delegate);
    m_dateController = new TaskDateController(m_store, this);

    m_empty = new QLabel(QStringLiteral("没有任务"), m_paper);
    m_empty->setObjectName(QStringLiteral("WidgetEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setFont(ds::qfont(ds::FontScene::Body));
    m_listStack = new QStackedWidget(m_paper);
    m_listStack->setObjectName(QStringLiteral("WidgetListStack"));
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_empty);
    auto* listRow = new QVBoxLayout;
    listRow->setContentsMargins(0, 8, 0, 8);
    listRow->addWidget(m_listStack, 1);
    paper->addLayout(listRow, 1);

    connect(m_list, &QListView::clicked, this, [this](const QModelIndex& index) {
        const QString id = m_model->entityId(index);
        if (!id.isEmpty()) emit taskOpenRequested(id);
    });
    connect(m_list, &QListView::activated, this, [this](const QModelIndex& index) {
        if (QApplication::keyboardModifiers() != Qt::NoModifier) return;
        const QString id = m_model->entityId(index);
        if (!id.isEmpty()) emit taskOpenRequested(id);
    });
    connect(m_delegate, &TaskDelegate::dateActivated, this,
            [this](const QString& id, const QRect& anchor) {
        m_dateController->open(id, QRect(m_list->viewport()->mapToGlobal(anchor.topLeft()),
                                         anchor.size()));
    });
    connect(m_delegate, &TaskDelegate::listActivated, this, [this](const QString& listId) {
        selectScope(ViewScope::List, listId);
    });
    connect(m_delegate, &TaskDelegate::postponeActivated, this, [this](const QStringList& ids) {
        if (ids.isEmpty()) return;
        if (!ConfirmDialog::prompt(QStringLiteral("顺延至今天"),
                QStringLiteral("当前清单中所有已过期任务将被顺延至今天。"),
                QStringLiteral("顺延"), false, this)) return;
        if (m_store && !m_store->postponeOverdueTasks(ids, toAppTime(appNow()).date()))
            QToolTip::showText(QCursor::pos(), m_store->lastError(), this);
    });
    connect(m_delegate, &TaskDelegate::childrenToggleActivated, this,
            [this](const QString& id) { m_model->toggleChildren(id); });
    connect(m_delegate, &TaskDelegate::sectionToggleActivated, this,
            [this](const QString& key) { m_model->toggleSection(key); });
    connect(m_list, &TaskListView::entityDropRequested, this,
            [this](const TaskDrop::Intent& intent) {
        if (!m_store) return;
        if (!m_store->applyDropIntent(intent)) {
            QToolTip::showText(QCursor::pos(), m_store->lastError(), this);
            return;
        }
        if (intent.placement == TaskDrop::Placement::AsChild)
            m_model->expandChildren(intent.targetParentId);
    });
    connect(m_list, &TaskListView::dragBlocked, this, [this](const QString& reason) {
        QToolTip::showText(QCursor::pos(), reason, this);
    });
    connect(m_list, &QListView::customContextMenuRequested, this, [this](const QPoint& position) {
        const QString id = m_model->entityId(m_list->indexAt(position));
        if (id.isEmpty() || !m_store) return;
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
        QMenu* rawMenu = commands.buildEntityMenu(context, id, this);
        if (auto* popup = qobject_cast<PopupMenu*>(rawMenu)) {
            if (m_appearance != 0) popup->setThemeOverride(resolvedTheme());
        }
        rawMenu->setAttribute(Qt::WA_DeleteOnClose, true);
        connect(&commands, &EntityCommands::commandExecuted, this,
                [this](EntityCommand command, const QString& entityId) {
            if (command == EntityCommand::ViewActivity || command == EntityCommand::UploadAttachment)
                emit taskOpenRequested(entityId);
        });
        connect(&commands, &EntityCommands::entityCreated, this,
                [this](const QString& created) { emit taskOpenRequested(created); });
        connect(&commands, &EntityCommands::errorOccurred, this, [this](const QString& message) {
            QToolTip::showText(QCursor::pos(), message, this);
        });
        rawMenu->exec(m_list->viewport()->mapToGlobal(position));
    });
    connect(m_dateController, &TaskDateController::draftSaved, this,
            [this](const Entity& draft, const ScheduleValue& schedule) {
        m_quickDraft = draft;
        m_quickSchedule = schedule;
        applyScheduleToEntity(&m_quickDraft, schedule);
        m_quickDateCustomized = true;
        m_quickDate->setText(scheduleLabel(m_quickDraft));
        updateQuickDateVisibility();
    });
    connect(m_dateController, &TaskDateController::closed, this,
            [this]() { QTimer::singleShot(0, this, &TaskWidget::updateQuickDateVisibility); });
    connect(m_dateController, &TaskDateController::saveFailed, this, [this](const QString& message) {
        QToolTip::showText(QCursor::pos(), message, this);
    });
    connect(m_quickDate, &QToolButton::clicked, this, [this]() {
        m_dateController->openDraft(m_quickDraft, m_quickSchedule,
            QRect(m_quickDate->mapToGlobal(QPoint()), m_quickDate->size()));
        updateQuickDateVisibility();
    });
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        updateQuickDateVisibility();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, &TaskWidget::updateEmptyState);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &TaskWidget::updateEmptyState);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &TaskWidget::updateEmptyState);
    if (m_store) {
        connect(m_store, &TaskStore::groupsChanged, this, &TaskWidget::applyFilter);
        connect(m_store, &TaskStore::tasksReset, this, &TaskWidget::applyFilter);
    }

    const struct { const char* name; Qt::Edges edges; } definitions[] = {
        {"TaskWidgetResizeLeft", Qt::LeftEdge}, {"TaskWidgetResizeRight", Qt::RightEdge},
        {"TaskWidgetResizeTop", Qt::TopEdge}, {"TaskWidgetResizeBottom", Qt::BottomEdge},
        {"TaskWidgetResizeTopLeft", Qt::LeftEdge | Qt::TopEdge},
        {"TaskWidgetResizeTopRight", Qt::RightEdge | Qt::TopEdge},
        {"TaskWidgetResizeBottomLeft", Qt::LeftEdge | Qt::BottomEdge},
        {"TaskWidgetResizeBottomRight", Qt::RightEdge | Qt::BottomEdge}
    };
    for (const auto& definition : definitions)
        m_resizeAreas.append(new WindowResizeArea(this, definition.edges,
                                                  QString::fromLatin1(definition.name)));

    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    disableNativeWindowShadows();
}

ds::Theme TaskWidget::resolvedTheme() const
{
    if (m_appearance == 1) return ds::Theme::Light;
    if (m_appearance == 2) return ds::Theme::Dark;
    return ThemeManager::instance().resolvedTheme();
}

void TaskWidget::applyAppearance()
{
    const ds::Theme theme = resolvedTheme();
    const auto color = [theme](ds::ColorToken token) { return ds::color(token, theme); };
    if (m_appearance == 0) {
        m_delegate->clearThemeOverride();
    } else {
        m_delegate->setThemeOverride(theme);
    }
    m_scope->setIcon(themedIcon(IconId::ChevronDown, theme, 12));
    m_scope->setIconSize(QSize(12, 12));
    m_more->setIcon(themedIcon(IconId::More, theme, 18));
    if (auto* plus = m_paper->findChild<QLabel*>(QStringLiteral("WidgetQuickAddIcon")))
        plus->setPixmap(themedIcon(IconId::Add, theme, 18).pixmap(18, 18));
    m_quickDate->setIcon(themedIcon(IconId::Calendar, theme, 14));
    m_quickDate->setIconSize(QSize(14, 14));

    m_paper->setStyleSheet(QStringLiteral(
        "QWidget#TaskWidgetPaper,QWidget#TaskWidgetHeader,QStackedWidget#WidgetListStack{background:transparent;border:0;}"
        "QToolButton#WidgetScope{background:transparent;border:0;border-radius:6px;color:%1;padding:0 4px;text-align:left;}"
        "QToolButton#WidgetScope:hover{background:%2;}"
        "QToolButton#WidgetMore{background:transparent;border:0;border-radius:6px;padding:0;}"
        "QToolButton#WidgetMore:hover{background:%2;}"
        "QFrame#WidgetQuickAddFrame{background:%3;border:0;border-radius:7px;}"
        "QLineEdit#WidgetQuickAdd{background:transparent;border:0;color:%1;padding:0;selection-background-color:%5;}"
        "QToolButton#WidgetQuickAddDate{background:transparent;border:0;border-radius:6px;color:%4;padding:0 5px;}"
        "QToolButton#WidgetQuickAddDate:hover{background:%2;}"
        "QListView#TaskList{background:transparent;border:0;outline:0;}"
        "QLabel#WidgetEmpty{background:transparent;border:0;color:%4;}")
        .arg(color(ds::ColorToken::Text).name())
        .arg(color(ds::ColorToken::Hover).name(QColor::HexArgb))
        .arg(color(ds::ColorToken::Field).name(QColor::HexArgb))
        .arg(color(ds::ColorToken::TextSecondary).name())
        .arg(color(ds::ColorToken::PrimarySoft).name(QColor::HexArgb)));
    QPalette palette = m_quick->palette();
    palette.setColor(QPalette::Text, color(ds::ColorToken::Text));
    palette.setColor(QPalette::PlaceholderText, color(ds::ColorToken::TextTertiary));
    palette.setColor(QPalette::Base, Qt::transparent);
    m_quick->setPalette(palette);
    palette = m_list->palette();
    palette.setColor(QPalette::Text, color(ds::ColorToken::Text));
    palette.setColor(QPalette::Base, Qt::transparent);
    palette.setColor(QPalette::Highlight, color(ds::ColorToken::RowSelected));
    palette.setColor(QPalette::HighlightedText, color(ds::ColorToken::Text));
    m_list->setPalette(palette);
    m_shadow = QImage();
    m_shadowSize = QSize();
    update();
}

void TaskWidget::selectScope(ViewScope scope, const QString& listId)
{
    m_filter.scope = scope;
    m_filter.listId = scope == ViewScope::List ? listId : QString();
    m_filter.folderId.clear();
    if (scope == ViewScope::List && m_filter.listId.isEmpty() && m_store)
        m_filter.listId = m_store->inboxListId();
    applyFilter();
    saveState();
}

void TaskWidget::setGrouping(GroupMode grouping)
{
    m_filter.grouping = grouping;
    applyFilter();
    saveState();
}

void TaskWidget::setShowCompleted(bool enabled)
{
    m_filter.showCompleted = enabled;
    applyFilter();
    saveState();
}

void TaskWidget::setShowDetails(bool enabled)
{
    m_filter.showDetails = enabled;
    applyFilter();
    saveState();
}

void TaskWidget::setCountdownDates(bool enabled)
{
    m_filter.countdownDates = enabled;
    applyFilter();
    saveState();
}

void TaskWidget::setAppearance(int appearance)
{
    m_appearance = qBound(0, appearance, 2);
    applyAppearance();
    saveState();
}

void TaskWidget::applyFilter()
{
    if (!m_store || !m_model) return;
    if (m_filter.scope == ViewScope::List) {
        bool valid = false;
        for (const TaskList& list : m_store->lists()) {
            if (list.id == m_filter.listId && list.archivedAtUtcMs == 0) {
                valid = true;
                break;
            }
        }
        if (!valid) m_filter.listId = m_store->inboxListId();
    }
    m_filter.tasksOnly = true;
    m_filter.search.clear();
    m_filter.renderer = ViewRenderer::List;
    m_model->setFilter(m_filter);
    updateHeader();
    updateEmptyState();
}

void TaskWidget::updateHeader()
{
    m_scopeTitle.clear();
    switch (m_filter.scope) {
    case ViewScope::Today: m_scopeTitle = QStringLiteral("今天"); break;
    case ViewScope::Next7Days: m_scopeTitle = QStringLiteral("最近7天"); break;
    case ViewScope::AllLists: m_scopeTitle = QStringLiteral("全部任务"); break;
    case ViewScope::List:
        if (m_store) {
            for (const TaskList& list : m_store->lists()) {
                if (list.id == m_filter.listId) {
                    m_scopeTitle = list.inbox ? QStringLiteral("收集箱") : list.name;
                    break;
                }
            }
        }
        break;
    default: break;
    }
    if (m_scopeTitle.isEmpty()) m_scopeTitle = QStringLiteral("收集箱");
    m_scope->setText(m_scope->fontMetrics().elidedText(m_scopeTitle, Qt::ElideRight, 180));
    m_scope->setToolTip(m_scopeTitle);
    setWindowTitle(m_scopeTitle + QStringLiteral(" — 任务小部件"));
}

void TaskWidget::updateEmptyState()
{
    if (!m_listStack || !m_model) return;
    m_listStack->setCurrentWidget(m_model->entityCount() == 0
        ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_list));
}

void TaskWidget::updateQuickDateVisibility()
{
    if (!m_quickDate) return;
    if (!m_quickDateCustomized) m_quickDate->setText(QStringLiteral("日期"));
    m_quickDate->setVisible(m_quickDateCustomized || m_quick->hasFocus()
                            || (m_dateController && m_dateController->isOpen()));
}

void TaskWidget::createTask()
{
    if (!m_store) return;
    const QString title = m_quick->text().trimmed();
    if (title.isEmpty()) {
        m_quick->setFocus();
        return;
    }
    Entity draft = m_quickDateCustomized ? m_quickDraft : Entity();
    draft.id.clear();
    draft.kind = EntityKind::Task;
    draft.title = title;
    draft.listId = m_filter.scope == ViewScope::List ? m_filter.listId : QString();
    ScheduleValue schedule;
    bool hasSchedule = false;
    if (m_quickDateCustomized) {
        schedule = m_quickSchedule;
        hasSchedule = true;
    } else if (m_filter.scope == ViewScope::Today || m_filter.scope == ViewScope::Next7Days) {
        schedule.allDayDate = toAppTime(appNow()).date();
        hasSchedule = true;
    }
    QString id;
    if (hasSchedule) {
        if (!m_store->saveSchedule(draft, schedule, &id)) {
            QToolTip::showText(QCursor::pos(), m_store->lastError(), this);
            return;
        }
    } else if (m_filter.scope == ViewScope::List) {
        id = m_store->createTask(title, m_filter.listId);
    } else {
        id = m_store->createTask(title);
    }
    if (id.isEmpty()) {
        QToolTip::showText(QCursor::pos(), m_store->lastError(), this);
        return;
    }
    m_quickDraft = Entity();
    m_quickSchedule = ScheduleValue();
    m_quickDateCustomized = false;
    m_quick->clear();
    updateQuickDateVisibility();
    m_model->refresh();
}

void TaskWidget::showScopeMenu()
{
    if (!m_store) return;
    auto* menu = new PopupMenu(this);
    menu->setObjectName(QStringLiteral("TaskWidgetScopeMenu"));
    menu->setProperty("entityMenu", true);
    if (m_appearance != 0) menu->setThemeOverride(resolvedTheme());
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    const auto addScope = [this, menu, group](const QString& text, ViewScope scope,
                                               const QString& listId, IconId icon) {
        QAction* action = menu->addAction(themedIcon(icon, resolvedTheme()), text);
        action->setCheckable(true);
        action->setChecked(m_filter.scope == scope
                           && (scope != ViewScope::List || m_filter.listId == listId));
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, scope, listId]() { selectScope(scope, listId); });
    };
    addScope(QStringLiteral("今天"), ViewScope::Today, QString(), IconId::Today);
    addScope(QStringLiteral("最近7天"), ViewScope::Next7Days, QString(), IconId::Upcoming);
    addScope(QStringLiteral("收集箱"), ViewScope::List, m_store->inboxListId(), IconId::Inbox);
    addScope(QStringLiteral("全部任务"), ViewScope::AllLists, QString(), IconId::All);
    menu->addSeparator();
    for (const TaskList& list : m_store->lists()) {
        if (list.inbox || list.archivedAtUtcMs > 0) continue;
        addScope(list.name, ViewScope::List, list.id, IconId::TaskList);
    }
    presentMenu(menu, m_scope);
}

void TaskWidget::showMoreMenu()
{
    auto* menu = new PopupMenu(this);
    menu->setObjectName(QStringLiteral("TaskWidgetMoreMenu"));
    menu->setProperty("entityMenu", true);
    if (m_appearance != 0) menu->setThemeOverride(resolvedTheme());
    PopupMenu* options = menu->addMenu(themedIcon(IconId::Settings, resolvedTheme()),
                                       QStringLiteral("选项"));
    options->setObjectName(QStringLiteral("TaskWidgetOptionsMenu"));
    options->setProperty("entityMenu", true);

    PopupMenu* grouping = options->addMenu(themedIcon(IconId::Grouping, resolvedTheme()),
                                           QStringLiteral("分组方式"));
    grouping->setObjectName(QStringLiteral("TaskWidgetGroupingMenu"));
    grouping->setProperty("entityMenu", true);
    auto* groupingGroup = new QActionGroup(grouping);
    groupingGroup->setExclusive(true);
    const struct { GroupMode mode; const char* text; IconId icon; } modes[] = {
        {GroupMode::Time, "按时间", IconId::Calendar},
        {GroupMode::List, "按清单", IconId::TaskList},
        {GroupMode::Priority, "按优先级", IconId::FlagHigh},
        {GroupMode::Created, "按创建时间", IconId::Clock},
        {GroupMode::None, "不分组", IconId::TaskList}
    };
    for (const auto& mode : modes) {
        QAction* action = grouping->addAction(themedIcon(mode.icon, resolvedTheme()),
                                              QString::fromUtf8(mode.text));
        action->setCheckable(true);
        action->setChecked(m_filter.grouping == mode.mode);
        groupingGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode]() { setGrouping(mode.mode); });
    }

    QAction* completed = options->addAction(themedIcon(IconId::Done, resolvedTheme()),
                                            QStringLiteral("显示已完成与已放弃"));
    completed->setObjectName(QStringLiteral("WidgetShowCompleted"));
    completed->setCheckable(true);
    completed->setChecked(m_filter.showCompleted);
    connect(completed, &QAction::triggered, this, &TaskWidget::setShowCompleted);
    QAction* details = options->addAction(themedIcon(IconId::DetailCalendar, resolvedTheme()),
                                          QStringLiteral("显示任务详情"));
    details->setObjectName(QStringLiteral("WidgetShowDetails"));
    details->setCheckable(true);
    details->setChecked(m_filter.showDetails);
    connect(details, &QAction::triggered, this, &TaskWidget::setShowDetails);
    QAction* countdown = options->addAction(themedIcon(IconId::Clock, resolvedTheme()),
                                            QStringLiteral("日期显示为倒计时"));
    countdown->setObjectName(QStringLiteral("WidgetCountdownDates"));
    countdown->setCheckable(true);
    countdown->setChecked(m_filter.countdownDates);
    connect(countdown, &QAction::triggered, this, &TaskWidget::setCountdownDates);

    PopupMenu* appearance = options->addMenu(themedIcon(IconId::Theme, resolvedTheme()),
                                             QStringLiteral("外观"));
    appearance->setObjectName(QStringLiteral("TaskWidgetAppearanceMenu"));
    appearance->setProperty("entityMenu", true);
    auto* appearanceGroup = new QActionGroup(appearance);
    appearanceGroup->setExclusive(true);
    const struct { int value; const char* text; } appearances[] = {
        {0, "跟随应用"}, {1, "浅色"}, {2, "深色"}
    };
    for (const auto& item : appearances) {
        QAction* action = appearance->addAction(QString::fromUtf8(item.text));
        action->setCheckable(true);
        action->setChecked(m_appearance == item.value);
        appearanceGroup->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, item]() { setAppearance(item.value); });
    }

    QAction* lock = menu->addAction(themedIcon(IconId::Lock, resolvedTheme()),
                                    m_locked ? QStringLiteral("解除锁定") : QStringLiteral("锁定"));
    lock->setObjectName(QStringLiteral("WidgetLock"));
    lock->setCheckable(true);
    lock->setChecked(m_locked);
    lock->setVisible(windowLockEnabled());
    connect(lock, &QAction::triggered, this, &TaskWidget::setLocked);
    QAction* close = menu->addAction(themedIcon(IconId::Exit, resolvedTheme()),
                                     QStringLiteral("关闭"));
    close->setObjectName(QStringLiteral("WidgetClose"));
    connect(close, &QAction::triggered, this, &TaskWidget::closeWidget);
    presentMenu(menu, m_more);
}

void TaskWidget::presentMenu(PopupMenu* menu, QWidget* anchor)
{
    if (m_menu) m_menu->hide();
    m_menu = menu;
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    popupAt(menu, anchor);
}

void TaskWidget::closePopups()
{
    if (m_menu) m_menu->hide();
    m_dateController->close();
}

void TaskWidget::setRestoreFrozen(bool frozen)
{
    if (frozen) closePopups();
    setEnabled(!frozen);
}

bool TaskWidget::hasUncommittedDraft() const
{
    return !m_quick->text().trimmed().isEmpty();
}

void TaskWidget::setLocked(bool locked)
{
    if (!windowLockEnabled() || m_locked == locked) return;
    m_locked = locked;
    for (WindowResizeArea* area : m_resizeAreas) area->setVisible(!locked);
    saveState();
    emit lockChanged(locked);
}

void TaskWidget::showWidget()
{
    if (!isEnabled()) return;
    m_open = true;
    m_model->refresh();
    show();
    saveState();
    raise();
    activateWindow();
}

void TaskWidget::closeWidget()
{
    closePopups();
    m_open = false;
    saveState();
    hide();
    emit closed();
}

bool TaskWidget::saveState()
{
    if (m_loading) return true;
    auto settings = widgetSettings();
    settings->setValue(QStringLiteral("open"), m_open);
    settings->setValue(QStringLiteral("locked"), m_locked);
    settings->setValue(QStringLiteral("appearance"), m_appearance);
    settings->setValue(QStringLiteral("geometry"), geometry());
    settings->setValue(QStringLiteral("scope"), static_cast<int>(m_filter.scope));
    settings->setValue(QStringLiteral("listId"), m_filter.listId);
    settings->setValue(QStringLiteral("grouping"), groupModeName(m_filter.grouping));
    settings->setValue(QStringLiteral("showCompleted"), m_filter.showCompleted);
    settings->setValue(QStringLiteral("showDetails"), m_filter.showDetails);
    settings->setValue(QStringLiteral("countdownDates"), m_filter.countdownDates);
    settings->sync();
    const bool ok = settings->status() == QSettings::NoError;
    if (!ok)
        qWarning() << "Task widget settings:" << settings->fileName();
    return ok;
}

bool TaskWidget::flush()
{
    m_geometryTimer.stop();
    return saveState();
}

void TaskWidget::scheduleGeometrySave()
{
    if (!m_loading) m_geometryTimer.start();
}

QPoint TaskWidget::constrainedWindowPosition(const QPoint& raw) const
{
    const QRect proposed(raw + QPoint(PopupShadowMargin, PopupShadowMargin),
                         size() - QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin));
    QScreen* screen = QGuiApplication::screenAt(proposed.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return raw;
    const QRect snapped = NoteSnap::snapped(proposed, screen->availableGeometry(), {}, 10, 8);
    return snapped.topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin);
}

void TaskWidget::syncChromeGeometry()
{
    const QRect paper = rect().adjusted(PopupShadowMargin, PopupShadowMargin,
                                        -PopupShadowMargin, -PopupShadowMargin);
    const int left = paper.left(), top = paper.top();
    const int right = paper.right(), bottom = paper.bottom();
    const QRect areas[] = {
        QRect(left - 3, top + 7, 6, qMax(1, paper.height() - 14)),
        QRect(right - 2, top + 7, 6, qMax(1, paper.height() - 14)),
        QRect(left + 7, top - 3, qMax(1, paper.width() - 14), 6),
        QRect(left + 7, bottom - 2, qMax(1, paper.width() - 14), 6),
        QRect(left - 3, top - 3, 10, 10), QRect(right - 6, top - 3, 10, 10),
        QRect(left - 3, bottom - 6, 10, 10), QRect(right - 6, bottom - 6, 10, 10)
    };
    for (int index = 0; index < m_resizeAreas.size(); ++index) {
        m_resizeAreas.at(index)->setGeometry(areas[index]);
        m_resizeAreas.at(index)->setVisible(!m_locked);
        m_resizeAreas.at(index)->raise();
    }
}

void TaskWidget::paintEvent(QPaintEvent*)
{
    const QRectF card = QRectF(rect()).adjusted(PopupShadowMargin, PopupShadowMargin,
                                                 -PopupShadowMargin, -PopupShadowMargin);
    const QColor shadow = ds::color(ds::ColorToken::Shadow, resolvedTheme());
    if (m_shadow.isNull() || m_shadowSize != size() || m_shadowColor != shadow) {
        m_shadow = popupShadow(size(), card, kPaperRadius, shadow);
        m_shadowSize = size();
        m_shadowColor = shadow;
    }
    QPainter painter(this);
    if (!m_shadow.isNull()) painter.drawImage(0, 0, m_shadow);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ds::color(ds::ColorToken::Surface, resolvedTheme()));
    painter.drawRoundedRect(card, kPaperRadius, kPaperRadius);
}

void TaskWidget::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    const bool changed = geometry() != m_observedGeometry;
    m_observedGeometry = geometry();
    if (changed) scheduleGeometrySave();
}

void TaskWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    syncChromeGeometry();
    const bool changed = geometry() != m_observedGeometry;
    m_observedGeometry = geometry();
    if (changed) scheduleGeometrySave();
    update();
}

void TaskWidget::closeEvent(QCloseEvent* event)
{
    event->ignore();
    closeWidget();
}

void TaskWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowDeactivate && !m_loading) saveState();
}

}  // namespace kl
