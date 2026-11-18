#ifndef KYLINLIST_ENTITYTEXTSESSION_H
#define KYLINLIST_ENTITYTEXTSESSION_H

#include "../domain/TextDraft.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>

namespace kl {

class TaskStore;

// 每个实体一个、由 TaskStore 持有并共享的正文编辑会话（TaskDetailWidget 与桌面便签共用同一份）。
// 会话是 title/body 的唯一权威草稿来源：任何视图的修改立即写回这里并同步广播 textChanged(origin)，
// 其余视图据此镜像刷新，不等待任何持久化。持久化按 450ms 防抖自动进行（flush() 可强制立即），
// 走既有 commitTextDraft 三方合并链：只写真正改变的字段，不相关元数据一律沿用远端。
//
// 撤销/重做按字段独立、全视图共享：历史存 {start,removed,inserted} 差分（不复制整篇文本），
// 每字段上限 200 条；merge=true 仅在“同一存活 origin + 相邻插入/删除 + 1000ms 内 + 无重做尾巴”
// 时并入上一条，其余（格式化/复选框/粘贴/输入法等）由 UI 传 false 强制拆分。
//
// 生命周期与异常：
// - 首次本地编辑时开启 Journal 编辑会话，finishEditing() 结束；目标被删除/移入回收站后只读
//   （unavailable），草稿保留、绝不复活实体；无脏文本时 finishEditing 允许成功关窗。
// - 保存/重载/信号全程使用守卫：origin 只以 QPointer 记录且悬空后永不合并；正在提交期间到来的
//   更新不丢弃（基线只推进到本次真正提交的检查点后立即与最新快照调和，更新草稿保持脏并在稍后
//   重新排队）；任何同步回调（本类或 store 的信号）删除本会话后立即中止，绝不继续访问成员。
// - SQL/日志失败保留状态与错误供重试；同字段远端真冲突按既有三方语义返回 Conflict，不覆盖。
class EntityTextSession : public QObject
{
    Q_OBJECT
public:
    // 供 TaskStore 创建；store 必须存活（本对象以 store 为 QObject 父，随其销毁）。
    EntityTextSession(TaskStore *store, const QString &entityId, QObject *parent);

    QString entityId() const { return m_entityId; }
    QString title() const { return m_title; }        // 原始草稿（未 trim；提交时才 trim）
    QString body() const { return m_body; }
    bool isDirty() const;                            // 任一字段相对已持久化基线有真实可写差异
    bool isAvailable() const { return m_available; } // 目标存在且未删除（false = 只读）
    bool isSaving() const { return m_flushing; }     // 正处于持久化调用窗口（同步、短暂）
    QString lastError() const { return m_lastError; }
    TextCommitStatus status() const { return m_status; }

    // 立即更新权威草稿并同步广播；origin 原样回传（绝不解引用）。内容未变时为无操作（无信号）。
    // 目标不可用（已删除/回收站）或无法开启编辑会话时返回 false。
    bool updateTitle(const QString &title, QObject *origin = nullptr, bool merge = false);
    bool updateBody(const QString &body, QObject *origin = nullptr, bool merge = false);

    // 立即持久化所有可提交内容并推进基线；仅返回 false 表示有内容保存失败（Conflict/Failed/
    // Unavailable），错误与脏状态保留供重试。空白标题属于不可提交字段：正文照常提交，标题草稿
    // 原样保留（仍脏，lastError 提示），flush 本身按“可提交内容均已保存”返回 true。
    bool flush();

    // 先保存全部可提交内容，再结束 Journal 会话：空白标题只阻止结束、绝不阻止正文落库。
    // 标题草稿 trim 后为空时阻止结束（返回 false）直到修正；目标已不可用且无脏文本时允许成功
    // （取消会话）以便投影关窗，绝不复活实体。结束/取消补账回调期间到来的新编辑会开启替换
    // 会话：新会话、新防抖与新草稿一律保留，结束被阻止（返回 false）。
    bool finishEditing();

    // 显式丢弃权威草稿并从 store 重读（仅在 cancelEditSession 成功后生效），对全部视图广播；
    // 同时清空两字段历史与错误。目标不可用时同样生效：回收站目标重置为当前存储值、永久消失
    // 目标退回最后已知持久化基线，保持只读（不写库、不复活），供“复制后手动丢弃关窗”。
    // 取消补账回调期间到来的新编辑（新会话/新草稿）会中止重载并被完整保留（返回 false）。
    bool reload();

    bool canUndoTitle() const { return !m_titleHistory.undo.isEmpty(); }
    bool canRedoTitle() const { return !m_titleHistory.redo.isEmpty(); }
    bool canUndoBody() const { return !m_bodyHistory.undo.isEmpty(); }
    bool canRedoBody() const { return !m_bodyHistory.redo.isEmpty(); }
    bool undoTitle(QObject *origin = nullptr);
    bool redoTitle(QObject *origin = nullptr);
    bool undoBody(QObject *origin = nullptr);
    bool redoBody(QObject *origin = nullptr);

signals:
    // 权威草稿发生字段级变更后同步发出（远早于持久化）；origin 为触发方，视图据此跳过自身回写。
    void textChanged(QObject *origin);
    // isDirty/isSaving/isAvailable/status/lastError/撤销重做可用性任何变化后发出。
    void stateChanged();

private:
    struct TextEdit {                    // 一次字段差分：start 处的 removed 被 inserted 替换
        int start = 0;
        QString removed;
        QString inserted;
    };
    struct FieldHistory {
        QVector<TextEdit> undo;          // 逆序栈顶在尾部
        QVector<TextEdit> redo;          // 新编辑清空（重做尾巴）
        QPointer<QObject> lastOrigin;    // 上一条差分的 origin；悬空/为空永不合并
        qint64 lastElapsedMs = -1;       // 上一条差分的单调时钟
    };

    void onStoreTaskChanged(const QString &id);
    void onStoreReset();
    void reconcileFromStore();
    bool ensureEditSession();
    bool refreshAvailability();
    bool titleDirty() const;
    bool bodyDirty() const;
    bool titleCommittable() const;       // 标题脏且 trim 后非空
    void scheduleSave();
    void clearStaleTitleError(const QString &title);       // 标题重新有效时清除空标题校验错误
    void pushHistory(FieldHistory *history, const TextEdit &edit, QObject *origin, bool merge);
    void clearHistory(FieldHistory *history);

    QElapsedTimer m_clock;               // 单调时钟，仅供合并窗口计时

    TaskStore *m_store;                  // QObject 父持有，寿命绑定
    QString m_entityId;
    QString m_title;                     // 原始（未 trim）标题草稿
    QString m_body;
    QString m_baselineTitle;             // 最近一次真实持久化的标题（提交值，已 trim）
    QString m_baselineBody;              // 最近一次真实持久化的正文
    QString m_sessionId;                 // Journal 编辑会话；空 = 尚未开始/已被外部终结
    QString m_lastError;
    TextCommitStatus m_status = TextCommitStatus::Unchanged;
    bool m_available = true;
    bool m_flushing = false;             // commitTextDraft 调用窗口（防递归 reconcile/flush）
    QTimer m_saveTimer;                  // 450ms 单次防抖
    FieldHistory m_titleHistory;
    FieldHistory m_bodyHistory;
};

} // namespace kl

#endif // KYLINLIST_ENTITYTEXTSESSION_H
