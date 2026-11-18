#include "app/EntityTextSession.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/TextDraft.h"

#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace kl;

namespace {

QVariant scalar(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql) && query.next()) return query.value(0);
    qWarning() << sql << query.lastError().text();
    return QVariant();
}
QString dbTitle(const QString &id)
{
    return scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(id)).toString();
}
QString dbBody(const QString &id)
{
    return scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(id)).toString();
}
qint64 dbRevision(const QString &id)
{
    return scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
}
QSet<QString> eventIds(const QVector<EntityEvent> &events)
{
    QSet<QString> ids; for (const EntityEvent &event : events) ids.insert(event.id); return ids;
}
}

// 共享文本会话（EntityTextSession）与 TaskStore/Journal 集成的行为测试：
// 即时镜像、双视图双向同步、共享分组撤销/重做、字段独立历史、防抖自动保存、
// 空白标题/删除目标/SQL 失败重试、外部刷新与真冲突、提交期间重入编辑不丢失、事件归因不重复。
class LiveTextTests : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void sharedRegistryAndImmediateMirror();
    void bidirectionalMirrorAcrossViews();
    void debounceAutoSaveAndMetadataPreservation();
    void sharedGroupedUndoRedo();
    void separateFieldHistories();
    void mergeRulesAndDestroyedOrigin();
    void deleteKeyMergeRestoresDeletedPrefix();
    void boundedHistory();
    void blankTitleRetainsDraftAndBlocksFinish();
    void deletedTargetReadOnlyNeverResurrects();
    void sqlFailureRetainsStateForRetry();
    void externalRefreshVersusTrueConflict();
    void revertToBaselineAdoptsNewerRemote();
    void reentrantEditDuringCommitNotLost();
    void flushReconcilesReentrantDirectSave();
    void finishReentrantEditDuringEndSession();
    void reloadReentrantEditKeepsNewDraft();
    void deletionGuardsDuringSignals();
    void journalAttributionNoDuplicates();
    void tasksResetImportDropsStaleSession();
    void importSuccessClearsSessionContext();

private:
    QString dbPath() const { return m_directory->filePath(QStringLiteral("kylinlist.db")); }
    QString backupPath() const { return m_directory->filePath(QStringLiteral("备份.klb")); }
    bool open();
    QString seedTask(TaskStore *store, const QString &title, const QString &body);

    std::unique_ptr<QTemporaryDir> m_directory;
};

void LiveTextTests::init()
{
    Database::instance().close(); m_directory.reset(new QTemporaryDir); QVERIFY(m_directory->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_directory->path().toUtf8());
}
void LiveTextTests::cleanup()
{
    Database::instance().close(); qunsetenv("KYLINLIST_DATA_DIR"); m_directory.reset();
}
bool LiveTextTests::open()
{
    QString error; const bool ok = Database::instance().openAt(dbPath(), &error);
    if (!ok) qWarning() << error; return ok;
}
QString LiveTextTests::seedTask(TaskStore *store, const QString &title, const QString &body)
{
    const QString id = store->createTask(title);
    if (body.isEmpty()) return id;
    Entity seed = store->entity(id); seed.body = body;
    return store->saveTask(seed) ? id : QString();
}

// 同一实体共享一个会话实例；缺失/回收站目标拒绝创建；textChanged 在任何持久化之前同步广播，
// origin 原样回传，DB 内容未动。
void LiveTextTests::sharedRegistryAndImmediateMirror()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("初始标题"), QStringLiteral("初始正文"));
    QVERIFY(isUuid(id));

    EntityTextSession *session = store.textSession(id);
    QVERIFY(session);
    QCOMPARE(store.textSession(id), session);                       // 注册表：同实体同实例
    QCOMPARE(session->entityId(), id);
    QCOMPARE(session->title(), QStringLiteral("初始标题"));
    QCOMPARE(session->body(), QStringLiteral("初始正文"));
    QVERIFY(!session->isDirty());
    QVERIFY(session->isAvailable());
    QVERIFY(!session->isSaving());
    QCOMPARE(session->status(), TextCommitStatus::Unchanged);
    QVERIFY(session->lastError().isEmpty());

    const QString other = store.createTask(QStringLiteral("另一任务"));
    QVERIFY(store.textSession(other) != session);                   // 不同实体不同实例
    QVERIFY(!store.textSession(newUuid()));                          // 不存在 → nullptr

    QObject view;
    QSignalSpy textSpy(session, &EntityTextSession::textChanged);
    QSignalSpy stateSpy(session, &EntityTextSession::stateChanged);
    QVERIFY(session->updateTitle(QStringLiteral("草稿标题"), &view));
    QCOMPARE(textSpy.count(), 1);                                   // 立即同步，先于任何 DB 写入
    QCOMPARE(textSpy.first().at(0).value<QObject *>(), &view);      // origin 原样回传
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(session->title(), QStringLiteral("草稿标题"));
    QVERIFY(session->isDirty());
    QCOMPARE(dbTitle(id), QStringLiteral("初始标题"));                // DB 尚未保存
    QVERIFY(session->updateTitle(QStringLiteral("草稿标题"), &view));// 内容未变：无信号无操作
    QCOMPARE(textSpy.count(), 1);
    QVERIFY(session->reload());                                     // 显式重载丢弃草稿
    QCOMPARE(session->title(), QStringLiteral("初始标题"));
    QCOMPARE(textSpy.count(), 2);                                   // reload 广播 origin=nullptr
    QCOMPARE(textSpy.last().at(0).value<QObject *>(), static_cast<QObject *>(nullptr));
    QVERIFY(!session->isDirty());
}

