#include "EntityCommands.h"

#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "AppIcons.h"
#include "DetailPopups.h"
#include "EntityPicker.h"
#include "ListAppearance.h"
#include "PopupMenu.h"
#include "ToastIsland.h"
#include "design/TypeSystem.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QResizeEvent>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QWidgetAction>

namespace kl {
namespace {

struct CommandInfo {
    const char *objectName;
    QString text;
    IconId icon;
};

CommandInfo commandInfo(EntityCommand cmd)
{
    switch (cmd) {
    case EntityCommand::SetDateToday:
        return {"setDateToday", QStringLiteral("今天"), IconId::Today};
    case EntityCommand::SetDateTomorrow:
        return {"setDateTomorrow", QStringLiteral("明天"), IconId::Upcoming};
    case EntityCommand::SetDateNextWeek:
        return {"setDateNextWeek", QStringLiteral("下周"), IconId::Calendar};
    case EntityCommand::SetDateNextPeriod:
        // 考据未确认 quick-nextperiod 的 tooltip，不扩写为下月或下次重复。
        return {"setDateNextPeriod", QStringLiteral("下一周期"), IconId::Repeat};
    case EntityCommand::SetDateNone:
        return {"setDateNone", QStringLiteral("无日期"), IconId::Calendar};
    case EntityCommand::ClearDate:
        return {"clearDate", QStringLiteral("清除日期"), IconId::Close};
    case EntityCommand::SetPriorityHigh:
        return {"setPriorityHigh", QStringLiteral("高优先级"), IconId::FlagHigh};
    case EntityCommand::SetPriorityMedium:
        return {"setPriorityMedium", QStringLiteral("中优先级"), IconId::FlagMedium};
    case EntityCommand::SetPriorityLow:
        return {"setPriorityLow", QStringLiteral("低优先级"), IconId::FlagLow};
    case EntityCommand::SetPriorityNone:
        return {"setPriorityNone", QStringLiteral("无优先级"), IconId::FlagNone};
    case EntityCommand::AddSubtask:
        return {"addSubtask", QStringLiteral("添加子任务"), IconId::AddSubtask};
    case EntityCommand::LinkParent:
        return {"linkParent", QStringLiteral("关联主任务"), IconId::LinkParent};
    case EntityCommand::TogglePin:
        return {"togglePin", QStringLiteral("置顶"), IconId::Pin};
    case EntityCommand::Abandon:
        return {"abandon", QStringLiteral("放弃"), IconId::Abandoned};
    case EntityCommand::MoveTo:
        return {"moveTo", QStringLiteral("移动到"), IconId::MoveTo};
    case EntityCommand::EditTags:
        return {"editTags", QStringLiteral("标签"), IconId::Tag};
    case EntityCommand::StartFocus:
        return {"startFocus", QStringLiteral("开始专注"), IconId::Clock};
    case EntityCommand::Clone:
        return {"clone", QStringLiteral("创建副本"), IconId::Copy};
    case EntityCommand::CopyLink:
        return {"copyLink", QStringLiteral("复制链接"), IconId::Link};
    case EntityCommand::ConvertKind:
        return {"convertKind", QStringLiteral("转换类型"), IconId::Convert};
    case EntityCommand::Delete:
        return {"delete", QStringLiteral("删除"), IconId::Delete};
    case EntityCommand::Restore:
        return {"restore", QStringLiteral("恢复"), IconId::Restore};
    case EntityCommand::DeletePermanently:
        return {"deletePermanently", QStringLiteral("永久删除"), IconId::Trash};
    case EntityCommand::UploadAttachment:
        return {"uploadAttachment", QStringLiteral("上传附件"), IconId::Attachment};
    case EntityCommand::ViewActivity:
        return {"viewActivity", QStringLiteral("任务动态"), IconId::ActivityLog};
    case EntityCommand::SaveAsTemplate:
        return {"saveAsTemplate", QStringLiteral("保存为模板"), IconId::DesktopNote};
    case EntityCommand::Export:
        return {"export", QStringLiteral("导出"), IconId::Backup};
    case EntityCommand::EditCompletionTime:
        return {"editCompletionTime", QStringLiteral("修改完成时间"), IconId::Clock};
    case EntityCommand::Complete:
        return {"complete", QStringLiteral("完成"), IconId::Check};
    case EntityCommand::CopyText:
        return {"copyText", QStringLiteral("复制文本"), IconId::Note};
    case EntityCommand::Merge:
        return {"merge", QStringLiteral("合并"), IconId::Checklist};
    case EntityCommand::RenameList:
        return {"renameList", QStringLiteral("重命名清单…"), IconId::TaskList};
    case EntityCommand::DeleteList:
        return {"deleteList", QStringLiteral("删除清单…"), IconId::Delete};
    case EntityCommand::RenameFolder:
        return {"renameFolder", QStringLiteral("重命名文件夹…"), IconId::Folder};
    case EntityCommand::DeleteFolder:
        return {"deleteFolder", QStringLiteral("解散"), IconId::Delete};
    case EntityCommand::CreateListInFolder:
        return {"createListInFolder", QStringLiteral("添加清单"), IconId::Add};
    case EntityCommand::Edit:
        return {"edit", QStringLiteral("编辑"), IconId::Settings};
    case EntityCommand::ShowWhenNonEmpty:
        return {"showWhenNonEmpty", QStringLiteral("有内容时显示"), IconId::List};
    case EntityCommand::Hide:
        return {"hide", QStringLiteral("隐藏"), IconId::Close};
    case EntityCommand::Share:
        return {"share", QStringLiteral("共享"), IconId::Link};
    case EntityCommand::Archive:
        return {"archive", QStringLiteral("归档"), IconId::Archive};
    case EntityCommand::Reactivate:
        return {"reactivate", QStringLiteral("恢复进行中"), IconId::Restore};
    case EntityCommand::Count:
        break;
    }
    return {"unknownCommand", QStringLiteral("未知命令"), IconId::More};
}

void setActionText(QAction *action, const QString &text)
{
    action->setText(text);
    action->setProperty("accessibleName", text);
    action->setToolTip(text);
    action->setStatusTip(text);
}

void setPinAction(QAction *action, bool pinned)
{
    setActionText(action, pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    action->setIcon(AppIcons::icon(pinned ? IconId::Unpin : IconId::Pin, 18));
}

// 网页快捷区分组高 68px：标题 y=10/h=16，图标盒 y=32；
// 图标四周各外扩 6px 作为悬浮与命中区域，最后一个图标的右缘距分组 12px。
class QuickSection final : public QWidget {
public:
    QuickSection(const QString &title, int glyphSize, QWidget *parent = nullptr)
        : QWidget(parent), m_glyphSize(glyphSize), m_title(new QLabel(title, this))
    {
        setFixedHeight(68);
        m_title->setObjectName(QStringLiteral("QuickBarTitle"));
        m_title->setFont(ds::qfont(ds::FontScene::MenuCaption));
        m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void addButton(QToolButton *button) { m_buttons.append(button); }
    QSize sizeHint() const override { return QSize(160, 68); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_title->setGeometry(12, 10, qMax(0, width() - 24), 16);
        const int visibleSpan = qMax(0, width() - 24 - m_glyphSize);
        for (int i = 0; i < m_buttons.size(); ++i) {
            const int glyphX = 12 + (m_buttons.size() > 1
                ? i * visibleSpan / (m_buttons.size() - 1) : 0);
            m_buttons.at(i)->setGeometry(glyphX - 6, 32,
                                        m_glyphSize + 12, m_glyphSize + 12);
        }
    }

private:
    int m_glyphSize;
    QLabel *m_title;
    QVector<QToolButton *> m_buttons;
};

QMenu *newMenu(const QString &objectName, QWidget *parent)
{
    QMenu *menu = new PopupMenu(parent);
    menu->setObjectName(objectName);
    menu->setProperty("entityMenu", true);
    // PopupMenu 两侧各留 20px 透明阴影边距；可见卡片与网页一致至少 194px。
    menu->setMinimumWidth(194 + 2 * PopupMenu::shadowMargin());
    menu->setToolTipsVisible(false);
    menu->setSeparatorsCollapsible(false);
    return menu;
}

QAction *addSeparator(QMenu *menu)
{
    QAction *separator = menu->addSeparator();
    separator->setObjectName(QStringLiteral("separator%1").arg(menu->actions().size()));
    separator->setProperty("accessibleName", QStringLiteral("命令分组"));
    separator->setToolTip(QStringLiteral("命令分组"));
    separator->setIcon(AppIcons::icon(IconId::None));
    return separator;
}

QWidgetAction *addWidgetAction(QMenu *menu, QWidget *widget, const QString &objectName,
                               const QString &accessibleName, IconId icon)
{
    QWidgetAction *action = new QWidgetAction(menu);
    action->setObjectName(objectName);
    action->setProperty("accessibleName", accessibleName);
    action->setToolTip(accessibleName);
    action->setStatusTip(accessibleName);
    action->setIcon(AppIcons::icon(icon));
    action->setDefaultWidget(widget);
    menu->addAction(action);
    return action;
}

QList<QAction *> visibleDestinations(QMenu *menu)
{
    QList<QAction *> result;
    for (QAction *action : menu->actions()) {
        if (action->isVisible() && action->isEnabled()
            && !action->property("targetListId").toString().isEmpty()) {
            result.append(action);
        }
    }
    return result;
}

void closeMoveMenus(QMenu *menu)
{
    if (!menu)
        return;
    QMenu *parentMenu = qobject_cast<QMenu *>(menu->parentWidget());
    menu->close();
    if (parentMenu)
        parentMenu->close();
}

} // namespace

EntityCommands::EntityCommands(TaskStore *store, QObject *parent)
    : QObject(parent), m_store(store)
{
}

QMenu *EntityCommands::buildEntityMenu(const MenuContext &ctx, const QString &entityId, QWidget *parent)
{
    QMenu *menu = newMenu(QStringLiteral("EntityContextMenu"), parent);

    // R10：回收站不混入任何普通属性或复用命令。
    if (ctx.deleted) {
        menu->addAction(makeAction(EntityCommand::Restore, entityId, menu));
        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::DeletePermanently, entityId, menu));
        return menu;
    }

