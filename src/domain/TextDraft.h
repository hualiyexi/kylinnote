#ifndef KYLINLIST_TEXTDRAFT_H
#define KYLINLIST_TEXTDRAFT_H

#include "Entity.h"

namespace kl {

// 正文字段（title/body）三方合并提交的纯数据描述，供 TaskDetailWidget 与桌面便签共用。
// baseline*：编辑器打开（或上次成功提交后重读）时看到的值；proposed*：当前草稿值。
// proposed 与 baseline 相同即视为该字段未修改，绝不覆盖远端同字段内容。
// sessionId 可选：传入编辑器自己的编辑会话 id，供取消会话时精确归因事件日志。
struct TextDraft {
    QString entityId;
    QString sessionId;
    QString baselineTitle;
    QString baselineBody;
    QString proposedTitle;
    QString proposedBody;

    bool titleDirty() const { return proposedTitle != baselineTitle; }
    bool bodyDirty() const { return proposedBody != baselineBody; }
};

// 提交状态：
// - Saved      三方校验通过且已写入（既有 saveTask/CAS/Journal 链，revision 推进）；
// - Unchanged  无需写入（本地未改/已恢复基线，或远端已等于本地），数据库零写入；
// - Conflict   同字段双方都改且值不同，零写入，不碰 revision/updated_at/事件；
// - Unavailable 目标不存在或已删除（含回收站），不可复活；
// - Failed     校验通过但持久化失败（CAS 失败/SQL 失败/字段校验拒绝），事务回滚零写入。
enum class TextCommitStatus { Saved, Unchanged, Conflict, Unavailable, Failed };

struct TextCommitResult {
    TextCommitStatus status = TextCommitStatus::Failed;
    Entity entity;                  // Saved/Unchanged/Conflict：当前最新实体；Unavailable 且仍在回收站时也带出；Failed 为空
    QString error;                  // Conflict/Unavailable/Failed 时可直接展示的安全消息
    bool ok() const { return status == TextCommitStatus::Saved || status == TextCommitStatus::Unchanged; }
};

// 单字段三方校验：返回 false 表示同字段双改且不同（冲突）。
// - 本地未变（proposed==baseline）：保留远端 current，不写；
// - 远端已等于本地（proposed==current）：不写；
// - 远端未变（baseline==current）：写入 proposed；
// - 其余：冲突，由调用方整体拒绝，保证零写入。
inline bool mergeTextField(const QString &baseline, const QString &proposed, const QString &current,
                           QString *merged, bool *needsWrite)
{
    if (proposed == baseline || proposed == current) {
        if (merged) *merged = current;
        if (needsWrite) *needsWrite = false;
        return true;
    }
    if (baseline == current) {
        if (merged) *merged = proposed;
        if (needsWrite) *needsWrite = true;
        return true;
    }
    return false;
}

} // namespace kl

#endif // KYLINLIST_TEXTDRAFT_H