// 两个视图共用同一权威草稿：任一视图的标题/正文修改立即镜像到会话与另一视图。
void LiveTextTests::bidirectionalMirrorAcrossViews()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("共享标题"), QStringLiteral("共享正文"));
    EntityTextSession *session = store.textSession(id);
    QObject viewA, viewB;
    QSignalSpy textSpy(session, &EntityTextSession::textChanged);

    QVERIFY(session->updateTitle(QStringLiteral("A 改标题"), &viewA));
    QCOMPARE(session->title(), QStringLiteral("A 改标题"));
    QCOMPARE(textSpy.last().at(0).value<QObject *>(), &viewA);
    QVERIFY(session->updateBody(QStringLiteral("B 改正文"), &viewB));
    QCOMPARE(session->body(), QStringLiteral("B 改正文"));
    QCOMPARE(session->title(), QStringLiteral("A 改标题"));          // 字段互不影响
    QCOMPARE(textSpy.last().at(0).value<QObject *>(), &viewB);
    QCOMPARE(textSpy.count(), 2);

    QVERIFY(session->flush());
    QCOMPARE(dbTitle(id), QStringLiteral("A 改标题"));
    QCOMPARE(dbBody(id), QStringLiteral("B 改正文"));
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());
}

// 450ms 防抖自动保存无需显式 flush；保存只写 title/body，计划、置顶、桌面便签等元数据原样保留。
void LiveTextTests::debounceAutoSaveAndMetadataPreservation()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("防抖标题"), QString());
    Entity task = store.entity(id);
    task.dueAt = QDateTime(QDate(2026, 12, 24), QTime(9, 30), Qt::UTC);
    task.priority = 2;
    task.pinnedAt = QDateTime(QDate(2026, 11, 1), QTime(8, 0), Qt::UTC);
    QVERIFY(store.saveTask(task));
    QVERIFY(store.saveDesktopState(id, NoteColor::Green, QRect(11, 22, 330, 440), 7, NoteLayer::OnTop));
    const qint64 revisionBefore = dbRevision(id);

    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral(" 防抖新标题 "), &view));   // 原始草稿保留空白
    QVERIFY(session->updateBody(QStringLiteral("防抖正文"), &view));
    QTest::qWait(700);                                              // 静置超过防抖窗口 → 自动保存
    QCOMPARE(dbTitle(id), QStringLiteral("防抖新标题"));              // 提交 trim 后的标题
    QCOMPARE(dbBody(id), QStringLiteral("防抖正文"));
    QCOMPARE(dbRevision(id), revisionBefore + 1);                    // 单次文本提交只推进一次
    QVERIFY(!session->isDirty());

    const Entity after = store.entity(id);
    QCOMPARE(after.dueAt, task.dueAt);
    QCOMPARE(after.priority, 2);
    QCOMPARE(after.pinnedAt, task.pinnedAt);
    const DesktopNote note = store.desktopNote(id);
    QCOMPARE(note.color, NoteColor::Green);
    QCOMPARE(note.geometry, QRect(11, 22, 330, 440));
    QCOMPARE(note.zOrder, 7);
    QCOMPARE(note.layer, NoteLayer::OnTop);
    QVERIFY(session->finishEditing());
}

// 同字段的连续输入按 origin 分组：一次撤销整组回退；任一视图触发撤销都作用于同一权威草稿并广播。
void LiveTextTests::sharedGroupedUndoRedo()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("标题"), QString());
    EntityTextSession *session = store.textSession(id);
    QObject viewA, viewB;
    QSignalSpy textSpy(session, &EntityTextSession::textChanged);

    QVERIFY(!session->canUndoTitle() && !session->canRedoTitle());
    QVERIFY(session->updateTitle(QStringLiteral("标a"), &viewA, true));
    QVERIFY(session->updateTitle(QStringLiteral("标ab"), &viewA, true));
    QVERIFY(session->updateTitle(QStringLiteral("标abc"), &viewA, true));   // 同 origin 相邻输入 → 合并一组
    QVERIFY(session->canUndoTitle());

    QVERIFY(session->undoTitle(&viewB));                            // 由另一视图触发共享撤销
    QCOMPARE(session->title(), QStringLiteral("标题"));               // 整组一次回退
    QCOMPARE(textSpy.last().at(0).value<QObject *>(), &viewB);
    QVERIFY(!session->canUndoTitle());
    QVERIFY(session->canRedoTitle());

    QVERIFY(session->redoTitle(&viewA));
    QCOMPARE(session->title(), QStringLiteral("标abc"));
    QVERIFY(session->canUndoTitle());
    QVERIFY(!session->canRedoTitle());

    QVERIFY(session->updateTitle(QStringLiteral("标abc!"), &viewB, false));   // merge=false 拆分步并清空重做尾巴
    QVERIFY(!session->canRedoTitle());
    QVERIFY(session->undoTitle(nullptr));
    QCOMPARE(session->title(), QStringLiteral("标abc"));
    QVERIFY(session->undoTitle(nullptr));                           // 分组步整体回退
    QCOMPARE(session->title(), QStringLiteral("标题"));
    QVERIFY(session->flush());
    QVERIFY(session->finishEditing());
}

// 标题与正文历史互不影响：标题撤销不改变正文，反之亦然。
void LiveTextTests::separateFieldHistories()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("独立标题"), QStringLiteral("独立正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("新标题"), &view));
    QVERIFY(session->updateBody(QStringLiteral("新正文"), &view));
    QVERIFY(session->canUndoTitle() && session->canUndoBody());

    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("独立标题"));
    QCOMPARE(session->body(), QStringLiteral("新正文"));              // 正文历史不受标题撤销影响
    QVERIFY(!session->canUndoTitle());
    QVERIFY(session->canUndoBody());

    QVERIFY(session->undoBody(&view));
    QCOMPARE(session->body(), QStringLiteral("独立正文"));
    QVERIFY(session->redoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("新标题"));
    QCOMPARE(session->body(), QStringLiteral("独立正文"));
    QVERIFY(session->flush());
    QCOMPARE(dbTitle(id), QStringLiteral("新标题"));
    // 正文草稿已被 undoBody 撤回基线（从未自动保存过）：无真实差异 → 零写入，DB 仍是原正文。
    QCOMPARE(dbBody(id), QStringLiteral("独立正文"));
    QVERIFY(session->finishEditing());
}