    const bool isTask = ctx.kind == EntityKind::Task;
    const bool completedTask = isTask && ctx.completed;
    const bool activeTask = isTask && !ctx.completed && !ctx.abandoned;

    // R26：多选不是单选菜单的重复，尤其“复制文本”不能换成“复制链接”。
    if (ctx.selectedCount > 1) {
        addQuickDateBar(menu, ctx, entityId);
        if (isTask)
            addPriorityBar(menu, ctx, entityId);
        addSeparator(menu);

        QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
        setPinAction(pin, ctx.pinned);
        menu->addAction(pin);
        if (isTask)
            menu->addAction(makeAction(EntityCommand::Complete, entityId, menu));
        if (isTask)
            menu->addAction(makeAction(EntityCommand::Abandon, entityId, menu));
        addMoveToMenu(menu, entityId);
        menu->addAction(makeAction(EntityCommand::EditTags, entityId, menu));
        if (isTask)
            menu->addAction(makeAction(EntityCommand::LinkParent, entityId, menu));

        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::Clone, entityId, menu));
        menu->addAction(makeAction(EntityCommand::CopyText, entityId, menu));
        if (isTask)
            menu->addAction(makeAction(EntityCommand::Merge, entityId, menu));
        QAction *convert = makeAction(EntityCommand::ConvertKind, entityId, menu);
        setActionText(convert, isTask ? QStringLiteral("转换为笔记") : QStringLiteral("转换为任务"));
        menu->addAction(convert);
        menu->addAction(makeAction(EntityCommand::Delete, entityId, menu));
        // entityId 只是代表项；批量写操作必须由持有完整选中集合的宿主执行。
        const QList<QAction *> batchActions = menu->findChildren<QAction *>();
        for (QAction *action : batchActions)
            action->setProperty("batchCommand", true);
        return menu;
    }

    addQuickDateBar(menu, ctx, entityId);
    if (isTask)
        addPriorityBar(menu, ctx, entityId);
    addSeparator(menu);

    // 已完成/已放弃任务和笔记不添加子任务。
    if (activeTask)
        menu->addAction(makeAction(EntityCommand::AddSubtask, entityId, menu));
    if (isTask)
        menu->addAction(makeAction(EntityCommand::LinkParent, entityId, menu));

    QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
    setPinAction(pin, ctx.pinned);
    menu->addAction(pin);
    if (isTask) {
        QAction *complete = makeAction(EntityCommand::Complete, entityId, menu);
        complete->setObjectName(QStringLiteral("CmdToggleComplete"));
        setActionText(complete, ctx.completed ? QStringLiteral("设为未完成") : QStringLiteral("完成任务"));
        menu->addAction(complete);
    }

    if (isTask) {
        QAction *abandon = makeAction(EntityCommand::Abandon, entityId, menu);
        abandon->setEnabled(!ctx.completed && !ctx.abandoned);
        menu->addAction(abandon);
    }
    addMoveToMenu(menu, entityId);

    addSeparator(menu);
    if (ctx.focusEnabled) {
        addFocusMenu(menu, entityId);
        addSeparator(menu);
    }

    // R09 已完成任务只保留复制链接、修改完成时间和删除。
    if (!completedTask) {
        menu->addAction(makeAction(EntityCommand::Clone, entityId, menu));
        QAction *convert = makeAction(EntityCommand::ConvertKind, entityId, menu);
        setActionText(convert, isTask ? QStringLiteral("转换为笔记") : QStringLiteral("转换为任务"));
        menu->addAction(convert);
    } else {
        menu->addAction(makeAction(EntityCommand::EditCompletionTime, entityId, menu));
    }
    menu->addAction(makeAction(EntityCommand::Delete, entityId, menu));
    return menu;
}

