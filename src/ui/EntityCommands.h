#ifndef KYLINLIST_ENTITYCOMMANDS_H
#define KYLINLIST_ENTITYCOMMANDS_H

#include "../domain/Entity.h"

#include <QObject>
#include <QString>

class QAction;
class QEvent;
class QMenu;
class QWidget;

namespace kl {

class TaskStore;

// 所有入口共用命令标识；菜单集合由对象状态和入口共同决定。
enum class EntityCommand {
    // 属性快捷
    SetDateToday, SetDateTomorrow, SetDateNextWeek, SetDateNextPeriod, SetDateNone, ClearDate,
    SetPriorityHigh, SetPriorityMedium, SetPriorityLow, SetPriorityNone,
    // 主命令
    AddSubtask, LinkParent, TogglePin, Abandon, MoveTo, EditTags,
    // 专注（模块门控）
    StartFocus,
    // 复用
    Clone, CopyLink, ConvertKind, Delete,
    // 回收站
    Restore, DeletePermanently,
    // 详情更多特有
    UploadAttachment, ViewActivity, SaveAsTemplate, Export, Print,
    // 已完成与多选
    EditCompletionTime, Complete, CopyText, Merge,
    // 视图级
    ViewList, ViewBoard, ViewTime, HideCompleted, ShowDetail, DisplaySettings,
    // 侧栏
    RenameList, DeleteList, RenameFolder, DeleteFolder, CreateListInFolder,
    Edit, ShowWhenNonEmpty, Hide, Share, Archive,
    Count
};

// 菜单上下文：描述当前对象状态。非多选入口保持 selectedCount == 1。
struct MenuContext {
    EntityKind kind = EntityKind::Task;
    bool completed = false;
    bool deleted = false;
    bool pinned = false;
    bool hasDate = false;
    bool focusEnabled = false;
    bool isInbox = false;
    bool isFolder = false;
    bool isList = false;
    bool isFilter = false;
    int selectedCount = 1;
};

// 菜单只负责呈现；命令统一经 execute() 分发，业务失败统一由 errorOccurred 报告。
class EntityCommands : public QObject
{
    Q_OBJECT
public:
    explicit EntityCommands(TaskStore *store, QObject *parent = nullptr);

    // 任务/笔记右键；回收站与多选使用各自的最小命令集。
    QMenu *buildEntityMenu(const MenuContext &ctx, const QString &entityId, QWidget *parent);
    // R11 详情底部更多；不重复日期、优先级和移动到入口。
    QMenu *buildDetailMoreMenu(const MenuContext &ctx, const QString &entityId, QWidget *parent);
    // R01-R05 侧栏对象菜单。
    QMenu *buildNavigationMenu(const MenuContext &ctx, const QString &objectId, QWidget *parent);
    // R12 列表/看板工作区更多，仅管理“怎么看”。
    QMenu *buildWorkspaceMenu(QWidget *parent);

    // 唯一命令执行入口。flush() 等编辑提交由调用方在触发前负责。
    // 无 TaskStore 业务契约的界面命令通过 commandExecuted 交由宿主继续处理。
    void execute(EntityCommand cmd, const QString &entityId);

signals:
    void commandExecuted(kl::EntityCommand cmd, QString entityId);
    void errorOccurred(QString message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QAction *makeAction(EntityCommand cmd, const QString &entityId, QWidget *parent);
    void addQuickDateBar(QMenu *menu, const MenuContext &ctx, const QString &entityId);
    void addPriorityBar(QMenu *menu, const MenuContext &ctx, const QString &entityId);
    void addViewBar(QMenu *menu);
    void addMoveToMenu(QMenu *menu, const QString &entityId);
    void addFocusMenu(QMenu *menu, const QString &entityId);
    void executeMoveTo(const QString &entityId, const QString &listId);
    QWidget *dialogParent() const;
    bool finish(EntityCommand cmd, const QString &entityId, bool success);

    TaskStore *m_store;
    QString m_moveTargetListId;
};

} // namespace kl

#endif // KYLINLIST_ENTITYCOMMANDS_H