// merge=false（粘贴/格式化/输入法等）强制拆分；已销毁 origin 的指针绝不并入后续输入。
void LiveTextTests::mergeRulesAndDestroyedOrigin()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("合并标题"), QString());
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("合并A"), &view, false));   // 拆分步
    QVERIFY(session->updateTitle(QStringLiteral("合并AB"), &view, false));  // 拆分步
    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("合并A"));
    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("合并标题"));

    {
        QObject doomed;
        QVERIFY(session->updateTitle(QStringLiteral("临时X"), &doomed, true));
        QVERIFY(session->updateTitle(QStringLiteral("临时XY"), &doomed, true));   // 存活期间可合并
    }                                                               // doomed 销毁，QPointer 悬空
    QObject fresh;
    QVERIFY(session->updateTitle(QStringLiteral("临时XYZ"), &fresh, true));  // 悬空 origin 不并入
    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("临时XY"));            // 仅回退 fresh 的一步
    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("合并标题"));           // doomed 组仍是独立一组
    QVERIFY(session->flush());
    QVERIFY(session->finishEditing());
}

// Delete 键在同一位置连续向后删：合并条目必须按原文顺序拼接被删字符，
// 一次撤销完整还原（'abcd' 删 a 再删 b → undo 恢复 'abcd'，而非 'cdba'）。
void LiveTextTests::deleteKeyMergeRestoresDeletedPrefix()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("abcd"), QString());
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("bcd"), &view, true));   // Delete 键删掉 'a'
    QVERIFY(session->updateTitle(QStringLiteral("cd"), &view, true));    // 同位置再删 'b' → 合并
    QVERIFY(session->canUndoTitle());
    QVERIFY(session->undoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("abcd"));                   // 一次撤销整体还原
    QVERIFY(!session->canUndoTitle());
    QVERIFY(session->redoTitle(&view));
    QCOMPARE(session->title(), QStringLiteral("cd"));
    QVERIFY(session->undoTitle(&view));
    QVERIFY(!session->isDirty());
    QVERIFY(session->flush());
    QCOMPARE(dbTitle(id), QStringLiteral("abcd"));
    QVERIFY(session->finishEditing());
}

// 每字段历史上限 200 条：超限丢弃最旧，仍可回退到第 201 步之前的状态。
void LiveTextTests::boundedHistory()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("计数"), QString());
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QString text = QStringLiteral("计数");
    for (int i = 0; i < 205; ++i) {
        text += QString::number(i % 10);
        QVERIFY(session->updateTitle(text, &view, false));           // 全部拆分，共 205 条
    }
    QString expected = text;
    for (int i = 0; i < 200; ++i) {                                  // 只能回退 200 步
        QVERIFY(session->undoTitle(&view));
        expected.chop(1);
        QCOMPARE(session->title(), expected);
    }
    QVERIFY(!session->canUndoTitle());
    QVERIFY(!session->undoTitle(&view));
    QVERIFY(session->reload());                                      // 重置，避免巨量草稿落库
    QVERIFY(session->finishEditing());
}

// 空白标题：原始草稿立即镜像、错误保留并阻止结束；finishEditing 先保存正文再阻止关闭；
// 修正后一切恢复正常。
void LiveTextTests::blankTitleRetainsDraftAndBlocksFinish()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("原标题"), QStringLiteral("原正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;

    QVERIFY(session->updateTitle(QStringLiteral("   "), &view));     // 空白草稿也立即镜像
    QCOMPARE(session->title(), QStringLiteral("   "));
    QVERIFY(session->isDirty());
    QVERIFY(session->updateBody(QStringLiteral("有效正文"), &view));

    // finishEditing 先保存全部可提交内容（正文），再被空白标题阻止结束。
    QVERIFY(!session->finishEditing());
    QCOMPARE(dbBody(id), QStringLiteral("有效正文"));                 // 正文已落库
    QCOMPARE(dbTitle(id), QStringLiteral("原标题"));                  // 标题未被覆盖
    QCOMPARE(session->title(), QStringLiteral("   "));               // 草稿原样保留、仍脏
    QVERIFY(session->isDirty());
    QVERIFY(session->lastError().contains(QStringLiteral("标题不能为空")));
    QVERIFY(!session->finishEditing());                              // 再次结束：幂等阻止
    QCOMPARE(dbBody(id), QStringLiteral("有效正文"));

    QVERIFY(session->updateTitle(QStringLiteral("  修正标题  "), &view));
    QVERIFY(session->lastError().isEmpty());               // 标题一有效：校验错误立即清除，不等保存
    QVERIFY(session->flush());
    QCOMPARE(dbTitle(id), QStringLiteral("修正标题"));                // 提交 trim 后的值
    QVERIFY(!session->isDirty());                                    // 空白差异不算真实差异
    QVERIFY(session->finishEditing());
    QVERIFY(session->lastError().isEmpty());
}

