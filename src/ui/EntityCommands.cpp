#include "EntityCommands.h"

#include "../app/TaskStore.h"
#include "ActivityLogDialog.h"
#include "AppIcons.h"
#include "EntityPicker.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
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
        return {"addSubtask", QStringLiteral("添加子任务"), IconId::Add};
    case EntityCommand::LinkParent:
        return {"linkParent", QStringLiteral("关联主任务"), IconId::Link};
    case EntityCommand::TogglePin:
        return {"togglePin", QStringLiteral("置顶"), IconId::Pin};
    case EntityCommand::Abandon:
        return {"abandon", QStringLiteral("放弃"), IconId::Abandoned};
    case EntityCommand::MoveTo:
        return {"moveTo", QStringLiteral("移动到"), IconId::Folder};
    case EntityCommand::EditTags:
        return {"editTags", QStringLiteral("标签"), IconId::Tag};
    case EntityCommand::StartFocus:
        return {"startFocus", QStringLiteral("开始专注"), IconId::Clock};
    case EntityCommand::Clone:
        return {"clone", QStringLiteral("创建副本"), IconId::Add};
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
    case EntityCommand::Print:
        return {"print", QStringLiteral("打印"), IconId::TaskList};
    case EntityCommand::EditCompletionTime:
        return {"editCompletionTime", QStringLiteral("修改完成时间"), IconId::Clock};
    case EntityCommand::Complete:
        return {"complete", QStringLiteral("完成"), IconId::Check};
    case EntityCommand::CopyText:
        return {"copyText", QStringLiteral("复制文本"), IconId::Note};
    case EntityCommand::Merge:
        return {"merge", QStringLiteral("合并"), IconId::Checklist};
    case EntityCommand::ViewList:
        return {"viewList", QStringLiteral("列表"), IconId::List};
    case EntityCommand::ViewBoard:
        return {"viewBoard", QStringLiteral("看板"), IconId::Checklist};
    case EntityCommand::ViewTime:
        return {"viewTime", QStringLiteral("时间线"), IconId::Calendar};
    case EntityCommand::HideCompleted:
        return {"hideCompleted", QStringLiteral("隐藏已完成"), IconId::Done};
    case EntityCommand::ShowDetail:
        return {"showDetail", QStringLiteral("显示详细"), IconId::Note};
    case EntityCommand::DisplaySettings:
        return {"displaySettings", QStringLiteral("显示设置"), IconId::Settings};
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
        return {"archive", QStringLiteral("归档"), IconId::Backup};
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