QMenu *EntityCommands::buildDetailMoreMenu(const MenuContext &ctx, const QString &entityId,
                                           QWidget *parent)
{
    QMenu *menu = newMenu(QStringLiteral("EntityDetailMoreMenu"), parent);
    menu->setMinimumWidth(164 + 2 * PopupMenu::shadowMargin());
    if (ctx.deleted) {
        menu->addAction(makeAction(EntityCommand::Restore, entityId, menu));
        menu->addAction(makeAction(EntityCommand::ViewActivity, entityId, menu));
        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::DeletePermanently, entityId, menu));
        return menu;
    }
    const bool isTask = ctx.kind == EntityKind::Task;
    const bool active = isTask && !ctx.completed && !ctx.abandoned;
    if (active) {
        menu->addAction(makeAction(EntityCommand::AddSubtask, entityId, menu));
        menu->addAction(makeAction(EntityCommand::LinkParent, entityId, menu));
    }
    if (!isTask || active) {
        QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
        setPinAction(pin, ctx.pinned);
        menu->addAction(pin);
    }
    if (active) menu->addAction(makeAction(EntityCommand::Abandon, entityId, menu));
    else if (isTask) menu->addAction(makeAction(EntityCommand::Reactivate, entityId, menu));
    menu->addAction(makeAction(EntityCommand::UploadAttachment, entityId, menu));
    addSeparator(menu);
    menu->addAction(makeAction(EntityCommand::ViewActivity, entityId, menu));
    menu->addAction(makeAction(EntityCommand::Clone, entityId, menu));
    QAction *convert = makeAction(EntityCommand::ConvertKind, entityId, menu);
    setActionText(convert, isTask ? QStringLiteral("转换为笔记") : QStringLiteral("转换为任务"));
    menu->addAction(convert);
    addSeparator(menu);
    menu->addAction(makeAction(EntityCommand::Delete, entityId, menu));
    return menu;
}