// 目标删除/移入回收站：会话转只读、草稿保留、绝不复活；无脏文本时允许结束关窗；
// 显式 reload 在不可用目标上也生效（丢弃草稿、清脏/历史/错误，绝不写库复活）。
void LiveTextTests::deletedTargetReadOnlyNeverResurrects()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("存活标题"), QStringLiteral("存活正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("未保存标题"), &view));
    QVERIFY(session->updateBody(QStringLiteral("未保存正文"), &view));

    QVERIFY(store.trashTask(id));                                    // taskChanged → 会话转只读
    QVERIFY(!session->isAvailable());
    QCOMPARE(session->title(), QStringLiteral("未保存标题"));         // 脏草稿保留
    QVERIFY(!session->updateBody(QStringLiteral("继续编辑"), &view));  // 只读
    QVERIFY(!session->flush());
    QVERIFY(!session->finishEditing());                              // 有脏文本不能结束
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1' AND deleted=0").arg(id)).toInt(), 0);

    // 显式丢弃（回收站目标）：重置为回收站存储值，保持只读，清脏/历史/错误。
    QVERIFY(session->reload());
    QCOMPARE(session->title(), QStringLiteral("存活标题"));
    QCOMPARE(session->body(), QStringLiteral("存活正文"));
    QVERIFY(!session->isAvailable());
    QVERIFY(!session->isDirty());
    QVERIFY(!session->canUndoTitle());
    QVERIFY(session->lastError().isEmpty());
    QVERIFY(session->finishEditing());                               // 无脏文本：允许关窗
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(id)).toInt(), 1);

    QVERIFY(store.restoreTask(id));                                  // 恢复后可用且干净
    QVERIFY(session->isAvailable());
    QVERIFY(!session->isDirty());

    QVERIFY(session->updateBody(QStringLiteral("又改正文"), &view));   // 重新弄脏（开启新编辑会话）
    QVERIFY(store.trashTask(id));
    QVERIFY(store.deletePermanently(id));                            // 永久删除：绝不可复活
    QVERIFY(!session->isAvailable());
    QCOMPARE(session->body(), QStringLiteral("又改正文"));            // 脏草稿仍保留
    QVERIFY(!session->flush());                                      // 无处可写：失败
    QVERIFY(!session->finishEditing());                              // 有脏文本：阻止结束

    // 显式丢弃（永久消失目标）：退回最后已知持久化基线，仍只读、不复活。
    QVERIFY(session->reload());
    QCOMPARE(session->title(), QStringLiteral("存活标题"));
    QCOMPARE(session->body(), QStringLiteral("存活正文"));
    QVERIFY(!session->isAvailable());
    QVERIFY(!session->isDirty());
    QVERIFY(session->lastError().isEmpty());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(id)).toInt(), 0);
    QVERIFY(session->finishEditing());                               // 无脏文本：关窗成功

    // 无脏文本的会话在目标消失后允许结束（投影关窗），不会写库复活。
    const QString fresh = seedTask(&store, QStringLiteral("干净任务"), QString());
    EntityTextSession *clean = store.textSession(fresh);
    QVERIFY(store.trashTask(fresh));
    QVERIFY(store.deletePermanently(fresh));
    QVERIFY(!clean->isDirty());
    QVERIFY(clean->finishEditing());
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(fresh)).toInt(), 0);
}

// SQL 失败：状态与错误保留、草稿不丢；修复后无需重开即可重试成功。
void LiveTextTests::sqlFailureRetainsStateForRetry()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("重试标题"), QStringLiteral("重试正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("落库失败标题"), &view));
    QVERIFY(session->updateBody(QStringLiteral("落库失败正文"), &view));

    QSqlQuery query(Database::instance().connection());
    QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER reject_live_text BEFORE UPDATE ON entities "
                                      "BEGIN SELECT RAISE(ABORT,'live text rejected'); END")));
    QVERIFY(!session->flush());
    QCOMPARE(session->status(), TextCommitStatus::Failed);
    QVERIFY(session->lastError().contains(QStringLiteral("live text rejected")));
    QVERIFY(session->isDirty());
    QCOMPARE(session->title(), QStringLiteral("落库失败标题"));       // 草稿与错误都保留
    QCOMPARE(dbTitle(id), QStringLiteral("重试标题"));

    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER reject_live_text")));
    QVERIFY(session->flush());                                       // 直接重试成功
    QCOMPARE(session->status(), TextCommitStatus::Saved);
    QCOMPARE(dbTitle(id), QStringLiteral("落库失败标题"));
    QCOMPARE(dbBody(id), QStringLiteral("落库失败正文"));
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());
}

// 无关元数据的外部修改不触碰文本；未脏字段被外部替换时采纳并清历史；脏字段的同字段远端修改
// 走既有三方冲突语义（不覆盖远端、不丢本地），reload 后回到远端值。
void LiveTextTests::externalRefreshVersusTrueConflict()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("本地标题"), QStringLiteral("本地正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("我的标题"), &view));  // 标题脏
    QVERIFY(!session->canUndoBody());

    // 无关元数据外部修改：不广播文本、不清历史、草稿不动。
    QSignalSpy textSpy(session, &EntityTextSession::textChanged);
    Entity metadata = store.entity(id); metadata.priority = 3; QVERIFY(store.saveTask(metadata));
    QCOMPARE(textSpy.count(), 0);
    QCOMPARE(session->title(), QStringLiteral("我的标题"));
    QVERIFY(session->canUndoTitle());

    // 未脏字段（正文）被外部替换：立即采纳 + 清空该字段历史。
    Entity remote = store.entity(id); remote.body = QStringLiteral("远端正文");
    QVERIFY(store.saveTask(remote));
    QCOMPARE(textSpy.count(), 1);
    QCOMPARE(session->body(), QStringLiteral("远端正文"));
    QVERIFY(!session->canUndoBody());
    QCOMPARE(session->title(), QStringLiteral("我的标题"));           // 脏标题不被远端覆盖

    // 同字段真冲突：远端标题也改了 → flush 冲突，零写入，双方内容各自保留。
    Entity clash = store.entity(id); clash.title = QStringLiteral("远端标题");
    QVERIFY(store.saveTask(clash));
    QCOMPARE(session->title(), QStringLiteral("我的标题"));
    QVERIFY(!session->flush());
    QCOMPARE(session->status(), TextCommitStatus::Conflict);
    QVERIFY(session->lastError().contains(QStringLiteral("标题")));
    QCOMPARE(dbTitle(id), QStringLiteral("远端标题"));                // 远端保留
    QCOMPARE(session->title(), QStringLiteral("我的标题"));           // 本地草稿保留

    // 显式重载：丢弃本地草稿、回到远端值、清空历史。
    QVERIFY(session->reload());
    QCOMPARE(session->title(), QStringLiteral("远端标题"));
    QCOMPARE(session->body(), QStringLiteral("远端正文"));
    QVERIFY(!session->canUndoTitle());
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());

    // 仅元数据刷新不得动“原始空白草稿”与其撤销历史：trim 后等于基线 → 非脏，
    // 远端持久化标题也没变 → 不采纳、不清历史。
    QVERIFY(session->updateTitle(QStringLiteral("远端标题 "), &view));  // 原始尾部空白
    QVERIFY(!session->isDirty());
    QVERIFY(session->canUndoTitle());
    Entity metaOnly = store.entity(id); metaOnly.priority = 1; QVERIFY(store.saveTask(metaOnly));
    QCOMPARE(session->title(), QStringLiteral("远端标题 "));           // 原始空白保留
    QVERIFY(session->canUndoTitle());                                 // 历史保留
    QVERIFY(session->undoTitle(nullptr));
    QCOMPARE(session->title(), QStringLiteral("远端标题"));
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());
}