QMenu *newMenu(const QString &objectName, QWidget *parent)
{
    QMenu *menu = new QMenu(parent);
    menu->setObjectName(objectName);
    menu->setToolTipsVisible(true);
    menu->setSeparatorsCollapsible(false);
    // QssBuilder 已统一提供 12px 外框、34px item 与 8px hover 圆角。
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

    // R26：多选不是单选菜单的重复，尤其“复制文本”不能换成“复制链接”。
    if (ctx.selectedCount > 1) {
        addQuickDateBar(menu, ctx, entityId);
        if (isTask)
            addPriorityBar(menu, ctx, entityId);
        addSeparator(menu);

        QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
        setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
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

    // R09 已完成任务没有添加子任务；R08 笔记没有子任务、主任务和放弃。
    if (isTask && !ctx.completed)
        menu->addAction(makeAction(EntityCommand::AddSubtask, entityId, menu));
    if (isTask)
        menu->addAction(makeAction(EntityCommand::LinkParent, entityId, menu));

    QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
    setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    menu->addAction(pin);

    if (isTask)
        menu->addAction(makeAction(EntityCommand::Abandon, entityId, menu));
    addMoveToMenu(menu, entityId);
    menu->addAction(makeAction(EntityCommand::EditTags, entityId, menu));

    addSeparator(menu);
    if (ctx.focusEnabled) {
        addFocusMenu(menu, entityId);
        addSeparator(menu);
    }

    // R09 已完成任务只保留复制链接、修改完成时间和删除。
    if (!completedTask) {
        menu->addAction(makeAction(EntityCommand::Clone, entityId, menu));
        menu->addAction(makeAction(EntityCommand::CopyLink, entityId, menu));
        QAction *convert = makeAction(EntityCommand::ConvertKind, entityId, menu);
        setActionText(convert, isTask ? QStringLiteral("转换为笔记") : QStringLiteral("转换为任务"));
        menu->addAction(convert);
    } else {
        menu->addAction(makeAction(EntityCommand::CopyLink, entityId, menu));
        menu->addAction(makeAction(EntityCommand::EditCompletionTime, entityId, menu));
    }
    menu->addAction(makeAction(EntityCommand::Delete, entityId, menu));
    return menu;
}

QMenu *EntityCommands::buildDetailMoreMenu(const MenuContext &ctx, const QString &entityId,
                                           QWidget *parent)
{
    QMenu *menu = newMenu(QStringLiteral("EntityDetailMoreMenu"), parent);
    if (ctx.deleted) {
        menu->addAction(makeAction(EntityCommand::Restore, entityId, menu));
        addSeparator(menu);
        menu->addAction(makeAction(EntityCommand::DeletePermanently, entityId, menu));
        return menu;
    }

    const bool isTask = ctx.kind == EntityKind::Task;
    const bool completedTask = isTask && ctx.completed;

    // R11：添加子任务、关联主任务、置顶、放弃、标签、上传附件。
    if (isTask && !ctx.completed)
        menu->addAction(makeAction(EntityCommand::AddSubtask, entityId, menu));
    if (isTask)
        menu->addAction(makeAction(EntityCommand::LinkParent, entityId, menu));
    QAction *pin = makeAction(EntityCommand::TogglePin, entityId, menu);
    setActionText(pin, ctx.pinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    menu->addAction(pin);
    if (isTask)
        menu->addAction(makeAction(EntityCommand::Abandon, entityId, menu));
    menu->addAction(makeAction(EntityCommand::EditTags, entityId, menu));
    menu->addAction(makeAction(EntityCommand::UploadAttachment, entityId, menu));

    addSeparator(menu);
    menu->addAction(makeAction(EntityCommand::ViewActivity, entityId, menu));
    menu->addAction(makeAction(EntityCommand::SaveAsTemplate, entityId, menu));
    if (!completedTask)
        menu->addAction(makeAction(EntityCommand::Clone, entityId, menu));
    menu->addAction(makeAction(EntityCommand::CopyLink, entityId, menu));
    if (completedTask)
        menu->addAction(makeAction(EntityCommand::EditCompletionTime, entityId, menu));
    if (!completedTask) {
        QAction *convert = makeAction(EntityCommand::ConvertKind, entityId, menu);
        setActionText(convert, isTask ? QStringLiteral("转换为笔记") : QStringLiteral("转换为任务"));
        menu->addAction(convert);
    }
    menu->addAction(makeAction(EntityCommand::Export, entityId, menu));
    menu->addAction(makeAction(EntityCommand::Print, entityId, menu));
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

QMenu *EntityCommands::buildWorkspaceMenu(QWidget *parent)
{
    QMenu *menu = newMenu(QStringLiteral("WorkspaceMoreMenu"), parent);
    addViewBar(menu);
    addSeparator(menu);
    menu->addAction(makeAction(EntityCommand::HideCompleted, QString(), menu));
    menu->addAction(makeAction(EntityCommand::ShowDetail, QString(), menu));
    menu->addAction(makeAction(EntityCommand::DisplaySettings, QString(), menu));
    menu->addAction(makeAction(EntityCommand::Print, QString(), menu));
    return menu;
}

QAction *EntityCommands::makeAction(EntityCommand cmd, const QString &entityId, QWidget *parent)
{
    const CommandInfo info = commandInfo(cmd);
    QAction *action = new QAction(AppIcons::icon(info.icon), info.text, parent);
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
                setActionText(action, entity.pinnedAt.isValid() ? QStringLiteral("取消置顶")
                                                                 : QStringLiteral("置顶"));
            } else if (cmd == EntityCommand::ConvertKind) {
                setActionText(action, entity.kind == EntityKind::Task ? QStringLiteral("转换为笔记")
                                                                       : QStringLiteral("转换为任务"));
            }
        }
    }

    connect(action, &QAction::triggered, this, [this, cmd, entityId]() { execute(cmd, entityId); });
    return action;
}

