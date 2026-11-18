#include "EntityTextSession.h"

#include "TaskStore.h"

#include <QElapsedTimer>

namespace kl {
namespace {

// 自动保存防抖窗口：每次真实编辑后重置，静置该时长后走 commitTextDraft 持久化。
const int kAutoSaveDelayMs = 450;
// 同一 origin 相邻插入/删除的合并窗口；超出即拆分为独立撤销步。
const qint64 kMergeWindowMs = 1000;
// 每字段差分历史上限，超出丢弃最旧条目。
const int kHistoryLimit = 200;

QString emptyTitleError()
{
    return QStringLiteral("任务标题不能为空");
}
QString unavailableError()
{
    return QStringLiteral("目标已在回收站或被删除，不能编辑文本");
}
QString reentryError()
{
    return QStringLiteral("操作期间产生了新的编辑，已保留未保存内容");
}

// 两条文本的最小差分：公共前后缀之外的片段即一次替换。
struct Diff {
    int start = 0;
    QString removed;
    QString inserted;
};
Diff makeDiff(const QString &oldText, const QString &newText)
{
    int prefix = 0;
    const int prefixMax = qMin(oldText.size(), newText.size());
    while (prefix < prefixMax && oldText.at(prefix) == newText.at(prefix)) ++prefix;
    int suffix = 0;
    const int suffixMax = qMin(oldText.size() - prefix, newText.size() - prefix);
    while (suffix < suffixMax
           && oldText.at(oldText.size() - 1 - suffix) == newText.at(newText.size() - 1 - suffix)) ++suffix;
    Diff diff;
    diff.start = prefix;
    diff.removed = oldText.mid(prefix, oldText.size() - prefix - suffix);
    diff.inserted = newText.mid(prefix, newText.size() - prefix - suffix);
    return diff;
}
// 在 text 的 start 处把 removed 替换为 inserted。
QString applyDiff(const QString &text, int start, const QString &removed, const QString &inserted)
{
    return text.left(start) + inserted + text.mid(start + removed.size());
}

} // namespace

EntityTextSession::EntityTextSession(TaskStore *store, const QString &entityId, QObject *parent)
    : QObject(parent), m_store(store), m_entityId(entityId)
{
    m_clock.start();                                        // 合并窗口使用的单调时钟
    // 构造时直接读取快照，不经过信号路径；目标缺失/已删除 → 只读会话。
    const Entity current = m_store->entity(m_entityId);
    m_available = !current.id.isEmpty() && !current.deleted;
    m_title = current.title;
    m_body = current.body;
    m_baselineTitle = current.title;
    m_baselineBody = current.body;
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(kAutoSaveDelayMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() {
        if (!m_flushing) flush();
    });
    // 外部写入（其他编辑器/直存/恢复导入）到达时做字段级调和；自身保存由 m_flushing 屏蔽，
    // 杜绝“保存中收到自己的 taskChanged 又触发调和/再保存”的递归环。
    connect(m_store, &TaskStore::taskChanged, this, &EntityTextSession::onStoreTaskChanged);
    connect(m_store, &TaskStore::tasksReset, this, &EntityTextSession::onStoreReset);
}

bool EntityTextSession::isDirty() const
{
    return titleDirty() || bodyDirty();
}

bool EntityTextSession::titleDirty() const
{
    // 提交口径是 trim 后的标题：空白差异不算真实可写差异。
    return m_title.trimmed() != m_baselineTitle;
}

bool EntityTextSession::bodyDirty() const
{
    return m_body != m_baselineBody;
}

bool EntityTextSession::titleCommittable() const
{
    return titleDirty() && !m_title.trimmed().isEmpty();
}

bool EntityTextSession::updateTitle(const QString &title, QObject *origin, bool merge)
{
    if (title == m_title) return true;                     // 无变化：无信号、无历史
    // 守卫：任何同步回调（本类信号、store 信号）都可能删除本会话，之后的成员访问必须先验证存活。
    QPointer<EntityTextSession> guard(this);
    if (!refreshAvailability()) {
        if (!guard) return false;
        m_lastError = unavailableError();
        emit stateChanged();
        return false;
    }
    if (!guard) return false;
    if (!ensureEditSession()) return false;                // 首次本地编辑开启 Journal 会话
    if (!guard) return false;
    // 一旦标题重新有效，立即清掉既有的空标题校验错误（只清这一种已知错误，SQL/日志等其他
    // 错误不受影响）：状态信号发出前 UI 即可隐藏提示，无需等待防抖保存。
    clearStaleTitleError(title);
    const Diff diff = makeDiff(m_title, title);
    pushHistory(&m_titleHistory, {diff.start, diff.removed, diff.inserted}, origin, merge);
    m_title = title;
    // 立即同步广播权威草稿：绝不等待防抖保存；origin 原样回传，视图自行忽略自身来源。
    emit textChanged(origin);
    if (!guard) return true;                               // 回调中已删除：草稿已生效，不再触碰成员
    scheduleSave();
    emit stateChanged();
    return true;
}

bool EntityTextSession::updateBody(const QString &body, QObject *origin, bool merge)
{
    if (body == m_body) return true;
    QPointer<EntityTextSession> guard(this);
    if (!refreshAvailability()) {
        if (!guard) return false;
        m_lastError = unavailableError();
        emit stateChanged();
        return false;
    }
    if (!guard) return false;
    if (!ensureEditSession()) return false;
    if (!guard) return false;
    const Diff diff = makeDiff(m_body, body);
    pushHistory(&m_bodyHistory, {diff.start, diff.removed, diff.inserted}, origin, merge);
    m_body = body;
    emit textChanged(origin);
    if (!guard) return true;
    scheduleSave();
    emit stateChanged();
    return true;
}

bool EntityTextSession::flush()
{
    if (m_flushing) return true;                           // 重入：外层 flush 负责收尾重排
    QPointer<EntityTextSession> guard(this);
    if (!refreshAvailability()) {
        if (!guard) return false;
        if (!isDirty()) return true;
        m_lastError = unavailableError();
        m_saveTimer.stop();
        emit stateChanged();
        return false;
    }
    if (!guard) return false;
    const bool commitTitle = titleCommittable();
    const bool commitBody = bodyDirty();
    if (!commitTitle && !commitBody) {
        // 无可提交内容（未编辑，或仅剩空白标题这类不可提交草稿）。本地可能已把草稿撤销/改回
        // 基线，而远端在此期间被外部更新过（当时字段为脏、调和按冲突语义保留了本地）：
        // 此刻字段已非脏，必须收敛到最新持久化值，绝不让权威草稿停留在过期基线上。
        reconcileFromStore();
        if (!guard) return false;
        m_saveTimer.stop();
        if (m_title.trimmed().isEmpty()) m_lastError = emptyTitleError();
        else m_lastError.clear();
        emit stateChanged();
        return true;
    }
    TextDraft draft;
    draft.entityId = m_entityId;
    draft.sessionId = m_sessionId;
    // 未提交字段 baseline==proposed：三方合并恒为“本地未变”，绝不覆盖远端；
    // 空白标题即走此路径（沿用 baselineTitle，草稿原样保留、仍脏）。
    draft.baselineTitle = m_baselineTitle;
    draft.proposedTitle = commitTitle ? m_title.trimmed() : m_baselineTitle;
    draft.baselineBody = commitBody ? m_baselineBody : m_body;
    draft.proposedBody = m_body;
    // 提交期间（taskChanged 回调）可能出现更新的草稿或他人的直存：基线只能推进到本次真正
    // 提交的检查点，随后 reconcile 收敛到最新快照；更新草稿保持脏、不丢弃。
    const QString committedTitle = draft.proposedTitle;
    const QString committedBody = draft.proposedBody;

    m_flushing = true;
    const TextCommitResult result = m_store->commitTextDraft(draft);
    if (!guard) return false;                              // 提交信号回调中本会话被删除
    m_flushing = false;
    m_status = result.status;

    if (result.status == TextCommitStatus::Saved
        || result.status == TextCommitStatus::Unchanged) {
        if (commitTitle) m_baselineTitle = committedTitle;
        if (commitBody) m_baselineBody = committedBody;
        // 确认检查点后立即与最新快照调和：提交窗口内被 m_flushing 屏蔽的外部写入（如回调里
        // 的直存）不再被漏掉，未脏字段收敛到最新值；脏字段（更新草稿）按冲突语义保留。
        if (result.status == TextCommitStatus::Saved) {
            reconcileFromStore();
            if (!guard) return true;
        }
        if (m_title.trimmed().isEmpty()) m_lastError = emptyTitleError();
        else m_lastError.clear();
        // 更新草稿（提交期间到达）或遗留不可提交字段时保持/重排防抖；否则停表。
        scheduleSave();
        emit stateChanged();
        return true;
    }
    // Conflict：同字段远端已改且不同——保留本地脏草稿与编辑基线（既有三方语义，不覆盖远端）。
    // Failed：SQL/CAS/校验失败——状态与错误保留，供用户重试（下次编辑或显式 flush）。
    // Unavailable：目标已消失——草稿保留、转为只读。三者都不自动重试，避免失败/冲突循环。
    m_lastError = result.error.isEmpty() ? m_store->lastError() : result.error;
    m_saveTimer.stop();
    emit stateChanged();
    return false;
}

bool EntityTextSession::finishEditing()
{
    if (m_flushing) return false;
    QPointer<EntityTextSession> guard(this);
    // 先保存全部可提交内容（如有效正文），空白标题只阻止结束、绝不阻止保存。
    if (!flush()) return false;
    if (!guard) return false;                              // flush 信号回调中本会话被删除
    if (m_title.trimmed().isEmpty()) {
        // 空白标题保留草稿与错误，明确阻止结束（直到修正）。
        m_lastError = emptyTitleError();
        m_saveTimer.stop();
        emit stateChanged();
        return false;
    }
    if (isDirty()) {
        // flush 期间到达的更新草稿：不能带着未落库内容结束会话，重排队后返回。
        scheduleSave();
        return false;
    }
    if (!m_available) {
        // 目标已消失且无脏文本：允许投影关窗；取消会话只补记本会话自有写入，绝不复活实体。
        if (m_sessionId.isEmpty()) return true;
        const QString endedSessionId = m_sessionId;        // 捕获本次要终结的会话
        if (!m_store->cancelEditSession(endedSessionId)) {
            if (guard) { m_lastError = m_store->lastError(); emit stateChanged(); }
            return false;
        }
        if (!guard) return false;
        if (m_sessionId != endedSessionId) {
            // 补账回调开启了新的编辑会话（如目标被恢复并继续编辑）：新会话/新防抖/新草稿
            // 一律保留，绝不当作已结束的旧会话清掉，结束被阻止。
            m_lastError = reentryError();
            emit stateChanged();
            return false;
        }
        m_sessionId.clear();
        m_saveTimer.stop();
        emit stateChanged();
        return true;
    }
    if (!m_sessionId.isEmpty()) {
        // 正常结束：eventsForEnd 只补记本会话自己真正写入的字段终值；失败保留会话供重试。
        const QString endedSessionId = m_sessionId;        // 捕获本次要终结的会话
        if (!m_store->endEditSession(endedSessionId)) {
            if (guard) { m_lastError = m_store->lastError(); emit stateChanged(); }
            return false;
        }
        if (!guard) return false;
        if (m_sessionId != endedSessionId) {
            // 补账回调（eventsChanged）里到来的编辑已开启替换会话并重新弄脏：只清比较所得的
            // “新”会话绝不允许——保留新会话与它排程的防抖保存，结束被阻止返回 false。
            m_lastError = reentryError();
            emit stateChanged();
            return false;
        }
        m_sessionId.clear();
        if (isDirty()) {
            // 防御：会话 id 未换但仍出现脏内容——同样保留草稿与防抖，阻止结束。
            m_lastError = reentryError();
            emit stateChanged();
            return false;
        }
    }
    m_lastError.clear();
    m_saveTimer.stop();
    emit stateChanged();
    return true;
}

bool EntityTextSession::reload()
{
    if (m_flushing) return false;
    QPointer<EntityTextSession> guard(this);
    // 丢弃权威草稿必须先终结 Journal 会话（失败保留会话与草稿，绝不半途丢弃）。
    if (!m_sessionId.isEmpty()) {
        const QString canceledSessionId = m_sessionId;     // 捕获本次要取消的会话
        if (!m_store->cancelEditSession(canceledSessionId)) {
            if (guard) { m_lastError = m_store->lastError(); emit stateChanged(); }
            return false;
        }
        if (!guard) return false;                          // 取消会话的日志信号回调中已删除
        if (m_sessionId != canceledSessionId) {
            // 取消补账的回调里到来的编辑已开启新会话/新草稿：显式重载意图不覆盖它们，
            // 最安全的做法是中止重载，原草稿与新草稿、新会话、新防抖全部保留。
            m_lastError = reentryError();
            emit stateChanged();
            return false;
        }
        m_sessionId.clear();
    }
    // 会话终结后再读快照：补账回调期间的可能写入不会被回读成旧值。
    const Entity current = m_store->entity(m_entityId);    // 只读，无信号
    // 显式丢弃对不可用目标同样生效（用户先复制再手动丢弃即可关掉被阻止的窗口）：
    // 回收站目标重置为当前存储值；永久消失目标退回最后已知持久化基线。只读、绝不写库复活。
    const bool readable = !current.id.isEmpty();
    const QString nextTitle = readable ? current.title : m_baselineTitle;
    const QString nextBody = readable ? current.body : m_baselineBody;
    m_title = nextTitle;
    m_body = nextBody;
    m_baselineTitle = nextTitle;
    m_baselineBody = nextBody;
    // 显式重载等价于外部整体替换：两字段历史一并清空。
    clearHistory(&m_titleHistory);
    clearHistory(&m_bodyHistory);
    m_available = readable && !current.deleted;
    m_status = TextCommitStatus::Unchanged;
    m_lastError.clear();
    m_saveTimer.stop();
    emit textChanged(nullptr);                             // 全视图刷新（非任何视图来源）
    if (!guard) return true;
    emit stateChanged();
    return true;
}

bool EntityTextSession::undoTitle(QObject *origin)
{
    if (m_titleHistory.undo.isEmpty()) return false;
    if (!m_available) return false;
    QPointer<EntityTextSession> guard(this);
    if (!ensureEditSession()) return false;
    if (!guard) return false;
    const TextEdit entry = m_titleHistory.undo.takeLast();
    // 逆变换：把 inserted 换回 removed；原条目转入重做栈（撤销/重做均不再进入撤销栈，无反馈记录）。
    m_title = applyDiff(m_title, entry.start, entry.inserted, entry.removed);
    clearStaleTitleError(m_title);                         // 撤销也可能把空白标题修回有效值
    m_titleHistory.redo.append(entry);
    m_titleHistory.lastOrigin = nullptr;
    m_titleHistory.lastElapsedMs = -1;                     // 撤销后强制断开合并链
    emit textChanged(origin);
    if (!guard) return true;
    scheduleSave();
    emit stateChanged();
    return true;
}

bool EntityTextSession::redoTitle(QObject *origin)
{
    if (m_titleHistory.redo.isEmpty()) return false;
    if (!m_available) return false;
    QPointer<EntityTextSession> guard(this);
    if (!ensureEditSession()) return false;
    if (!guard) return false;
    const TextEdit entry = m_titleHistory.redo.takeLast();
    m_title = applyDiff(m_title, entry.start, entry.removed, entry.inserted);
    clearStaleTitleError(m_title);
    m_titleHistory.undo.append(entry);
    m_titleHistory.lastOrigin = nullptr;
    m_titleHistory.lastElapsedMs = -1;
    emit textChanged(origin);
    if (!guard) return true;
    scheduleSave();
    emit stateChanged();
    return true;
}

bool EntityTextSession::undoBody(QObject *origin)
{
    if (m_bodyHistory.undo.isEmpty()) return false;
    if (!m_available) return false;
    QPointer<EntityTextSession> guard(this);
    if (!ensureEditSession()) return false;
    if (!guard) return false;
    const TextEdit entry = m_bodyHistory.undo.takeLast();
    m_body = applyDiff(m_body, entry.start, entry.inserted, entry.removed);
    m_bodyHistory.redo.append(entry);
    m_bodyHistory.lastOrigin = nullptr;
    m_bodyHistory.lastElapsedMs = -1;
    emit textChanged(origin);
    if (!guard) return true;
    scheduleSave();
    emit stateChanged();
    return true;
}

bool EntityTextSession::redoBody(QObject *origin)
{
    if (m_bodyHistory.redo.isEmpty()) return false;
    if (!m_available) return false;
    QPointer<EntityTextSession> guard(this);
    if (!ensureEditSession()) return false;
    if (!guard) return false;
    const TextEdit entry = m_bodyHistory.redo.takeLast();
    m_body = applyDiff(m_body, entry.start, entry.removed, entry.inserted);
    m_bodyHistory.undo.append(entry);
    m_bodyHistory.lastOrigin = nullptr;
    m_bodyHistory.lastElapsedMs = -1;
    emit textChanged(origin);
    if (!guard) return true;
    scheduleSave();
    emit stateChanged();
    return true;
}

void EntityTextSession::onStoreTaskChanged(const QString &id)
{
    if (m_flushing || id != m_entityId) return;            // 自身保存窗口内的信号一律忽略
    reconcileFromStore();
}

void EntityTextSession::onStoreReset()
{
    if (m_flushing) return;
    reconcileFromStore();
}

void EntityTextSession::reconcileFromStore()
{
    QPointer<EntityTextSession> guard(this);
    const Entity current = m_store->entity(m_entityId);
    const bool available = !current.id.isEmpty() && !current.deleted;
    bool textAdopted = false;
    bool stateAffected = false;
    if (available != m_available) { m_available = available; stateAffected = true; }
    // tasksReset 前可能被 endAllSessions（如备份导入）外部终结：陈旧会话 id 一律作废，
    // 下次编辑重新开启，绝不复用旧会话归因。
    if (!m_sessionId.isEmpty() && !m_store->isEditSessionActive(m_sessionId)) {
        m_sessionId.clear();
        stateAffected = true;
    }
    if (!available) {
        // 目标消失：保留脏草稿、转只读；不广播文本（投影自行按 unavailable 处理）。
        if (stateAffected) emit stateChanged();
        return;
    }
    // 未脏字段只在“持久化值真的变了”（当前存储值 != 已持久化基线）时采纳并清该字段历史；
    // 仅元数据刷新绝不动原始草稿（含尾部空白）与撤销历史。
    if (!titleDirty() && current.title != m_baselineTitle) {
        m_title = current.title;
        m_baselineTitle = current.title;
        clearHistory(&m_titleHistory);
        textAdopted = true;
        stateAffected = true;
    }
    if (!bodyDirty() && current.body != m_baselineBody) {
        m_body = current.body;
        m_baselineBody = current.body;
        clearHistory(&m_bodyHistory);
        textAdopted = true;
        stateAffected = true;
    }
    // 脏字段：本地草稿与编辑基线原样保留——后续提交沿用三方冲突语义（同字段远端已改且不同
    // 则 Conflict），绝不静默覆盖远端，也绝不让远端覆盖未保存的本地输入。
    if (textAdopted) emit textChanged(nullptr);
    if (!guard) return;
    if (stateAffected) emit stateChanged();
}

bool EntityTextSession::ensureEditSession()
{
    if (!m_sessionId.isEmpty()) {
        if (m_store->isEditSessionActive(m_sessionId)) return true;
        m_sessionId.clear();                               // 已被外部终结，重新开启
    }
    QPointer<EntityTextSession> guard(this);
    const QString sessionId = m_store->beginEditSession(m_entityId);
    if (!guard) return false;                              // 失败路径 errorOccurred 回调中已删除
    if (sessionId.isEmpty()) {
        m_lastError = m_store->lastError().isEmpty() ? QStringLiteral("无法创建编辑会话")
                                                     : m_store->lastError();
        emit stateChanged();
        return false;
    }
    m_sessionId = sessionId;
    return true;
}

bool EntityTextSession::refreshAvailability()
{
    const Entity current = m_store->entity(m_entityId);
    const bool available = !current.id.isEmpty() && !current.deleted;
    if (available != m_available) {
        m_available = available;
        emit stateChanged();                               // 回调可能删除本会话；调用方负责守卫
    }
    return available;
}

void EntityTextSession::scheduleSave()
{
    if (titleCommittable() || bodyDirty()) m_saveTimer.start();
    else m_saveTimer.stop();                               // 仅剩不可提交草稿（空白标题）时停表
}

void EntityTextSession::clearStaleTitleError(const QString &title)
{
    // 标题重新有效即清掉空标题校验错误；只匹配这一种已知文案，SQL/日志等其他错误原样保留。
    if (!title.trimmed().isEmpty() && m_lastError == emptyTitleError()) m_lastError.clear();
}

void EntityTextSession::pushHistory(FieldHistory *history, const TextEdit &edit, QObject *origin,
                                    bool merge)
{
    const qint64 now = m_clock.elapsed();
    bool merged = false;
    // 合并条件从严：显式 merge、栈非空、无重做尾巴、同一“存活”origin（悬空/空 origin 永不合并，
    // 已销毁视图的指针即使被复用也不会并入他人条目）、1000ms 内、且是相邻的纯插入或纯删除。
    if (merge && origin && !history->undo.isEmpty() && history->redo.isEmpty()
        && !history->lastOrigin.isNull() && history->lastOrigin.data() == origin
        && history->lastElapsedMs >= 0 && now - history->lastElapsedMs <= kMergeWindowMs) {
        TextEdit &top = history->undo.last();
        if (edit.removed.isEmpty() && !edit.inserted.isEmpty() && !top.inserted.isEmpty()
            && edit.start == top.start + top.inserted.size()) {
            // 连续向后输入并入上一条：上一条允许是替换（如全选重输后继续打字），
            // 只要求插入点恰好接在其插入文本末尾——合并条目的逆变换仍精确还原。
            top.inserted += edit.inserted;
            merged = true;
        } else if (!edit.removed.isEmpty() && edit.inserted.isEmpty() && !top.removed.isEmpty()
                   && top.inserted.isEmpty()) {
            if (edit.start + edit.removed.size() == top.start) {
                top.start = edit.start;                    // 退格：紧邻上一删除之前
                top.removed = edit.removed + top.removed;
                merged = true;
            } else if (edit.start == top.start) {
                // Delete 键在同一位置继续向后删：被删字符按原文顺序接在已删字符之后。
                top.removed = top.removed + edit.removed;
                merged = true;
            }
        }
    }
    if (!merged) {
        history->undo.append(edit);
        history->redo.clear();                             // 新编辑作废全部重做尾巴
    }
    while (history->undo.size() > kHistoryLimit) history->undo.removeFirst();
    history->lastOrigin = origin;
    history->lastElapsedMs = now;
}

void EntityTextSession::clearHistory(FieldHistory *history)
{
    history->undo.clear();
    history->redo.clear();
    history->lastOrigin = nullptr;
    history->lastElapsedMs = -1;
}

} // namespace kl