// 本地撤销/改回基线后，远端已有更新：flush 的零提交路径必须调和未脏字段，采纳最新持久化值
// 而不是停留在过期基线（对应详情页“本地回退后采纳远端而非覆盖”场景），且零写入不推 revision。
void LiveTextTests::revertToBaselineAdoptsNewerRemote()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("回退标题"), QStringLiteral("基线正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QSignalSpy textSpy(session, &EntityTextSession::textChanged);

    QVERIFY(session->updateBody(QStringLiteral("本地临时正文"), &view));   // 先弄脏
    Entity remote = store.entity(id); remote.body = QStringLiteral("远端新正文");
    QVERIFY(store.saveTask(remote));                                    // 外部写入：脏字段按冲突语义保留
    QCOMPARE(session->body(), QStringLiteral("本地临时正文"));
    QVERIFY(session->updateBody(QStringLiteral("基线正文"), &view));      // 本地改回基线 → 非脏
    QVERIFY(!session->isDirty());
    const int spyBefore = textSpy.count();
    const qint64 revision = dbRevision(id);
    QVERIFY(session->flush());                                          // 零提交：仍须调和到最新远端
    QCOMPARE(textSpy.count(), spyBefore + 1);                           // 采纳广播 textChanged(nullptr)
    QCOMPARE(session->body(), QStringLiteral("远端新正文"));             // 不停留在过期基线
    QCOMPARE(dbBody(id), QStringLiteral("远端新正文"));
    QCOMPARE(dbRevision(id), revision);                                 // 零写入：revision 不动
    QVERIFY(!session->isDirty());
    QVERIFY(!session->canUndoBody());                                   // 外部替换：正文历史已清
    QCOMPARE(session->title(), QStringLiteral("回退标题"));
    QVERIFY(session->finishEditing());
}

// 提交落库期间（taskChanged 回调内）到达的更新草稿不丢失：基线只推进到本次提交值，
// 新草稿保持脏并由重排的防抖保存收尾。
void LiveTextTests::reentrantEditDuringCommitNotLost()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("重入标题"), QStringLiteral("重入正文"));
    EntityTextSession *session = store.textSession(id);
    QObject viewA, viewB;
    bool fired = false;
    bool sawSaving = false;
    const QMetaObject::Connection hook = connect(&store, &TaskStore::taskChanged, this,
        [&](const QString &changedId) {
            if (changedId != id || fired) return;
            fired = true;
            sawSaving = session->isSaving();                        // 提交窗口内
            session->updateBody(QStringLiteral("更新正文"), &viewB); // 提交期间的新草稿
        });

    QVERIFY(session->updateBody(QStringLiteral("第一次正文"), &viewA));
    QVERIFY(session->flush());
    QVERIFY(fired);
    QVERIFY(sawSaving);
    QCOMPARE(dbBody(id), QStringLiteral("第一次正文"));              // 本次提交的是旧值
    QCOMPARE(session->body(), QStringLiteral("更新正文"));           // 新草稿未丢
    QVERIFY(session->isDirty());
    disconnect(hook);

    QTest::qWait(700);                                              // 重排的防抖自动收尾
    QCOMPARE(dbBody(id), QStringLiteral("更新正文"));
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());
}

// 提交窗口内（m_flushing 屏蔽 taskChanged 期间）他人直存的写入，在成功确认检查点后必须
// 通过 reconcile 收敛：未脏字段采纳最新存储值，权威草稿不得停留在过期内容上。
void LiveTextTests::flushReconcilesReentrantDirectSave()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("直存标题"), QStringLiteral("直存原正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    bool directSaved = false;
    const QMetaObject::Connection hook = connect(&store, &TaskStore::taskChanged, this,
        [&](const QString &changedId) {
            if (changedId != id || directSaved) return;
            directSaved = true;
            Entity fresh = store.entity(id);
            fresh.body = QStringLiteral("窗口内直存正文");
            QVERIFY(store.saveTask(fresh));                 // 提交回调里的重入直存
        });

    QVERIFY(session->updateTitle(QStringLiteral("新直存标题"), &view));  // 仅标题脏
    QVERIFY(session->flush());
    disconnect(hook);
    QVERIFY(directSaved);
    QCOMPARE(session->title(), QStringLiteral("新直存标题"));
    QCOMPARE(session->body(), QStringLiteral("窗口内直存正文"));         // 已收敛：不残留旧正文
    QVERIFY(!session->isDirty());
    QCOMPARE(dbTitle(id), QStringLiteral("新直存标题"));
    QCOMPARE(dbBody(id), QStringLiteral("窗口内直存正文"));
    QVERIFY(session->finishEditing());
}