QMenu *EntityCommands::buildNavigationMenu(const MenuContext &ctx, const QString &objectId,
                                           QWidget *parent)
{
    QMenu *menu = newMenu(QStringLiteral("NavigationContextMenu"), parent);

    // R02：系统收集箱不套普通清单菜单。
    if (ctx.isInbox) {
        menu->addAction(makeAction(EntityCommand::Edit, objectId, menu));
        menu->addAction(makeAction(EntityCommand::ShowWhenNonEmpty, objectId, menu));
        menu->addAction(makeAction(EntityCommand::Hide, objectId, menu));
        return menu;
    }

    // R03：文件夹命令作用于容器，不解散其中的实体。
    if (ctx.isFolder) {
        menu->addAction(makeAction(EntityCommand::CreateListInFolder, objectId, menu));
        QAction *rename = makeAction(EntityCommand::RenameFolder, objectId, menu);
        setActionText(rename, QStringLiteral("编辑"));
        menu->addAction(rename);
        QAction *pin = makeAction(EntityCommand::TogglePin, objectId, menu);
        setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
        menu->addAction(pin);
        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::DeleteFolder, objectId, menu));
        return menu;
    }

    // R05：过滤器不提供共享和归档。
    if (ctx.isFilter) {
        menu->addAction(makeAction(EntityCommand::Edit, objectId, menu));
        QAction *pin = makeAction(EntityCommand::TogglePin, objectId, menu);
        setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
        menu->addAction(pin);
        QAction *copy = makeAction(EntityCommand::Clone, objectId, menu);
        setActionText(copy, QStringLiteral("复制"));
        menu->addAction(copy);
        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::Delete, objectId, menu));
        return menu;
    }

    // R04：普通清单。
    if (ctx.isList) {
        QAction *rename = makeAction(EntityCommand::RenameList, objectId, menu);
        setActionText(rename, QStringLiteral("编辑"));
        menu->addAction(rename);
        QAction *pin = makeAction(EntityCommand::TogglePin, objectId, menu);
        setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
        menu->addAction(pin);
        QAction *copy = makeAction(EntityCommand::Clone, objectId, menu);
        setActionText(copy, QStringLiteral("复制"));
        menu->addAction(copy);
        menu->addAction(makeAction(EntityCommand::Share, objectId, menu));
        menu->addAction(makeAction(EntityCommand::Archive, objectId, menu));
        addSeparator(menu);
        QAction *remove = makeAction(EntityCommand::DeleteList, objectId, menu);
        setActionText(remove, QStringLiteral("删除"));
        menu->addAction(remove);
        return menu;
    }

    // R01：系统聚合入口只有显示策略。
    menu->addAction(makeAction(EntityCommand::Hide, objectId, menu));
    return menu;
}

QAction *EntityCommands::makeAction(EntityCommand cmd, const QString &entityId, QWidget *parent)
{
    const CommandInfo info = commandInfo(cmd);
    QAction *action = new QAction(AppIcons::icon(info.icon, 18), info.text, parent);
    action->setObjectName(QString::fromLatin1(info.objectName));
    action->setProperty("accessibleName", info.text);
    action->setToolTip(info.text);
    action->setStatusTip(info.text);
    action->setProperty("entityCommand", static_cast<int>(cmd));
    action->setProperty("entityId", entityId);
    action->setIconVisibleInMenu(true);
    if (cmd == EntityCommand::Count)
        action->setEnabled(false);

    // 直接从 Store 读取时能保证其他调用方使用 makeAction 也得到正确方向；
    // 菜单构建器仍按调用方给出的 MenuContext 覆盖文本。
    if (m_store && !entityId.isEmpty()) {
        const Entity entity = m_store->entity(entityId);
        if (!entity.id.isEmpty()) {
            if (cmd == EntityCommand::TogglePin) {
                setPinAction(action, entity.pinnedAt.isValid());
            } else if (cmd == EntityCommand::ConvertKind) {
                setActionText(action, entity.kind == EntityKind::Task ? QStringLiteral("转换为笔记")
                                                                       : QStringLiteral("转换为任务"));
            }
        }
    }

    connect(action, &QAction::triggered, this, [this, cmd, entityId]() { execute(cmd, entityId); });
    return action;
}