void EntityCommands::addQuickDateBar(QMenu *menu, const MenuContext &ctx, const QString &entityId)
{
    QWidget *host = new QWidget(menu);
    host->setObjectName(QStringLiteral("dateQuickBarWidget"));
    QHBoxLayout *layout = new QHBoxLayout(host);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(2);

    const auto addButton = [this, menu, host, layout, &entityId](EntityCommand cmd, const QString &objectName) {
        QAction *action = makeAction(cmd, entityId, host);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFixedSize(34, 34);
        button->setIconSize(QSize(18, 18));
        button->setFocusPolicy(Qt::StrongFocus);
        button->setDefaultAction(action);
        button->setAccessibleName(action->property("accessibleName").toString());
        connect(action, &QAction::triggered, menu, [menu]() { menu->close(); });
        layout->addWidget(button);
    };

    // 与考据 SVG 名保持一致，便于自动化与可访问性检查定位。
    addButton(EntityCommand::SetDateToday, QStringLiteral("quick-today"));
    addButton(EntityCommand::SetDateTomorrow, QStringLiteral("tomorrow-list"));
    addButton(EntityCommand::SetDateNextWeek, QStringLiteral("quick-nextweek"));
    addButton(EntityCommand::SetDateNextPeriod, QStringLiteral("quick-nextperiod"));
    if (ctx.hasDate) {
        addButton(EntityCommand::SetDateNone, QStringLiteral("date-unselected"));
        addButton(EntityCommand::ClearDate, QStringLiteral("quick-clear"));
    } else {
        // R06 无日期样本的省略号为 disabled 溢出项，不可点击。
        QAction *overflow = new QAction(AppIcons::icon(IconId::More), QStringLiteral("更多日期"), host);
        overflow->setObjectName(QStringLiteral("dateOverflowAction"));
        overflow->setProperty("accessibleName", QStringLiteral("更多日期"));
        overflow->setToolTip(QStringLiteral("更多日期"));
        overflow->setEnabled(false);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(QStringLiteral("date-overflow"));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFixedSize(34, 34);
        button->setIconSize(QSize(18, 18));
        button->setDefaultAction(overflow);
        button->setAccessibleName(overflow->property("accessibleName").toString());
        layout->addWidget(button);
    }

    addWidgetAction(menu, host, QStringLiteral("dateQuickBar"), QStringLiteral("日期快捷设置"),
                    IconId::Calendar);
}

void EntityCommands::addPriorityBar(QMenu *menu, const MenuContext &ctx, const QString &entityId)
{
    Q_UNUSED(ctx);
    QWidget *host = new QWidget(menu);
    host->setObjectName(QStringLiteral("priorityBarWidget"));
    QHBoxLayout *layout = new QHBoxLayout(host);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(2);

    const auto addButton = [this, menu, host, layout, &entityId](EntityCommand cmd, const QString &objectName) {
        QAction *action = makeAction(cmd, entityId, host);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFixedSize(34, 34);
        button->setIconSize(QSize(18, 18));
        button->setFocusPolicy(Qt::StrongFocus);
        button->setDefaultAction(action);
        button->setAccessibleName(action->property("accessibleName").toString());
        connect(action, &QAction::triggered, menu, [menu]() { menu->close(); });
        layout->addWidget(button);
    };
    addButton(EntityCommand::SetPriorityHigh, QStringLiteral("priority-3"));
    addButton(EntityCommand::SetPriorityMedium, QStringLiteral("priority-2"));
    addButton(EntityCommand::SetPriorityLow, QStringLiteral("priority-1"));
    addButton(EntityCommand::SetPriorityNone, QStringLiteral("priority-0"));

    addWidgetAction(menu, host, QStringLiteral("priorityBar"), QStringLiteral("优先级快捷设置"),
                    IconId::FlagHigh);
}