// 结束会话的补账（eventsChanged）回调里到来的一次编辑：替换会话被完整保留（不清除、不停表），
// finishEditing 被阻止返回 false；新防抖照常自动保存；下一次 finish 只按新会话补记一条正确事件。
void LiveTextTests::finishReentrantEditDuringEndSession()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("会话标题"), QStringLiteral("会话正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("新标题"), &view));
    QVERIFY(session->flush());                              // 旧会话已有自己的标题写入归因
    const QSet<QString> before = eventIds(store.events(id));

    bool reentered = false;
    const QMetaObject::Connection hook = connect(&store, &TaskStore::eventsChanged, this,
        [&](const QString &entityId) {
            if (entityId != id || reentered) return;
            reentered = true;
            QVERIFY(session->updateBody(QStringLiteral("结束期间正文"), &view));   // 一次性重入编辑
        });
    QVERIFY(!session->finishEditing());                     // 新脏草稿阻止关闭
    disconnect(hook);
    QVERIFY(reentered);
    QVERIFY(session->isDirty());                            // 回调草稿未被丢弃
    QCOMPARE(session->body(), QStringLiteral("结束期间正文"));
    QCOMPARE(dbBody(id), QStringLiteral("会话正文"));        // 尚未保存
    QVERIFY(session->lastError().contains(QStringLiteral("新的编辑")));
    QCOMPARE(session->title(), QStringLiteral("新标题"));
    // 旧会话自己的补账已完成：恰好一条标题事件。
    QCOMPARE([&]{ int n = 0; for (const EntityEvent &e : store.events(id)) if (!before.contains(e.id) && e.field == QStringLiteral("title")) ++n; return n; }(), 1);

    QTest::qWait(700);                                      // 替换会话的防抖未被外层停掉
    QCOMPARE(dbBody(id), QStringLiteral("结束期间正文"));
    QVERIFY(!session->isDirty());

    const QSet<QString> beforeFinal = eventIds(store.events(id));
    QVERIFY(session->finishEditing());                      // 新会话正常结束
    const QVector<EntityEvent> finalEvents = [&]{
        QVector<EntityEvent> added;
        for (const EntityEvent &e : store.events(id)) if (!beforeFinal.contains(e.id)) added.append(e);
        return added;
    }();
    QCOMPARE(finalEvents.size(), 1);                        // 只按新会话补记一条正文事件
    QCOMPARE(finalEvents.first().field, QStringLiteral("body"));
    QCOMPARE(finalEvents.first().newValue.toString(), QStringLiteral("结束期间正文"));
    // 全程归因无重复：旧会话一条标题 + 新会话一条正文。
    int total = 0;
    for (const EntityEvent &e : store.events(id)) if (!before.contains(e.id)) ++total;
    QCOMPARE(total, 2);
}

// reload 的取消补账回调里到来的一次编辑：中止重载（返回 false），原草稿与新草稿、
// 新会话、新防抖全部保留；后续自动保存与结束补记都归因新会话。
void LiveTextTests::reloadReentrantEditKeepsNewDraft()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("重载标题"), QStringLiteral("重载正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("脏标题"), &view));
    QVERIFY(session->updateBody(QStringLiteral("脏正文"), &view));
    QVERIFY(session->flush());                              // 旧会话已写入标题/正文
    const QSet<QString> before = eventIds(store.events(id));

    bool reentered = false;
    const QMetaObject::Connection hook = connect(&store, &TaskStore::eventsChanged, this,
        [&](const QString &entityId) {
            if (entityId != id || reentered) return;
            reentered = true;
            QVERIFY(session->updateBody(QStringLiteral("重载期间正文"), &view));   // 取消补账中编辑
        });
    QVERIFY(!session->reload());                            // 中止重载：不丢弃任何草稿
    disconnect(hook);
    QVERIFY(reentered);
    QCOMPARE(session->title(), QStringLiteral("脏标题"));    // 原草稿保留
    QCOMPARE(session->body(), QStringLiteral("重载期间正文"));   // 回调新草稿保留
    QVERIFY(session->isDirty());
    QVERIFY(session->lastError().contains(QStringLiteral("新的编辑")));
    QCOMPARE(dbTitle(id), QStringLiteral("脏标题"));
    QCOMPARE(dbBody(id), QStringLiteral("脏正文"));          // 尚未自动保存

    QTest::qWait(700);                                      // 新防抖照常运行
    QCOMPARE(dbBody(id), QStringLiteral("重载期间正文"));
    QVERIFY(!session->isDirty());

    const QSet<QString> beforeFinal = eventIds(store.events(id));
    QVERIFY(session->finishEditing());
    const QVector<EntityEvent> finalEvents = [&]{
        QVector<EntityEvent> added;
        for (const EntityEvent &e : store.events(id)) if (!beforeFinal.contains(e.id)) added.append(e);
        return added;
    }();
    QCOMPARE(finalEvents.size(), 1);                        // 新会话只补记自己的正文事件
    QCOMPARE(finalEvents.first().field, QStringLiteral("body"));
    QCOMPARE(finalEvents.first().newValue.toString(), QStringLiteral("重载期间正文"));
    // 全程：旧会话取消补记标题+正文两条，新会话结束补记正文一条，共 3 条、无重复。
    int total = 0;
    for (const EntityEvent &e : store.events(id)) if (!before.contains(e.id)) ++total;
    QCOMPARE(total, 3);
}