QMenu *EntityCommands::buildPriorityMenu(const QString &entityId, QWidget *parent)
{
    auto *menu = new PopupMenu(parent);
    menu->setObjectName(QStringLiteral("PriorityMenu"));
    menu->setProperty("entityMenu", true);
    menu->setFixedWidth(164 + 2 * PopupMenu::shadowMargin());
    menu->setSeparatorsCollapsible(false);
    const int current = m_store ? m_store->entity(entityId).priority : 0;
    const EntityCommand commands[] = {EntityCommand::SetPriorityHigh, EntityCommand::SetPriorityMedium,
                                      EntityCommand::SetPriorityLow, EntityCommand::SetPriorityNone};
    const int values[] = {3, 2, 1, 0};
    for (int i = 0; i < 4; ++i) {
        QAction* action = makeAction(commands[i], entityId, menu);
        action->setCheckable(true);
        action->setChecked(current == values[i]);
        menu->addAction(action);
    }
    return menu;
}

void EntityCommands::addQuickDateBar(QMenu *menu, const MenuContext &ctx, const QString &entityId)
{
    auto *host = new QuickSection(QStringLiteral("日期"), 22, menu);
    host->setObjectName(QStringLiteral("dateQuickBarWidget"));

    const auto addButton = [this, menu, host, &entityId](EntityCommand cmd, const QString &objectName) {
        QAction *action = makeAction(cmd, entityId, host);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setIconSize(QSize(22, 22));
        button->setFocusPolicy(Qt::StrongFocus);
        button->setDefaultAction(action);
        if (AppIcons::hasIcon(objectName))
            button->setIcon(AppIcons::icon(objectName, ds::ColorToken::TextSecondary, 22));
        button->setAccessibleName(action->property("accessibleName").toString());
        connect(action, &QAction::triggered, menu, [menu]() { menu->close(); });
        host->addButton(button);
    };

    Q_UNUSED(ctx);
    // 仅提供已经有明确数据语义的日期命令，不展示未定义的“下一周期”。
    addButton(EntityCommand::SetDateToday, QStringLiteral("quick-today"));
    addButton(EntityCommand::SetDateTomorrow, QStringLiteral("tomorrow-list"));
    addButton(EntityCommand::SetDateNextWeek, QStringLiteral("quick-nextweek"));
    addButton(EntityCommand::SetDateNone, QStringLiteral("date-unselected"));

    addWidgetAction(menu, host, QStringLiteral("dateQuickBar"), QStringLiteral("日期快捷设置"),
                    IconId::Calendar);
}

void EntityCommands::addPriorityBar(QMenu *menu, const MenuContext &ctx, const QString &entityId)
{
    Q_UNUSED(ctx);
    auto *host = new QuickSection(QStringLiteral("优先级"), 20, menu);
    host->setObjectName(QStringLiteral("priorityBarWidget"));
    const int currentPriority = m_store ? m_store->entity(entityId).priority : 0;

    const auto addButton = [this, menu, host, &entityId, currentPriority](
                               EntityCommand cmd, const QString &objectName, int priority) {
        QAction *action = makeAction(cmd, entityId, host);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(objectName);
        button->setProperty("quickPriority", true);
        button->setProperty("priorityValue", priority);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setIconSize(QSize(20, 20));
        button->setFocusPolicy(Qt::StrongFocus);
        button->setDefaultAction(action);
        button->setCheckable(true);
        button->setChecked(currentPriority == priority);
        button->setAccessibleName(action->property("accessibleName").toString());
        connect(action, &QAction::triggered, menu, [menu]() { menu->close(); });
        host->addButton(button);
    };
    addButton(EntityCommand::SetPriorityHigh, QStringLiteral("priority-3"), 3);
    addButton(EntityCommand::SetPriorityMedium, QStringLiteral("priority-2"), 2);
    addButton(EntityCommand::SetPriorityLow, QStringLiteral("priority-1"), 1);
    addButton(EntityCommand::SetPriorityNone, QStringLiteral("priority-0"), 0);

    addWidgetAction(menu, host, QStringLiteral("priorityBar"), QStringLiteral("优先级快捷设置"),
                    IconId::FlagHigh);
}