void EntityCommands::addViewBar(QMenu *menu)
{
    QWidget *host = new QWidget(menu);
    host->setObjectName(QStringLiteral("workspaceViewBarWidget"));
    QHBoxLayout *layout = new QHBoxLayout(host);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(2);

    const auto addButton = [this, menu, host, layout](EntityCommand cmd, const QString &objectName) {
        QAction *action = makeAction(cmd, QString(), host);
        QToolButton *button = new QToolButton(host);
        button->setObjectName(objectName);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFixedSize(34, 34);
        button->setIconSize(QSize(18, 18));
        button->setFocusPolicy(Qt::StrongFocus);
        button->setDefaultAction(action);
        button->setAccessibleName(action->property("accessibleName").toString());
        connect(action, &QAction::triggered, menu, [menu]() { menu->close(); });
        layout->addWidget(button);
    };
    addButton(EntityCommand::ViewList, QStringLiteral("view-list"));
    addButton(EntityCommand::ViewBoard, QStringLiteral("view-board"));
    addButton(EntityCommand::ViewTime, QStringLiteral("view-timeline"));

    QAction *overflow = new QAction(AppIcons::icon(IconId::More), QStringLiteral("更多视图"), host);
    overflow->setObjectName(QStringLiteral("viewOverflowAction"));
    overflow->setProperty("accessibleName", QStringLiteral("更多视图"));
    overflow->setToolTip(QStringLiteral("更多视图"));
    overflow->setEnabled(false);
    QToolButton *button = new QToolButton(host);
    button->setObjectName(QStringLiteral("view-overflow"));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFixedSize(34, 34);
    button->setIconSize(QSize(18, 18));
    button->setDefaultAction(overflow);
    layout->addWidget(button);

    addWidgetAction(menu, host, QStringLiteral("workspaceViewBar"), QStringLiteral("工作区视图"),
                    IconId::List);
}