// 同步回调（textChanged/stateChanged/taskChanged/eventsChanged）中删除会话或其目标：
// 所有守卫路径立即中止，绝不 use-after-free，store 保持可用。
void LiveTextTests::deletionGuardsDuringSignals()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    QObject view;

    // a) textChanged 回调中删除会话：updateTitle 不得再触碰成员。
    {
        const QString id = seedTask(&store, QStringLiteral("删除一"), QString());
        EntityTextSession *session = store.textSession(id);
        bool deleted = false;
        connect(session, &EntityTextSession::textChanged, [&]() {
            if (!deleted) { deleted = true; delete session; }
        });
        const bool ok = session->updateTitle(QStringLiteral("新"), &view);
        QVERIFY(deleted);
        QVERIFY(ok);
    }

    // b) refreshAvailability 首次发现目标消失 → stateChanged 回调中删除。
    {
        const QString id = seedTask(&store, QStringLiteral("删除二"), QString());
        EntityTextSession *session = store.textSession(id);
        bool deleted = false;
        connect(session, &EntityTextSession::stateChanged, [&]() {
            if (!deleted) { deleted = true; delete session; }
        });
        session->blockSignals(true);                        // 屏蔽调和，让 refreshAvailability 自己发现
        QVERIFY(store.trashTask(id));
        session->blockSignals(false);
        QCOMPARE(session->title(), QStringLiteral("删除二"));   // 草稿未受影响，可用性尚未刷新
        const bool ok = session->updateTitle(QStringLiteral("新"), &view);
        QVERIFY(deleted);
        QVERIFY(!ok);
    }

    // c) flush 的 commitTextDraft 期间（taskChanged 信号）删除会话：提交本身已完成。
    {
        const QString id = seedTask(&store, QStringLiteral("删除三"), QStringLiteral("正文三"));
        EntityTextSession *session = store.textSession(id);
        QVERIFY(session->updateTitle(QStringLiteral("新标题三"), &view));
        bool deleted = false;
        const QMetaObject::Connection hook = connect(&store, &TaskStore::taskChanged, this,
            [&](const QString &changedId) {
                if (changedId != id || deleted || !session->isSaving()) return;
                deleted = true;
                delete session;
            });
        const bool ok = session->flush();
        disconnect(hook);                                    // 块内局部被捕获：离开作用域前必须断开
        QVERIFY(deleted);
        QVERIFY(!ok);                                        // 会话已亡：守卫路径返回，返回值仅防误用
        QCOMPARE(dbTitle(id), QStringLiteral("新标题三"));    // 落库不因删除回滚
    }

    // d) finishEditing → endEditSession 补账（eventsChanged）回调中删除会话。
    {
        const QString id = seedTask(&store, QStringLiteral("删除四"), QString());
        EntityTextSession *session = store.textSession(id);
        QVERIFY(session->updateTitle(QStringLiteral("新标题四"), &view));
        QVERIFY(session->flush());
        bool deleted = false;
        const QMetaObject::Connection hook = connect(&store, &TaskStore::eventsChanged, this,
            [&](const QString &entityId) {
                if (entityId != id || deleted) return;
                deleted = true;
                delete session;
            });
        const bool ok = session->finishEditing();
        disconnect(hook);                                    // 块内局部被捕获：离开作用域前必须断开
        QVERIFY(deleted);
        QVERIFY(!ok);                                        // 删除发生在补账提交之后：会话已亡
    }

    // e) reload → cancelEditSession 补账（eventsChanged）回调中删除会话。
    {
        const QString id = seedTask(&store, QStringLiteral("删除五"), QStringLiteral("正文五"));
        EntityTextSession *session = store.textSession(id);
        QVERIFY(session->updateTitle(QStringLiteral("新标题五"), &view));
        QVERIFY(session->flush());
        bool deleted = false;
        const QMetaObject::Connection hook = connect(&store, &TaskStore::eventsChanged, this,
            [&](const QString &entityId) {
                if (entityId != id || deleted) return;
                deleted = true;
                delete session;
            });
        const bool ok = session->reload();
        disconnect(hook);                                    // 块内局部被捕获：离开作用域前必须断开
        QVERIFY(deleted);
        QVERIFY(!ok);                                        // 会话已亡：中止丢弃路径
        QCOMPARE(dbTitle(id), QStringLiteral("新标题五"));    // 会话自身写入不受影响
    }

    QVERIFY(!store.entities().isEmpty());                    // store 始终可用
}

// 事件归因：会话归因的保存不产生直存事件，finishEditing 只补记自己真正写入的字段各一条；
// 空闲会话不吞并外部直存的事件。
void LiveTextTests::journalAttributionNoDuplicates()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("归因标题"), QStringLiteral("归因正文"));
    EntityTextSession *session = store.textSession(id);
    QObject view;

    QSet<QString> before = eventIds(store.events(id));
    const auto addedSince = [&](const QSet<QString> &marker) {
        QVector<EntityEvent> added;
        for (const EntityEvent &event : store.events(id)) if (!marker.contains(event.id)) added.append(event);
        return added;
    };

    QVERIFY(session->updateTitle(QStringLiteral("终稿标题"), &view));
    QVERIFY(session->updateBody(QStringLiteral("终稿正文"), &view));
    QVERIFY(session->flush());
    QVERIFY(addedSince(before).isEmpty());                          // 归因保存：推迟到会话结束
    QVERIFY(session->finishEditing());
    const QVector<EntityEvent> own = addedSince(before);
    QCOMPARE(own.size(), 2);                                        // 每字段一条，绝不重复
    bool sawTitle = false, sawBody = false;
    for (const EntityEvent &event : own) {
        QCOMPARE(event.type, QStringLiteral("changed"));
        if (event.field == QStringLiteral("title")) {
            sawTitle = true; QCOMPARE(event.newValue.toString(), QStringLiteral("终稿标题"));
        } else if (event.field == QStringLiteral("body")) {
            sawBody = true; QCOMPARE(event.newValue.toString(), QStringLiteral("终稿正文"));
        } else QFAIL("会话结束不应记录 title/body 以外字段");
    }
    QVERIFY(sawTitle && sawBody);

    // 会话空闲（已结束）时外部直存：事件立即记账，不被会话吞掉，也不产生会话补账。
    before = eventIds(store.events(id));
    Entity direct = store.entity(id); direct.title = QStringLiteral("外部直存标题");
    QVERIFY(store.saveTask(direct));
    const QVector<EntityEvent> directEvents = addedSince(before);
    QCOMPARE(directEvents.size(), 1);
    QCOMPARE(directEvents.first().field, QStringLiteral("title"));
    QCOMPARE(directEvents.first().newValue.toString(), QStringLiteral("外部直存标题"));
    QCOMPARE(session->title(), QStringLiteral("外部直存标题"));       // 空闲会话跟随远端
    QVERIFY(!session->isDirty());
    QVERIFY(session->finishEditing());                              // 无会话/无脏：幂等成功
}