void EntityCommands::addMoveToMenu(QMenu *menu, const QString &entityId)
{
    QAction *moveAction = makeAction(EntityCommand::MoveTo, entityId, menu);
    auto *moveMenu = new ListDestinationMenu(menu);
    moveMenu->setObjectName(QStringLiteral("MoveToMenu"));
    moveAction->setMenu(moveMenu);
    menu->addAction(moveAction);
    moveMenu->setLists(m_store ? m_store->lists() : QVector<TaskList>(),
                       m_store ? m_store->folders() : QVector<Folder>(),
                       m_store ? m_store->entity(entityId).listId : QString());
    moveAction->setEnabled(m_store && !m_store->lists().isEmpty());
    connect(moveMenu, &ListDestinationMenu::listSelected, this,
            [this, entityId](const QString& listId) { executeMoveTo(entityId, listId); });
}

void EntityCommands::addFocusMenu(QMenu *menu, const QString &entityId)
{
    QAction *focusAction = makeAction(EntityCommand::StartFocus, entityId, menu);
    QMenu *focusMenu = newMenu(QStringLiteral("StartFocusMenu"), menu);
    focusAction->setMenu(focusMenu);
    menu->addAction(focusAction);

    // 考据只确认存在子层，未确认时长项；不虚构预设时长。
    QAction *start = makeAction(EntityCommand::StartFocus, entityId, focusMenu);
    start->setObjectName(QStringLiteral("startFocusSession"));
    focusMenu->addAction(start);
}

bool EntityCommands::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QObject::eventFilter(watched, event);

    QLineEdit *search = qobject_cast<QLineEdit *>(watched);
    QMenu *menu = search ? qobject_cast<QMenu *>(search->parentWidget()) : nullptr;
    if (!menu)
        return QObject::eventFilter(watched, event);

    QKeyEvent *key = static_cast<QKeyEvent *>(event);
    const QList<QAction *> destinations = visibleDestinations(menu);
    if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
        if (!destinations.isEmpty()) {
            QAction *target = key->key() == Qt::Key_Down ? destinations.first() : destinations.last();
            menu->setActiveAction(target);
            menu->setFocus(Qt::TabFocusReason);
        }
        return true;
    }
    if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        if (!destinations.isEmpty())
            destinations.first()->trigger();
        closeMoveMenus(menu);
        return true;
    }
    return QObject::eventFilter(watched, event);
}