void EntityCommands::addMoveToMenu(QMenu *menu, const QString &entityId)
{
    QAction *moveAction = makeAction(EntityCommand::MoveTo, entityId, menu);
    QMenu *moveMenu = newMenu(QStringLiteral("MoveToMenu"), menu);
    moveAction->setMenu(moveMenu);
    menu->addAction(moveAction);

    QLineEdit *search = new QLineEdit(moveMenu);
    search->setObjectName(QStringLiteral("moveToSearch"));
    search->setPlaceholderText(QStringLiteral("搜索清单"));
    search->setClearButtonEnabled(true);
    search->setAccessibleName(QStringLiteral("搜索目标清单"));
    search->setToolTip(QStringLiteral("输入名称筛选；按向下键选择，回车移动"));
    search->installEventFilter(this);
    QWidgetAction *searchAction = new QWidgetAction(moveMenu);
    searchAction->setObjectName(QStringLiteral("moveToSearchAction"));
    searchAction->setProperty("accessibleName", QStringLiteral("搜索目标清单"));
    searchAction->setToolTip(QStringLiteral("搜索目标清单"));
    searchAction->setIcon(AppIcons::icon(IconId::Search));
    searchAction->setDefaultWidget(search);
    moveMenu->addAction(searchAction);
    addSeparator(moveMenu);

    QVector<TaskList> lists = m_store ? m_store->lists() : QVector<TaskList>();
    QVector<Folder> folders = m_store ? m_store->folders() : QVector<Folder>();
    std::sort(lists.begin(), lists.end(), [](const TaskList &a, const TaskList &b) {
        if (a.sortOrder != b.sortOrder)
            return a.sortOrder < b.sortOrder;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    std::sort(folders.begin(), folders.end(), [](const Folder &a, const Folder &b) {
        if (a.sortOrder != b.sortOrder)
            return a.sortOrder < b.sortOrder;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });

    const QString currentListId = m_store ? m_store->entity(entityId).listId : QString();
    const auto addDestination = [this, moveMenu, entityId, currentListId](const TaskList &list,
                                                                          const QString &folderId,
                                                                          bool indented) {
        QAction *action = new QAction(AppIcons::icon(list.kind == EntityKind::Note ? IconId::Note
                                                                                   : IconId::TaskList),
                                      indented ? QStringLiteral("    ") + list.name : list.name,
                                      moveMenu);
        action->setObjectName(QStringLiteral("moveToList.%1").arg(list.id));
        action->setProperty("accessibleName", QStringLiteral("移动到%1").arg(list.name));
        action->setToolTip(QStringLiteral("移动到“%1”").arg(list.name));
        action->setStatusTip(QStringLiteral("移动到“%1”").arg(list.name));
        action->setProperty("targetListId", list.id);
        action->setProperty("targetListName", list.name);
        action->setProperty("moveFolderId", folderId);
        action->setEnabled(list.id != currentListId);
        moveMenu->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, entityId, listId = list.id]() { executeMoveTo(entityId, listId); });
    };

    // v5 只有一层文件夹；文件夹标题不可作为目标，清单才是目标节点。
    for (const Folder &folder : folders) {
        QAction *heading = new QAction(AppIcons::icon(IconId::Folder), folder.name, moveMenu);
        heading->setObjectName(QStringLiteral("moveFolder.%1").arg(folder.id));
        heading->setProperty("accessibleName", QStringLiteral("文件夹%1").arg(folder.name));
        heading->setToolTip(QStringLiteral("文件夹：%1").arg(folder.name));
        heading->setProperty("moveFolderHeading", folder.id);
        heading->setProperty("moveFolderName", folder.name);
        heading->setEnabled(false);
        moveMenu->addAction(heading);
        for (const TaskList &list : lists) {
            if (list.folderId == folder.id)
                addDestination(list, folder.id, true);
        }
    }
    for (const TaskList &list : lists) {
        if (list.folderId.isEmpty())
            addDestination(list, QString(), false);
    }

    QAction *empty = new QAction(AppIcons::icon(IconId::Search), QStringLiteral("没有匹配清单"), moveMenu);
    empty->setObjectName(QStringLiteral("moveToEmpty"));
    empty->setProperty("accessibleName", QStringLiteral("没有匹配清单"));
    empty->setToolTip(QStringLiteral("没有匹配清单"));
    empty->setEnabled(false);
    empty->setVisible(false);
    moveMenu->addAction(empty);
    moveAction->setEnabled(!lists.isEmpty());

    connect(search, &QLineEdit::textChanged, moveMenu, [moveMenu, empty](const QString &text) {
        const QString needle = text.trimmed();
        QSet<QString> visibleFolders;
        int visibleCount = 0;
        const QList<QAction *> actions = moveMenu->actions();
        for (QAction *action : actions) {
            const QString listId = action->property("targetListId").toString();
            if (listId.isEmpty())
                continue;
            const QString folderId = action->property("moveFolderId").toString();
            QString folderName;
            for (QAction *candidate : actions) {
                if (candidate->property("moveFolderHeading").toString() == folderId) {
                    folderName = candidate->property("moveFolderName").toString();
                    break;
                }
            }
            const bool matches = needle.isEmpty()
                || action->property("targetListName").toString().contains(needle, Qt::CaseInsensitive)
                || folderName.contains(needle, Qt::CaseInsensitive);
            action->setVisible(matches);
            if (matches) {
                ++visibleCount;
                if (!folderId.isEmpty())
                    visibleFolders.insert(folderId);
            }
        }
        for (QAction *action : actions) {
            const QString folderId = action->property("moveFolderHeading").toString();
            if (!folderId.isEmpty())
                action->setVisible(visibleFolders.contains(folderId));
        }
        empty->setVisible(visibleCount == 0);
    });
    connect(moveMenu, &QMenu::aboutToShow, search, [search]() {
        QTimer::singleShot(0, search, [search]() { search->setFocus(Qt::TabFocusReason); });
    });
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
        if (cmd == EntityCommand::SetDateNone || cmd == EntityCommand::ClearDate) {
            entity.allDayDate = QDate();
            entity.startAt = QDateTime();
            entity.dueAt = QDateTime();
        } else {
            const QDate today = toAppTime(appNow()).date();
            int days = 0;
            if (cmd == EntityCommand::SetDateTomorrow)
                days = 1;
            else if (cmd == EntityCommand::SetDateNextWeek)
                days = 7;
            entity.allDayDate = today.addDays(days);
            entity.startAt = QDateTime();
            entity.dueAt = QDateTime();
        }
        finish(cmd, entityId, m_store->saveTask(entity));
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
        const QString newId = m_store->createTask(title, entity.listId, entity.sectionId, entity.id);
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
        EntityPickerDialog picker(m_store, dialogParent());
        picker.setWindowTitle(QStringLiteral("关联主任务"));
        if (picker.exec() != QDialog::Accepted)
            return;
        const Entity parent = picker.selectedEntity();
        if (parent.id.isEmpty() || parent.kind != EntityKind::Task || parent.deleted
            || parent.id == entity.id) {
            emit errorOccurred(QStringLiteral("请选择另一项有效任务作为主任务"));
            return;
        }
        entity.parentId = parent.id;
        finish(cmd, entityId, m_store->saveTask(entity));
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
        entity.listId = m_moveTargetListId;
        entity.sectionId.clear();
        finish(cmd, entityId, m_store->saveTask(entity));
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
        finish(cmd, entityId, m_store->setCompleted(entityId, true));
        return;
    }
    case EntityCommand::Clone: {
        const Entity entity = m_store ? m_store->entity(entityId) : Entity();
        if (entity.id.isEmpty()) {
            // 清单/过滤器复制需要宿主提供其定义，不能误用 cloneTask。
            emit commandExecuted(cmd, entityId);
            return;
        }
        finish(cmd, entityId, !m_store->cloneTask(entityId).isEmpty());
        return;
    }
    case EntityCommand::CopyLink: {
        if (entityId.isEmpty()) {
            emit errorOccurred(QStringLiteral("实体不存在"));
            return;
        }
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (!clipboard) {
            emit errorOccurred(QStringLiteral("剪贴板不可用"));
            return;
        }
        // 与 Markdown 内链一致：kl:<entity-id>。
        clipboard->setText(QStringLiteral("kl:") + entityId);
        emit commandExecuted(cmd, entityId);
        return;
    }
    case EntityCommand::ConvertKind: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        const EntityKind target = entity.kind == EntityKind::Task ? EntityKind::Note : EntityKind::Task;
        const QString text = target == EntityKind::Note
            ? QStringLiteral("完成状态、日期、提醒、重复、优先级和贴桌面行为将暂停；字段、提醒、附件与清单均原样保留，转回任务后恢复。是否继续？")
            : QStringLiteral("将恢复任务状态、日期、提醒、重复、优先级及贴桌面行为；字段、提醒、附件与清单均已保留。是否继续？");
        if (QMessageBox::question(dialogParent(), QStringLiteral("类型转换"), text,
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        finish(cmd, entityId, m_store->convertKind(entityId, target));
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
    case EntityCommand::UploadAttachment: {
        Entity entity;
        if (!loadEntity(&entity))
            return;
        const QString path = QFileDialog::getOpenFileName(dialogParent(), QStringLiteral("上传附件"));
        if (path.isEmpty())
            return;
        const Attachment attachment = m_store->importAttachment(entityId, path);
        finish(cmd, entityId, !attachment.id.isEmpty());
        return;
    }
    case EntityCommand::ViewActivity: {
        if (!m_store || entityId.isEmpty()) {
            emit errorOccurred(QStringLiteral("任务动态不可用"));
            return;
        }
        ActivityLogDialog dialog(m_store, entityId, dialogParent());
        dialog.exec();
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
                                  QStringLiteral("删除后，其中实体将移回系统收集箱，内容不会删除。是否继续？"),
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
    case EntityCommand::Print:
    case EntityCommand::EditCompletionTime:
    case EntityCommand::CopyText:
    case EntityCommand::Merge:
    case EntityCommand::ViewList:
    case EntityCommand::ViewBoard:
    case EntityCommand::ViewTime:
    case EntityCommand::HideCompleted:
    case EntityCommand::ShowDetail:
    case EntityCommand::DisplaySettings:
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