// 备份导入成功即作废共享会话注册表：陈旧撤销/重做上下文绝不跨恢复存活；随导入消失的实体
// 不重建会话、不复活；未保存草稿随旧会话一起丢弃（恢复成功点统一清除，见 importSuccess…）。
void LiveTextTests::tasksResetImportDropsStaleSession()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString keeper = seedTask(&store, QStringLiteral("保留任务"), QString());
    QVERIFY(store.exportBackup(backupPath()));

    const QString id = seedTask(&store, QStringLiteral("导入前标题"), QStringLiteral("导入前正文"));
    EntityTextSession *session = store.textSession(id);
    QPointer<EntityTextSession> tracked(session);
    QObject view;
    QVERIFY(session->updateTitle(QStringLiteral("导入期间草稿"), &view));
    QVERIFY(session->isDirty());

    QVERIFY(store.importBackup(backupPath()));                      // 不含 id：成功导入即整体清除
    QVERIFY(tracked.isNull());                                      // 旧会话已销毁，草稿不套用到新数据
    QVERIFY(!store.textSession(id));                                // 实体随导入消失：不重建、不复活
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(id)).toInt(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM entities WHERE id='%1'").arg(keeper)).toInt(), 1);
    QVERIFY(store.textSession(keeper));                             // 存活实体按需重建新会话
    QCOMPARE(store.textSession(keeper)->title(), QStringLiteral("保留任务"));
}

// 成功导入必须整体作废共享会话上下文：即使恢复后 title/body 与旧草稿完全相同（调和无从
// 察觉差异），旧撤销/重做也绝不能在新数据上重放；失败路径则完整保留会话身份与历史。
void LiveTextTests::importSuccessClearsSessionContext()
{
    QVERIFY(open()); TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = seedTask(&store, QStringLiteral("恢复标题"), QStringLiteral("恢复正文"));
    EntityTextSession *oldSession = store.textSession(id);
    QPointer<EntityTextSession> tracked(oldSession);
    QObject view;

    // 编辑后撤销回原值：内容与基线相同，但撤销/重做历史仍保留旧编辑上下文。
    QVERIFY(oldSession->updateTitle(QStringLiteral("临时标题"), &view));
    QVERIFY(oldSession->updateBody(QStringLiteral("临时正文"), &view));
    QVERIFY(oldSession->undoTitle(&view));
    QVERIFY(oldSession->undoBody(&view));
    QVERIFY(!oldSession->isDirty());
    QVERIFY(oldSession->canRedoTitle() && oldSession->canRedoBody());
    QVERIFY(oldSession->finishEditing());                    // 干净结束；历史仍在

    QVERIFY(store.exportBackup(backupPath()));               // 导出与当前完全相同的值
    QVERIFY(store.importBackup(backupPath()));               // 成功导入：文本相同也必须清上下文

    QVERIFY(tracked.isNull());                               // 旧会话对象已销毁（QPointer 失效）
    EntityTextSession *fresh = store.textSession(id);        // 注册表按需重建
    QVERIFY(fresh && fresh != oldSession);
    QCOMPARE(fresh->title(), QStringLiteral("恢复标题"));      // 新会话持有正确文本
    QCOMPARE(fresh->body(), QStringLiteral("恢复正文"));
    QVERIFY(!fresh->canUndoTitle() && !fresh->canRedoTitle());// 旧编辑不可重放
    QVERIFY(!fresh->canUndoBody() && !fresh->canRedoBody());
    QVERIFY(!fresh->isDirty());
    QCOMPARE(fresh->status(), TextCommitStatus::Unchanged);

    // 失败路径保留会话身份与历史：损坏容器在动库前被拒，绝不清除。
    QPointer<EntityTextSession> kept(fresh);
    QVERIFY(fresh->updateTitle(QStringLiteral("保留编辑"), &view));
    QVERIFY(fresh->canUndoTitle());
    const QString broken = m_directory->filePath(QStringLiteral("broken.klb"));
    QFile file(broken); QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("not-a-backup") > 0); file.close();
    QVERIFY(!store.importBackup(broken));
    QVERIFY(!kept.isNull());                                 // 会话仍在
    QCOMPARE(kept->title(), QStringLiteral("保留编辑"));       // 草稿与历史原样保留
    QVERIFY(kept->canUndoTitle());
    QVERIFY(kept->undoTitle(nullptr));                       // 旧历史仍可正常回退
    QCOMPARE(kept->title(), QStringLiteral("恢复标题"));
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv); LiveTextTests tests; return QTest::qExec(&tests, argc, argv);
}

#include "tst_livetext.moc"