void EntityCommands::execute(EntityCommand cmd, const QString &entityId)
{
    if (cmd == EntityCommand::Count) {
        emit errorOccurred(QStringLiteral("未知实体命令"));
        return;
    }

    QAction *sourceAction = qobject_cast<QAction *>(sender());
    if (sourceAction && sourceAction->property("batchCommand").toBool()) {
        // 批量命令仍统一经过本入口，但不能只写入代表项 entityId。
        emit commandExecuted(cmd, entityId);
        return;
    }

    const auto loadEntity = [this, &entityId](Entity *entity) {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return false;
        }
        *entity = m_store->entity(entityId);
        if (entity->id.isEmpty()) {
            emit errorOccurred(QStringLiteral("实体不存在"));
            return false;
        }
        return true;
    };

    switch (cmd) {
    case EntityCommand::SetDateToday:
    case EntityCommand::SetDateTomorrow:
    case EntityCommand::SetDateNextWeek:
    case EntityCommand::SetDateNone:
    case EntityCommand::ClearDate: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        ScheduleValue schedule = m_store->schedule(entityId);
        schedule.startAt = QDateTime();
        schedule.dueAt = QDateTime();
        schedule.allDayEndDate = QDate();
        schedule.hasScheduleRange = false;
        if (cmd == EntityCommand::SetDateNone || cmd == EntityCommand::ClearDate) {
            schedule.allDayDate = QDate();
            schedule.reminders.clear();
            schedule.repeating = false;
        } else {
            const QDate today = toAppTime(appNow()).date();
            int days = 0;
            if (cmd == EntityCommand::SetDateTomorrow)
                days = 1;
            else if (cmd == EntityCommand::SetDateNextWeek)
                days = 7;
            schedule.allDayDate = today.addDays(days);
        }
        finish(cmd, entityId, m_store->saveSchedule(entity, schedule));
        return;
    }
    case EntityCommand::SetPriorityHigh:
    case EntityCommand::SetPriorityMedium:
    case EntityCommand::SetPriorityLow:
    case EntityCommand::SetPriorityNone: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("笔记不支持优先级"));
            return;
        }
        if (cmd == EntityCommand::SetPriorityHigh)
            entity.priority = 3;
        else if (cmd == EntityCommand::SetPriorityMedium)
            entity.priority = 2;
        else if (cmd == EntityCommand::SetPriorityLow)
            entity.priority = 1;
        else
            entity.priority = 0;
        finish(cmd, entityId, m_store->saveTask(entity));
        return;
    }
    case EntityCommand::AddSubtask: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("仅任务可添加子任务"));
            return;
        }
        bool accepted = false;
        const QString title = QInputDialog::getText(dialogParent(), QStringLiteral("添加子任务"),
                                                    QStringLiteral("子任务标题："), QLineEdit::Normal,
                                                    QString(), &accepted).trimmed();
        if (!accepted)
            return;
        if (title.isEmpty()) {
            emit errorOccurred(QStringLiteral("子任务标题不能为空"));
            return;
        }
        const QString newId = m_store->createSubtask(title, entity.id);
        if (!newId.isEmpty()) emit entityCreated(newId);
        finish(cmd, entityId, !newId.isEmpty());
        return;
    }
    case EntityCommand::LinkParent: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("仅任务可关联主任务"));
            return;
        }
        const QPointer<EntityCommands> guard(this);
        const QPointer<TaskStore> store(m_store);
        EntityPickerDialog picker(m_store, dialogParent());
        picker.setWindowTitle(QStringLiteral("关联主任务"));
        if (picker.exec() != QDialog::Accepted || !guard || !store) return;
        const Entity parent = store->entity(picker.selectedEntity().id);
        Entity latest = store->entity(entityId);
        if (latest.id.isEmpty() || latest.deleted || latest.kind != EntityKind::Task) return;
        if (parent.id.isEmpty() || parent.kind != EntityKind::Task || parent.deleted
            || parent.id == latest.id) {
            emit errorOccurred(QStringLiteral("请选择另一项有效任务作为主任务"));
            return;
        }
        latest.parentId = parent.id;
        finish(cmd, entityId, store->saveTask(latest));
        return;
    }
    case EntityCommand::TogglePin: {
        const Entity entity = m_store ? m_store->entity(entityId) : Entity();
        if (entity.id.isEmpty()) {
            // 文件夹/清单/过滤器的置顶状态不属于实体 Store 契约。
            emit commandExecuted(cmd, entityId);
            return;
        }
        finish(cmd, entityId, m_store->setPinnedAt(entityId,
                                                  entity.pinnedAt.isValid() ? QDateTime() : appNow()));
        return;
    }
    case EntityCommand::Abandon: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("笔记不支持放弃状态"));
            return;
        }
        finish(cmd, entityId, m_store->abandonTask(entityId));
        return;
    }
    case EntityCommand::MoveTo: {
        if (m_moveTargetListId.isEmpty()) {
            // 无目标时让宿主打开等价的选择器；子菜单目标仍回到本入口。
            emit commandExecuted(cmd, entityId);
            return;
        }
        Entity entity;
        if (!loadEntity(&entity))
            return;
        finish(cmd, entityId, m_store->moveEntityToList(entityId, m_moveTargetListId));
        return;
    }
    case EntityCommand::Complete: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("笔记没有完成状态"));
            return;
        }
        finish(cmd, entityId, m_store->setCompleted(entityId, !entity.completed));
        return;
    }
    case EntityCommand::Reactivate: {
        Entity entity;
        if (!loadEntity(&entity)) return;
        if (entity.kind != EntityKind::Task) {
            emit errorOccurred(QStringLiteral("笔记没有任务状态"));
            return;
        }
        finish(cmd, entityId, m_store->setStatus(entityId, TaskStatus::Active));
        return;
    }
    case EntityCommand::Clone: {
        const Entity entity = m_store ? m_store->entity(entityId) : Entity();
        if (entity.id.isEmpty()) {
            // 清单/过滤器复制需要宿主提供其定义，不能误用 cloneTask。
            emit commandExecuted(cmd, entityId);
            return;
        }
        const QString newId = m_store->cloneTask(entityId);
        if (!newId.isEmpty()) emit entityCreated(newId);
        finish(cmd, entityId, !newId.isEmpty());
        return;
    }
    case EntityCommand::CopyLink: {
        Entity entity;
        if (!loadEntity(&entity)) return;
        const QString reference = entityReference(entity.id, entity.title);
        if (reference.isEmpty()) {
            emit errorOccurred(QStringLiteral("实体不存在"));
            return;
        }
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (!clipboard) {
            emit errorOccurred(QStringLiteral("剪贴板不可用"));
            return;
        }
        clipboard->setText(reference);
        emit commandExecuted(cmd, entityId);
        return;
    }
    case EntityCommand::ConvertKind: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        const EntityKind target = entity.kind == EntityKind::Task ? EntityKind::Note : EntityKind::Task;
        if (m_store->convertKind(entityId, target)) {
            finish(EntityCommand::ConvertKind, entityId, true);
            if (QWidget* parent = dialogParent()) ToastIsland::showTransient(parent->window(), QStringLiteral("已转换"));
        } else {
            finish(EntityCommand::ConvertKind, entityId, false);
        }
        return;
    }
    case EntityCommand::Delete: {
        const Entity entity = m_store ? m_store->entity(entityId) : Entity();
        if (entity.id.isEmpty()) {
            // 过滤器删除由宿主处理；普通实体统一进入回收站。
            emit commandExecuted(cmd, entityId);
            return;
        }
        finish(cmd, entityId, m_store->trashTask(entityId));
        return;
    }
    case EntityCommand::Restore: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        finish(cmd, entityId, m_store->restoreTask(entityId));
        return;
    }
    case EntityCommand::DeletePermanently: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        if (QMessageBox::question(dialogParent(), QStringLiteral("永久删除"),
                                  QStringLiteral("永久删除后无法恢复，其子任务也会一并删除。是否继续？"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        finish(cmd, entityId, m_store->deletePermanently(entityId));
        return;
    }
    case EntityCommand::UploadAttachment:
    case EntityCommand::ViewActivity: {
        Entity entity;
        if (!loadEntity(&entity)) return;
        if (cmd == EntityCommand::UploadAttachment && entity.deleted) {
            emit errorOccurred(QStringLiteral("回收站中的内容不能添加附件"));
            return;
        }
        // 宿主将附件交给共享导入组件，将动态展开在当前详情内。
        emit commandExecuted(cmd, entityId);
        return;
    }
    case EntityCommand::RenameList: {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return;
        }
        QString currentName;
        for (const TaskList &list : m_store->lists()) {
            if (list.id == entityId)
                currentName = list.name;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(dialogParent(), QStringLiteral("编辑清单"),
                                                   QStringLiteral("清单名称："), QLineEdit::Normal,
                                                   currentName, &accepted).trimmed();
        if (!accepted)
            return;
        if (name.isEmpty()) {
            emit errorOccurred(QStringLiteral("清单名称不能为空"));
            return;
        }
        finish(cmd, entityId, m_store->renameList(entityId, name));
        return;
    }
    case EntityCommand::DeleteList: {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return;
        }
        if (QMessageBox::question(dialogParent(), QStringLiteral("删除清单"),
                                  QStringLiteral("删除清单后，其中所有任务和笔记会移入垃圾桶，可在垃圾桶中恢复。是否继续？"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        finish(cmd, entityId, m_store->deleteList(entityId));
        return;
    }
    case EntityCommand::RenameFolder: {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return;
        }
        QString currentName;
        for (const Folder &folder : m_store->folders()) {
            if (folder.id == entityId)
                currentName = folder.name;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(dialogParent(), QStringLiteral("编辑文件夹"),
                                                   QStringLiteral("文件夹名称："), QLineEdit::Normal,
                                                   currentName, &accepted).trimmed();
        if (!accepted)
            return;
        if (name.isEmpty()) {
            emit errorOccurred(QStringLiteral("文件夹名称不能为空"));
            return;
        }
        finish(cmd, entityId, m_store->renameFolder(entityId, name));
        return;
    }
    case EntityCommand::DeleteFolder: {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return;
        }
        if (QMessageBox::question(dialogParent(), QStringLiteral("解散文件夹"),
                                  QStringLiteral("解散后，其中清单会平铺显示，清单和实体内容均保留。是否继续？"),
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        finish(cmd, entityId, m_store->deleteFolder(entityId));
        return;
    }
    case EntityCommand::CreateListInFolder: {
        if (!m_store) {
            emit errorOccurred(QStringLiteral("命令服务不可用"));
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(dialogParent(), QStringLiteral("添加清单"),
                                                   QStringLiteral("清单名称："), QLineEdit::Normal,
                                                   QString(), &accepted).trimmed();
        if (!accepted)
            return;
        if (name.isEmpty()) {
            emit errorOccurred(QStringLiteral("清单名称不能为空"));
            return;
        }
        finish(cmd, entityId, !m_store->createList(name, entityId).isEmpty());
        return;
    }

    // 下列命令需要选择器、批量选中集合、专注/视图状态或输出契约。
    // 仍统一经过 execute()，由宿主接收 commandExecuted 后完成对应 UI 操作。
    case EntityCommand::SetDateNextPeriod:
    case EntityCommand::EditTags:
    case EntityCommand::StartFocus:
    case EntityCommand::SaveAsTemplate:
    case EntityCommand::Export:
    case EntityCommand::EditCompletionTime:
    case EntityCommand::CopyText:
    case EntityCommand::Merge:
    case EntityCommand::Edit:
    case EntityCommand::ShowWhenNonEmpty:
    case EntityCommand::Hide:
    case EntityCommand::Share:
    case EntityCommand::Archive:
        emit commandExecuted(cmd, entityId);
        return;
    case EntityCommand::Count:
        return;
    }
}

void EntityCommands::executeMoveTo(const QString &entityId, const QString &listId)
{
    const QString previous = m_moveTargetListId;
    m_moveTargetListId = listId;
    execute(EntityCommand::MoveTo, entityId);
    m_moveTargetListId = previous;
}

QWidget *EntityCommands::dialogParent() const
{
    QWidget *widget = qobject_cast<QWidget *>(parent());
    return widget ? widget : QApplication::activeWindow();
}

bool EntityCommands::finish(EntityCommand cmd, const QString &entityId, bool success)
{
    if (!success) {
        QString message = m_store ? m_store->lastError() : QStringLiteral("命令服务不可用");
        if (message.trimmed().isEmpty())
            message = QStringLiteral("命令执行失败");
        emit errorOccurred(message);
        return false;
    }
    emit commandExecuted(cmd, entityId);
    return true;
}

} // namespace kl
