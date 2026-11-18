#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/ActivityPolicy.h"
#include "domain/TaskDrop.h"
#include "domain/TaskGrouping.h"
#include "domain/ViewSpec.h"

#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace kl;

namespace {
QDate today() { return toAppTime(appNow()).date(); }
QDateTime wall(const QDate &date, const QTime &time = QTime(10, 0))
{
    return fromAppTime(QDateTime(date, time));
}
bool execute(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql)) return true;
    qWarning() << query.lastError().text(); return false;
}
QVariant scalar(const QString &sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql) && query.next()) return query.value(0);
    qWarning() << query.lastError().text(); return QVariant();
}
QList<QVariantList> rows(const QString &table)
{
    QSqlQuery query(Database::instance().connection());
    QList<QVariantList> result;
    if (!query.exec(QStringLiteral("SELECT * FROM %1 ORDER BY id").arg(table))) {
        qWarning() << query.lastError().text(); return result;
    }
    while (query.next()) {
        QVariantList row;
        for (int i = 0; i < query.record().count(); ++i) row.append(query.value(i));
        result.append(row);
    }
    return result;
}
using DatabaseState = QVector<QList<QVariantList>>;
DatabaseState databaseState()
{
    DatabaseState result;
    const char *tables[] = {"entities", "entity_events", "lists", "sections", "recurrence_series",
        "recurrence_links", "reminders", "attachments", "desktop_notes", "outbox", "tombstones"};
    for (const char *table : tables) result.append(rows(QString::fromLatin1(table)));
    return result;
}
TaskDrop::Intent intentFor(const TaskStore &store, const QString &id, GroupMode mode,
                          const GroupKey &target, const QStringList &ordered = QStringList())
{
    TaskDrop::Intent intent;
    intent.sourceId = id; intent.mode = mode;
    intent.sourceGroup = groupOf(store.entity(id), mode, today()); intent.targetGroup = target;
    intent.orderedIds = ordered.isEmpty() ? QStringList{id} : ordered;
    intent.targetDate = TaskDrop::dateForGroup(target, today());
    return intent;
}
QStringList sectionIds(const TaskStore &store, const ViewSpec &spec, const GroupKey &group)
{
    for (const ViewSection &section : buildFolderSections(store.entities(), store.lists(), spec, today(), store.archivedListIds())) {
        if (section.key != group.stableId()) continue;
        QStringList ids;
        for (const Entity &entity : section.items) ids.append(entity.id);
        return ids;
    }
    return QStringList();
}
QVariantList entityRow(const QString &id)
{
    for (const QVariantList &row : rows(QStringLiteral("entities")))
        if (row.first().toString() == id) return row;
    return QVariantList();
}
QSet<QString> eventIds(const QVector<EntityEvent> &events)
{
    QSet<QString> result;
    for (const EntityEvent &event : events) result.insert(event.id);
    return result;
}
QVector<EntityEvent> addedEvents(TaskStore *store, const QString &id, const QSet<QString> &before)
{
    QVector<EntityEvent> result;
    for (const EntityEvent &event : store->events(id)) if (!before.contains(event.id)) result.append(event);
    return result;
}
void verifyBusinessEvents(const QVector<EntityEvent> &events)
{
    for (const EntityEvent &event : events) {
        QVERIFY(isVisibleActivityEvent(event));
        QVERIFY(event.field != QStringLiteral("sortOrder"));
        QVERIFY(event.field != QStringLiteral("pinned"));
        QVERIFY(event.field != QStringLiteral("section"));
        QVERIFY(event.field != QStringLiteral("folder"));
    }
}
}

class DropGroupTests : public QObject
{
    Q_OBJECT
private slots:
    void init();
    void cleanup();
    void timeGroupMappings();
    void timeDropPreservesDateSemantics_data();
    void timeDropPreservesDateSemantics();
    void timeDropToUndatedClearsAllDateFields();
    void sameTimeGroupReordersWithoutDateNormalization();
    void filteredReorderNoOpPreservesRanks_data();
    void filteredReorderNoOpPreservesRanks();
    void filteredReorderMergesLatestHiddenSiblings();
    void filteredSiblingReorderKeepsSubtreesAndOtherParents();
    void hiddenSiblingRankCasFailureIsAtomic();
    void sameDisplayGroupDetachMatchesTarget_data();
    void sameDisplayGroupDetachMatchesTarget();
    void beforeAfterMovesToAnchorSiblingParent();
    void pinnedMixedKindsRespectManualOrder();
    void priorityDropDetachesChildAndMovesDisplayGroup();
    void pinnedSiblingReorderAndCrossGroupTransitions();
    void listDropMovesCompleteSubtreeAndMoveToSharesPolicy();
    void asChildPreservesPropertiesAndRejectsHiddenCycles();
    void createdAndInvalidIntentsMakeNoWrites();
    void listDropCasFailureRollsBackHiddenDescendants();
    void dropReloadsLatestRepositorySnapshot();
    void activityPolicyIsAnExplicitWhitelist();
    void activityContextsAndEventOnlySessionSignals();
    void archiveEventsAreAtomicAndListDeletionNotifies();
private:
    std::unique_ptr<QTemporaryDir> m_directory;
};

void DropGroupTests::init()
{
    Database::instance().close(); m_directory.reset(new QTemporaryDir);
    QVERIFY(m_directory->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_directory->path().toUtf8());
    QString error;
    QVERIFY2(Database::instance().openAt(m_directory->filePath(QStringLiteral("kylinlist.db")), &error), qPrintable(error));
}
void DropGroupTests::cleanup()
{
    Database::instance().close(); qunsetenv("KYLINLIST_DATA_DIR"); m_directory.reset();
}

void DropGroupTests::timeGroupMappings()
{
    const QDate day(2026, 11, 15);
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Overdue), day), day.addDays(-1));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Today), day), day);
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Tomorrow), day), day.addDays(1));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Next7Days), day), day.addDays(2));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Later), day), day.addDays(7));
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Undated), day).isValid());
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Today), QDate()).isValid());
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::flat(), day).isValid());
    for (int priority = 0; priority <= 3; ++priority)
        QCOMPARE(TaskDrop::priorityForGroup(GroupKey::forPriority(priority)), priority);
    QCOMPARE(TaskDrop::priorityForGroup(GroupKey::notes()), -1);
}

void DropGroupTests::timeDropPreservesDateSemantics_data()
{
    QTest::addColumn<QDate>("allDayDate");
    QTest::addColumn<QDate>("allDayEndDate");
    QTest::addColumn<QDateTime>("startAt");
    QTest::addColumn<QDateTime>("dueAt");
    QTest::addColumn<bool>("hasRange");
    const QDate from = today().addDays(-4);
    QTest::newRow("undated-to-date-only") << QDate() << QDate() << QDateTime() << QDateTime() << false;
    QTest::newRow("date-only") << from << QDate() << QDateTime() << QDateTime() << false;
    QTest::newRow("explicit-midnight") << QDate() << QDate() << QDateTime() << wall(from, QTime(0, 0)) << false;
    QTest::newRow("timed-seconds-and-milliseconds") << QDate() << QDate() << QDateTime() << wall(from, QTime(17, 26, 43, 219)) << false;
    QTest::newRow("timed-same-day-range") << QDate() << QDate() << wall(from, QTime(8, 12, 34, 567)) << wall(from, QTime(10, 12, 34, 567)) << true;
    QTest::newRow("cross-day-span") << QDate() << QDate() << wall(from.addDays(-2), QTime(23, 59, 58, 123)) << wall(from, QTime(0, 2, 3, 456)) << true;
    QTest::newRow("all-day-range") << from.addDays(-1) << from << QDateTime() << QDateTime() << true;
}
void DropGroupTests::timeDropPreservesDateSemantics()
{
    QFETCH(QDate, allDayDate); QFETCH(QDate, allDayEndDate); QFETCH(QDateTime, startAt); QFETCH(QDateTime, dueAt);
    QFETCH(bool, hasRange);
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("日期语义"));
    Entity value = store.entity(id); value.allDayDate = allDayDate; value.allDayEndDate = allDayEndDate;
    value.startAt = startAt; value.dueAt = dueAt; value.hasScheduleRange = hasRange;
    value.timeZone = QStringLiteral("UTC"); // 拖放按应用时区平移，而不是改实体时区或绝对时刻格式。
    value.reminderAt = utcNow().addSecs(-120); QVERIFY(store.saveTask(value));
    QVERIFY(execute(QStringLiteral("UPDATE reminders SET sort_order=7 WHERE entity_id='%1'").arg(id)));
    QVERIFY(store.load()); // 旧库合法的非连续提醒排序也不得被拖放归一或推进 revision。
    const Entity before = store.entity(id);
    const auto reminderRows = rows(QStringLiteral("reminders"));
    const TaskDrop::Intent intent = intentFor(store, id, GroupMode::Time, GroupKey::forTime(TimeBucket::Tomorrow));
    QSignalSpy changed(&store, &TaskStore::eventsChanged);
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    const Entity moved = store.entity(id); const QDate target = today().addDays(1);
    QCOMPARE(effectiveDate(moved), target); QCOMPARE(moved.timeZone, before.timeZone);
    if (!effectiveDate(before).isValid()) {
        QCOMPARE(moved.allDayDate, target); QVERIFY(!moved.startAt.isValid()); QVERIFY(!moved.dueAt.isValid());
    } else {
        const qint64 days = effectiveDate(before).daysTo(target);
        QCOMPARE(moved.allDayDate, before.allDayDate.isValid() ? before.allDayDate.addDays(days) : QDate());
        QCOMPARE(moved.allDayEndDate, before.allDayEndDate.isValid() ? before.allDayEndDate.addDays(days) : QDate());
        QCOMPARE(moved.hasScheduleRange, before.hasScheduleRange);
        QCOMPARE(moved.startAt, before.startAt.isValid() ? fromAppTime(toAppTime(before.startAt).addDays(days)) : QDateTime());
        QCOMPARE(moved.dueAt, before.dueAt.isValid() ? fromAppTime(toAppTime(before.dueAt).addDays(days)) : QDateTime());
        if (before.startAt.isValid() && before.dueAt.isValid())
            QCOMPARE(moved.startAt.msecsTo(moved.dueAt), before.startAt.msecsTo(before.dueAt));
    }
    QCOMPARE(rows(QStringLiteral("reminders")), reminderRows); QCOMPARE(moved.reminderAt, before.reminderAt);
    QCOMPARE(groupOf(moved, GroupMode::Time, today()), intent.targetGroup); QCOMPARE(changed.count(), 1);
    verifyBusinessEvents(store.events(id));
    const DatabaseState state = databaseState(); changed.clear();
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state); QCOMPARE(changed.count(), 0);
}

void DropGroupTests::timeDropToUndatedClearsAllDateFields()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString id = store.createTask(QStringLiteral("清空日期"));
    Entity value = store.entity(id); value.allDayDate = today().addDays(-2);
    value.allDayEndDate = today(); value.hasScheduleRange = true;
    value.reminderAt = utcNow().addSecs(3600); QVERIFY(store.saveTask(value));
    const auto reminders = rows(QStringLiteral("reminders"));
    const auto intent = intentFor(store, id, GroupMode::Time, GroupKey::forTime(TimeBucket::Undated));
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    const Entity moved = store.entity(id);
    QVERIFY(!moved.allDayDate.isValid()); QVERIFY(!moved.allDayEndDate.isValid());
    QVERIFY(!moved.startAt.isValid()); QVERIFY(!moved.dueAt.isValid()); QVERIFY(!moved.hasScheduleRange);
    QCOMPARE(rows(QStringLiteral("reminders")), reminders);
    const DatabaseState state = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state);
}

void DropGroupTests::sameTimeGroupReordersWithoutDateNormalization()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString a = store.createTask(QStringLiteral("第六天")), b = store.createTask(QStringLiteral("第四天"));
    Entity first = store.entity(a); first.allDayDate = today().addDays(5); first.sortOrder = 9; QVERIFY(store.saveTask(first));
    Entity second = store.entity(b); second.dueAt = wall(today().addDays(3), QTime(0, 0)); second.sortOrder = 8; QVERIFY(store.saveTask(second));
    QVERIFY(store.saveDesktopState(a, NoteColor::Blue, QRect(10, 20, 300, 210), 4, NoteLayer::OnTop, true));
    const auto desktop = rows(QStringLiteral("desktop_notes"));
    const auto events = rows(QStringLiteral("entity_events"));
    TaskDrop::Intent intent = intentFor(store, a, GroupMode::Time, GroupKey::forTime(TimeBucket::Next7Days), {a, b});
    intent.placement = TaskDrop::Placement::Before; intent.anchorId = b;
    QCOMPARE(intent.targetDate, today().addDays(2)); // 映射提示不能把组内原日期统一成后天。
    QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    QCOMPARE(store.entity(a).allDayDate, first.allDayDate); QCOMPARE(store.entity(b).dueAt, second.dueAt);
    QCOMPARE(store.entity(a).sortOrder, qint64(0)); QCOMPARE(store.entity(b).sortOrder, qint64(1));
    QCOMPARE(rows(QStringLiteral("entity_events")), events); QCOMPARE(eventChanged.count(), 0);
    QCOMPARE(rows(QStringLiteral("desktop_notes")), desktop); QCOMPARE(store.entity(a).color, NoteColor::Blue);
    const DatabaseState state = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state);
    intent.targetDate = QDate(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state);
}

void DropGroupTests::filteredReorderNoOpPreservesRanks_data()
{
    QTest::addColumn<qint64>("baseRank");
    QTest::addColumn<qint64>("rankStep");
    QTest::newRow("hidden-rank-zero-collision") << qint64(0) << qint64(1);
    QTest::newRow("sparse-ranks-stay-sparse") << qint64(7) << qint64(11);
}
void DropGroupTests::filteredReorderNoOpPreservesRanks()
{
    QFETCH(qint64, baseRank); QFETCH(qint64, rankStep);
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString a = store.createTask(QStringLiteral("隐藏 A"));
    const QString b = store.createTask(QStringLiteral("可见 B"));
    const QString c = store.createTask(QStringLiteral("可见 C"));
    const QStringList ids{a, b, c};
    const QDateTime created(QDate(2026, 1, 1), QTime(0, 0), Qt::UTC);
    for (int index = 0; index < ids.size(); ++index) {
        Entity value = store.entity(ids.at(index));
        value.sortOrder = baseRank + index * rankStep; value.allDayDate = today().addDays(3);
        QVERIFY(store.saveTask(value));
        QVERIFY(execute(QStringLiteral("UPDATE entities SET created_at_utc_ms=%1 WHERE id='%2'")
            .arg(utcMs(created.addSecs(index))).arg(value.id)));
    }
    QVERIFY(store.load());
    ViewSpec spec; spec.grouping = GroupMode::Time; spec.search = QStringLiteral("可见");
    const GroupKey group = GroupKey::forTime(TimeBucket::Next7Days);
    const QStringList visible = sectionIds(store, spec, group);
    QCOMPARE(visible, (QStringList{b, c}));
    TaskDrop::Intent intent = intentFor(store, c, GroupMode::Time, group, visible);
    intent.placement = TaskDrop::Placement::After; intent.anchorId = b;
    const DatabaseState before = databaseState();
    QSignalSpy changed(&store, &TaskStore::taskChanged), events(&store, &TaskStore::eventsChanged);
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    QVERIFY(databaseState() == before); QCOMPARE(changed.count(), 0); QCOMPARE(events.count(), 0);
    spec.search.clear();
    QCOMPARE(sectionIds(store, spec, group), ids);
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == before);
}

void DropGroupTests::filteredReorderMergesLatestHiddenSiblings()
{
    TaskStore current(Database::instance().connection()); QVERIFY(current.load());
    const QString c = current.createTask(QStringLiteral("源 C"));
    const QString a = current.createTask(QStringLiteral("旧可见 A"));
    const QString b = current.createTask(QStringLiteral("旧可见 B"));
    const QStringList ids{a, b, c};
    const qint64 ranks[] = {7, 11, 29};
    for (int index = 0; index < ids.size(); ++index) {
        Entity value = current.entity(ids.at(index)); value.sortOrder = ranks[index];
        value.allDayDate = today().addDays(3); QVERIFY(current.saveTask(value));
    }
    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    Entity value = current.entity(b); value.sortOrder = 5; QVERIFY(current.saveTask(value));
    value = current.entity(a); value.body = QStringLiteral("最新正文不能被排序覆盖"); QVERIFY(current.saveTask(value));
    const QString hidden = current.createTask(QStringLiteral("新快照才有的隐藏同级"));
    value = current.entity(hidden); value.allDayDate = today().addDays(3); value.sortOrder = 19;
    QVERIFY(current.saveTask(value));
    // 提示仍携带旧 A/B 相对顺序；只移动 C，不可回滚其他写者已提交的 B/A 顺序。
    const GroupKey group = GroupKey::forTime(TimeBucket::Next7Days);
    TaskDrop::Intent intent = intentFor(stale, c, GroupMode::Time, group, {a, c, b});
    intent.placement = TaskDrop::Placement::Before; intent.anchorId = b;
    const auto events = rows(QStringLiteral("entity_events"));
    const auto reminders = rows(QStringLiteral("reminders"));
    QSignalSpy eventChanged(&stale, &TaskStore::eventsChanged);
    QVERIFY2(stale.applyDropIntent(intent), qPrintable(stale.lastError()));
    ViewSpec spec; spec.grouping = GroupMode::Time;
    QCOMPARE(sectionIds(stale, spec, group), (QStringList{c, b, a, hidden}));
    QCOMPARE(stale.entity(a).body, QStringLiteral("最新正文不能被排序覆盖"));
    QCOMPARE(stale.entity(hidden).sortOrder, qint64(3));
    QCOMPARE(stale.entity(c).allDayDate, today().addDays(3));
    QCOMPARE(rows(QStringLiteral("entity_events")), events); QCOMPARE(rows(QStringLiteral("reminders")), reminders);
    QCOMPARE(eventChanged.count(), 0);
    const DatabaseState beforeRepeat = databaseState();
    QVERIFY(stale.applyDropIntent(intent)); QVERIFY(databaseState() == beforeRepeat);
}

void DropGroupTests::filteredSiblingReorderKeepsSubtreesAndOtherParents()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("可见父 P"));
    const QString otherParent = store.createTask(QStringLiteral("可见父 Q"));
    for (const QString &id : {parent, otherParent}) {
        Entity value = store.entity(id); value.allDayDate = today().addDays(3);
        value.sortOrder = id == parent ? 70 : 80; QVERIFY(store.saveTask(value));
    }
    const QString a = store.createSubtask(QStringLiteral("隐藏同级 A"), parent);
    const QString b = store.createSubtask(QStringLiteral("可见同级 B"), parent);
    const QString c = store.createSubtask(QStringLiteral("可见源 C"), parent);
    const QString grandchild = store.createSubtask(QStringLiteral("可见源后代"), c);
    const QString foreign = store.createSubtask(QStringLiteral("可见其他父级孩子"), otherParent);
    const QString completed = store.createSubtask(QStringLiteral("完成同级"), parent);
    const QString abandoned = store.createSubtask(QStringLiteral("放弃同级"), parent);
    const QString trashed = store.createSubtask(QStringLiteral("回收站同级"), parent);
    const QString archivedList = store.createList(QStringLiteral("归档边界"));
    const QString archived = store.createTask(QStringLiteral("归档任务"), archivedList);
    const QStringList ordered{a, b, c, grandchild, foreign, completed, abandoned, trashed};
    const qint64 ranks[] = {7, 11, 21, 57, 91, 2, 3, 4};
    for (int index = 0; index < ordered.size(); ++index) {
        Entity value = store.entity(ordered.at(index)); value.sortOrder = ranks[index];
        if (value.id == c) { value.allDayDate = today().addDays(5); value.pinnedAt = utcNow(); }
        QVERIFY(store.saveTask(value));
    }
    QVERIFY(store.setCompleted(completed, true)); QVERIFY(store.abandonTask(abandoned));
    QVERIFY(store.trashTask(trashed)); QVERIFY(store.setListArchived(archivedList, true));
    const Entity oldSource = store.entity(c);
    const QStringList untouched{parent, otherParent, grandchild, foreign, completed, abandoned, trashed, archived};
    QHash<QString, QVariantList> oldRows;
    for (const QString &id : untouched) oldRows.insert(id, entityRow(id));
    ViewSpec spec; spec.grouping = GroupMode::Time; spec.search = QStringLiteral("可见");
    const GroupKey group = GroupKey::forTime(TimeBucket::Next7Days);
    QStringList hint = sectionIds(store, spec, group);
    QVERIFY(!hint.contains(a)); QVERIFY(hint.contains(parent) && hint.contains(foreign));
    hint.removeAll(c); hint.removeAll(grandchild);
    const int at = hint.indexOf(b); QVERIFY(at >= 0); hint.insert(at, c); hint.insert(at + 1, grandchild);
    TaskDrop::Intent intent = intentFor(store, c, GroupMode::Time, group, hint);
    intent.sourceGroup = group; intent.placement = TaskDrop::Placement::Before; intent.anchorId = b;
    const auto events = rows(QStringLiteral("entity_events"));
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    spec.search.clear();
    const Grouping grouping = groupEntities(store.entities(), store.lists(), spec, today(), store.archivedListIds());
    QStringList children;
    for (const Entity &entity : grouping.children.value(parent)) children.append(entity.id);
    QCOMPARE(children, (QStringList{a, c, b}));
    QCOMPARE(store.entity(c).parentId, parent); QCOMPARE(store.entity(c).allDayDate, oldSource.allDayDate);
    QCOMPARE(store.entity(c).pinnedAt, oldSource.pinnedAt);
    for (const QString &id : untouched) QCOMPARE(entityRow(id), oldRows.value(id));
    QCOMPARE(rows(QStringLiteral("entity_events")), events);
    const DatabaseState repeated = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == repeated);
}

void DropGroupTests::hiddenSiblingRankCasFailureIsAtomic()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    QVERIFY(store.setLocalMeta(QStringLiteral("signed_in"), QStringLiteral("1")));
    const QString source = store.createTask(QStringLiteral("先保存的源"));
    const QString anchor = store.createTask(QStringLiteral("参照同级"));
    const QString hidden = store.createTask(QStringLiteral("后保存的隐藏同级"));
    const QStringList ids{source, anchor, hidden}; const qint64 ranks[] = {29, 11, 7};
    const QDateTime created(QDate(2026, 1, 1), QTime(0, 0), Qt::UTC);
    for (int index = 0; index < ids.size(); ++index) {
        Entity value = store.entity(ids.at(index)); value.sortOrder = ranks[index]; QVERIFY(store.saveTask(value));
        QVERIFY(execute(QStringLiteral("UPDATE entities SET created_at_utc_ms=%1 WHERE id='%2'")
            .arg(utcMs(created.addSecs(index))).arg(value.id)));
    }
    QVERIFY(store.load());
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER invalidate_hidden_rank AFTER UPDATE ON entities WHEN NEW.id='%1' "
        "BEGIN UPDATE entities SET revision=revision+1 WHERE id='%2'; END").arg(source, hidden)));
    TaskDrop::Intent intent = intentFor(store, source, GroupMode::None, GroupKey::flat(), {source, anchor});
    intent.placement = TaskDrop::Placement::Before; intent.anchorId = anchor;
    const DatabaseState before = databaseState();
    QSignalSpy changed(&store, &TaskStore::taskChanged), events(&store, &TaskStore::eventsChanged);
    QVERIFY(!store.applyDropIntent(intent)); QVERIFY(!store.lastError().isEmpty()); QVERIFY(databaseState() == before);
    QCOMPARE(changed.count(), 0); QCOMPARE(events.count(), 0); QCOMPARE(store.entity(hidden).sortOrder, qint64(7));
    QVERIFY(execute(QStringLiteral("DROP TRIGGER invalidate_hidden_rank")));
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    ViewSpec spec; spec.grouping = GroupMode::None;
    QCOMPARE(sectionIds(store, spec, GroupKey::flat()), (QStringList{hidden, source, anchor}));
    QCOMPARE(store.entity(hidden).sortOrder, qint64(0)); QCOMPARE(events.count(), 0);
}

void DropGroupTests::sameDisplayGroupDetachMatchesTarget_data()
{
    QTest::addColumn<int>("modeValue");
    QTest::addColumn<QString>("targetId");
    QTest::addColumn<int>("childOffset");
    QTest::addColumn<bool>("timed");
    QTest::addColumn<bool>("ownPin");
    QTest::newRow("inherited-today-date-only") << int(GroupMode::Time) << QStringLiteral("time:today") << 1 << false << false;
    QTest::newRow("inherited-today-explicit-midnight-span") << int(GroupMode::Time) << QStringLiteral("time:today") << 1 << true << true;
    QTest::newRow("matching-next7-date-is-not-snapped") << int(GroupMode::Time) << QStringLiteral("time:next7days") << 5 << true << true;
    QTest::newRow("inherited-high-priority") << int(GroupMode::Priority) << QStringLiteral("priority:3") << 5 << false << false;
    QTest::newRow("inherited-flat-clears-own-pin") << int(GroupMode::None) << QStringLiteral("none") << 5 << false << true;
    QTest::newRow("inherited-pinned-becomes-own-pin") << int(GroupMode::Time) << QStringLiteral("pinned") << 5 << true << false;
}
void DropGroupTests::sameDisplayGroupDetachMatchesTarget()
{
    QFETCH(int, modeValue); QFETCH(QString, targetId); QFETCH(int, childOffset);
    QFETCH(bool, timed); QFETCH(bool, ownPin);
    const GroupMode mode = static_cast<GroupMode>(modeValue);
    const GroupKey group = GroupKey::fromStableId(targetId);
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("继承显示组的父项"));
    const QString anchor = store.createTask(QStringLiteral("同显示组独立锚点"));
    for (const QString &id : {parent, anchor}) {
        Entity value = store.entity(id); value.sortOrder = id == parent ? 7 : 11;
        value.allDayDate = group.kind == GroupKey::Time ? TaskDrop::dateForGroup(group, today()) : today();
        value.priority = 3;
        if (group.kind == GroupKey::Pinned) value.pinnedAt = utcNow().addSecs(-100);
        QVERIFY(store.saveTask(value));
    }
    const QString child = store.createSubtask(QStringLiteral("真实属性不同的子项"), parent);
    Entity value = store.entity(child); value.priority = 1; value.timeZone = QStringLiteral("UTC");
    value.pinnedAt = ownPin ? utcNow().addSecs(-50) : QDateTime();
    const QDate childDay = today().addDays(childOffset);
    if (timed) {
        value.allDayDate = QDate();
        value.startAt = wall(childDay.addDays(-2), QTime(19, 45, 16, 123));
        value.dueAt = wall(childDay, QTime(0, 0, 0, 235));
        value.hasScheduleRange = true;
    } else value.allDayDate = childDay;
    value.reminderAt = utcNow().addSecs(3600); QVERIFY(store.saveTask(value));
    const QString grandchild = store.createSubtask(QStringLiteral("拖动子树后代"), child);
    QVERIFY(execute(QStringLiteral("UPDATE reminders SET sort_order=7 WHERE entity_id='%1'").arg(child)));
    QVERIFY(store.load());
    const Entity before = store.entity(child);
    const QVariantList descendantRow = entityRow(grandchild);
    const auto reminders = rows(QStringLiteral("reminders"));
    const QSet<QString> oldEvents = eventIds(store.events(child));
    TaskDrop::Intent intent = intentFor(store, child, mode, group, {parent, anchor, child, grandchild});
    intent.sourceGroup = group; intent.placement = TaskDrop::Placement::After; intent.anchorId = anchor;
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    const Entity moved = store.entity(child);
    QVERIFY(moved.parentId.isEmpty()); QCOMPARE(moved.kind, EntityKind::Task);
    QCOMPARE(groupOf(moved, mode, today()), group);
    const bool shift = group.kind == GroupKey::Time && timeBucketOf(before, today()) != group.timeBucket();
    const qint64 days = shift ? effectiveDate(before).daysTo(intent.targetDate) : 0;
    QCOMPARE(moved.allDayDate, before.allDayDate.isValid() ? before.allDayDate.addDays(days) : QDate());
    QCOMPARE(moved.allDayEndDate, before.allDayEndDate.isValid() ? before.allDayEndDate.addDays(days) : QDate());
    QCOMPARE(moved.hasScheduleRange, before.hasScheduleRange);
    QCOMPARE(moved.startAt, before.startAt.isValid() ? fromAppTime(toAppTime(before.startAt).addDays(days)) : QDateTime());
    QCOMPARE(moved.dueAt, before.dueAt.isValid() ? fromAppTime(toAppTime(before.dueAt).addDays(days)) : QDateTime());
    QCOMPARE(moved.priority, group.kind == GroupKey::Priority ? 3 : before.priority);
    QCOMPARE(moved.pinnedAt.isValid(), group.kind == GroupKey::Pinned);
    QCOMPARE(moved.timeZone, before.timeZone); QCOMPARE(rows(QStringLiteral("reminders")), reminders);
    QCOMPARE(entityRow(grandchild), descendantRow); QCOMPARE(store.entity(grandchild).parentId, child);
    const auto added = addedEvents(&store, child, oldEvents);
    bool parentChanged = false;
    for (const EntityEvent &event : added) if (event.field == QStringLiteral("parent")) parentChanged = true;
    QVERIFY(parentChanged); verifyBusinessEvents(added);
    const DatabaseState repeated = databaseState();
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == repeated);
}

void DropGroupTests::beforeAfterMovesToAnchorSiblingParent()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString oldParent = store.createTask(QStringLiteral("旧父项"));
    const QString targetParent = store.createTask(QStringLiteral("目标父项"));
    for (const QString &id : {oldParent, targetParent}) {
        Entity value = store.entity(id); value.allDayDate = today(); QVERIFY(store.saveTask(value));
    }
    const QString source = store.createSubtask(QStringLiteral("跨父级同级移动"), oldParent);
    const QString anchor = store.createSubtask(QStringLiteral("目标兄弟"), targetParent);
    Entity value = store.entity(source); value.allDayDate = today().addDays(1); value.pinnedAt = utcNow();
    value.priority = 2; QVERIFY(store.saveTask(value));
    const Entity before = store.entity(source);
    TaskDrop::Intent intent = intentFor(store, source, GroupMode::Time, GroupKey::forTime(TimeBucket::Today),
                                       {oldParent, targetParent, source, anchor});
    intent.sourceGroup = intent.targetGroup; intent.placement = TaskDrop::Placement::Before; intent.anchorId = anchor;
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    const Entity moved = store.entity(source);
    QCOMPARE(moved.parentId, targetParent); QCOMPARE(moved.allDayDate, before.allDayDate);
    QCOMPARE(moved.pinnedAt, before.pinnedAt); QCOMPARE(moved.priority, before.priority);
    ViewSpec spec;
    const Grouping grouping = groupEntities(store.entities(), store.lists(), spec, today());
    QCOMPARE(grouping.children.value(targetParent).first().id, source);
    const DatabaseState repeated = databaseState();
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == repeated);
    // 同级落点不是绕过设父边界的入口：不同清单、终态父级和环均不得写入。
    const QString otherList = store.createList(QStringLiteral("其他清单"));
    const QString foreignParent = store.createTask(QStringLiteral("外部父项"), otherList);
    const QString foreign = store.createSubtask(QStringLiteral("外部兄弟"), foreignParent);
    value = store.entity(foreignParent); value.allDayDate = today(); QVERIFY(store.saveTask(value));
    intent.anchorId = foreign; intent.orderedIds = QStringList{source, foreign};
    const DatabaseState rejected = databaseState(); QVERIFY(!store.applyDropIntent(intent)); QVERIFY(databaseState() == rejected);
}

void DropGroupTests::pinnedMixedKindsRespectManualOrder()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString task = store.createTask(QStringLiteral("置顶任务"));
    const QString note = store.createNote(QStringLiteral("置顶笔记"));
    const QDateTime taskPin = utcNow().addSecs(-100), notePin = utcNow().addSecs(-50);
    QVERIFY(store.setPinnedAt(task, taskPin)); QVERIFY(store.setPinnedAt(note, notePin));
    Entity value = store.entity(task); value.sortOrder = 0; value.reminderAt = utcNow().addSecs(3600); QVERIFY(store.saveTask(value));
    value = store.entity(note); value.sortOrder = 1; QVERIFY(store.saveTask(value));
    QVERIFY(execute(QStringLiteral("UPDATE reminders SET sort_order=7 WHERE entity_id='%1'").arg(task)));
    QVERIFY(store.load());
    const auto events = rows(QStringLiteral("entity_events")), reminders = rows(QStringLiteral("reminders"));
    TaskDrop::Intent intent = intentFor(store, note, GroupMode::Time, GroupKey::pinned(), {note, task});
    intent.placement = TaskDrop::Placement::Before; intent.anchorId = task;
    QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    ViewSpec spec;
    QCOMPARE(sectionIds(store, spec, GroupKey::pinned()), (QStringList{note, task}));
    QCOMPARE(store.entity(note).sortOrder, qint64(0)); QCOMPARE(store.entity(task).sortOrder, qint64(1));
    QCOMPARE(store.entity(task).pinnedAt, taskPin); QCOMPARE(store.entity(note).pinnedAt, notePin);
    QCOMPARE(store.entity(task).kind, EntityKind::Task); QCOMPARE(store.entity(note).kind, EntityKind::Note);
    QCOMPARE(rows(QStringLiteral("entity_events")), events); QCOMPARE(rows(QStringLiteral("reminders")), reminders);
    QCOMPARE(eventChanged.count(), 0);
    const DatabaseState repeated = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == repeated);
}

void DropGroupTests::priorityDropDetachesChildAndMovesDisplayGroup()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("低优先主任务"));
    Entity value = store.entity(parent); value.priority = 1; QVERIFY(store.saveTask(value));
    const QString child = store.createSubtask(QStringLiteral("独立拖动子项"), parent);
    const QString grandchild = store.createSubtask(QStringLiteral("保留关系的孙项"), child);
    const QString anchor = store.createTask(QStringLiteral("高优先落点"));
    value = store.entity(child); value.priority = 0; value.allDayDate = today().addDays(5);
    value.pinnedAt = utcNow().addSecs(-100); QVERIFY(store.saveTask(value));
    value = store.entity(grandchild); value.priority = 2; QVERIFY(store.saveTask(value));
    value = store.entity(anchor); value.priority = 3; QVERIFY(store.saveTask(value));
    const Entity oldChild = store.entity(child); const auto before = eventIds(store.events(child));
    TaskDrop::Intent intent = intentFor(store, child, GroupMode::Priority, GroupKey::forPriority(3), {child, grandchild, anchor});
    intent.sourceGroup = GroupKey::forPriority(1); // 子项真实置顶/priority 不等于父项继承的显示组。
    intent.placement = TaskDrop::Placement::Before; intent.anchorId = anchor;
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    QVERIFY(store.entity(child).parentId.isEmpty()); QVERIFY(!store.entity(child).pinnedAt.isValid());
    QCOMPARE(store.entity(child).priority, 3); QCOMPARE(store.entity(child).allDayDate, oldChild.allDayDate);
    QCOMPARE(store.entity(grandchild).parentId, child); QCOMPARE(store.entity(grandchild).priority, 2);
    QCOMPARE(store.entity(parent).priority, 1);
    ViewSpec spec; spec.grouping = GroupMode::Priority;
    const Grouping grouping = groupEntities(store.entities(), store.lists(), spec, today());
    bool sourceVisibleInTarget = false;
    for (const GroupedSection &section : grouping.sections) if (section.group == intent.targetGroup)
        for (const Entity &entity : section.items) if (entity.id == child) sourceVisibleInTarget = true;
    QVERIFY(sourceVisibleInTarget); QCOMPARE(grouping.children.value(child).first().id, grandchild);
    const auto added = addedEvents(&store, child, before); QSet<QString> fields;
    for (const EntityEvent &event : added) {
        fields.insert(event.field);
        if (event.field == QStringLiteral("parent")) {
            QCOMPARE(event.context.value(QStringLiteral("oldParentTitle")).toString(), QStringLiteral("低优先主任务"));
            QCOMPARE(event.context.value(QStringLiteral("newParentTitle")).toString(), QString());
        }
    }
    QVERIFY(fields == QSet<QString>({QStringLiteral("parent"), QStringLiteral("priority")}));
    verifyBusinessEvents(added);
    const DatabaseState state = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state);
}

void DropGroupTests::pinnedSiblingReorderAndCrossGroupTransitions()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("置顶父项"));
    QVERIFY(store.setPinnedAt(parent, utcNow().addSecs(-600)));
    const QString first = store.createSubtask(QStringLiteral("无自身置顶"), parent);
    const QString second = store.createSubtask(QStringLiteral("有自身置顶"), parent);
    const QDateTime ownPin = utcNow().addSecs(-300); QVERIFY(store.setPinnedAt(second, ownPin));
    const auto events = rows(QStringLiteral("entity_events"));
    TaskDrop::Intent reorder = intentFor(store, second, GroupMode::Time, GroupKey::pinned(), {parent, second, first});
    reorder.placement = TaskDrop::Placement::Before; reorder.anchorId = first;
    QVERIFY2(store.applyDropIntent(reorder), qPrintable(store.lastError()));
    QCOMPARE(store.entity(second).parentId, parent); QCOMPARE(store.entity(second).pinnedAt, ownPin);
    QCOMPARE(store.entity(first).parentId, parent); QVERIFY(!store.entity(first).pinnedAt.isValid());
    QCOMPARE(rows(QStringLiteral("entity_events")), events);
    TaskDrop::Intent leave = intentFor(store, first, GroupMode::Time, GroupKey::forTime(TimeBucket::Tomorrow));
    leave.sourceGroup = GroupKey::pinned();
    QVERIFY2(store.applyDropIntent(leave), qPrintable(store.lastError()));
    QVERIFY(store.entity(first).parentId.isEmpty()); QVERIFY(!store.entity(first).pinnedAt.isValid());
    QCOMPARE(store.entity(first).allDayDate, today().addDays(1));
    const auto pin = intentFor(store, first, GroupMode::Time, GroupKey::pinned());
    const auto beforePinEvents = rows(QStringLiteral("entity_events"));
    QVERIFY(store.applyDropIntent(pin)); QVERIFY(store.entity(first).pinnedAt.isValid());
    QCOMPARE(rows(QStringLiteral("entity_events")), beforePinEvents);
    const DatabaseState state = databaseState(); QVERIFY(store.applyDropIntent(pin)); QVERIFY(databaseState() == state);
    const auto unpin = intentFor(store, first, GroupMode::Time, GroupKey::forTime(TimeBucket::Tomorrow));
    QVERIFY(store.applyDropIntent(unpin)); QVERIFY(!store.entity(first).pinnedAt.isValid());
    QCOMPARE(rows(QStringLiteral("entity_events")), beforePinEvents);
}

void DropGroupTests::listDropMovesCompleteSubtreeAndMoveToSharesPolicy()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString a = store.createList(QStringLiteral("来源清单")), b = store.createList(QStringLiteral("目标清单"));
    const QString c = store.createList(QStringLiteral("命令目标"));
    const QString sectionA = store.createSection(a, QStringLiteral("旧分段"));
    const QString sectionB = store.createSection(b, QStringLiteral("目标原分段"));
    const QString parent = store.createTask(QStringLiteral("留在来源的主项"), a, sectionA);
    const QString source = store.createTask(QStringLiteral("源子树"), a, sectionA, parent);
    const QString visible = store.createTask(QStringLiteral("可见孩子"), a, sectionA, source);
    const QString hidden = store.createTask(QStringLiteral("折叠隐藏孙项"), a, sectionA, visible);
    const QString completed = store.createTask(QStringLiteral("已完成孩子"), a, sectionA, source);
    const QString abandoned = store.createTask(QStringLiteral("放弃后代"), a, sectionA, completed);
    const QString trashed = store.createTask(QStringLiteral("回收站后代"), a, sectionA, visible);
    const QString anchor = store.createTask(QStringLiteral("目标参照"), b, sectionB);
    QVERIFY(store.setCompleted(completed, true)); QVERIFY(store.abandonTask(abandoned)); QVERIFY(store.trashTask(trashed));
    QVERIFY(store.setPinnedAt(source, utcNow().addSecs(-20)));
    const QStringList subtree{source, visible, hidden, completed, abandoned, trashed};
    QHash<QString, Entity> originals;
    for (const QString &id : subtree) originals.insert(id, store.entity(id));
    const auto before = eventIds(store.events(source));
    TaskDrop::Intent intent = intentFor(store, source, GroupMode::List, GroupKey::forList(b), {source, visible, anchor});
    intent.sourceGroup = GroupKey::forList(a); intent.placement = TaskDrop::Placement::Before; intent.anchorId = anchor;
    QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    QCOMPARE(eventChanged.count(), subtree.size());
    for (const QString &id : subtree) {
        const Entity moved = store.entity(id); const Entity old = originals.value(id);
        QCOMPARE(moved.listId, b); QVERIFY(moved.sectionId.isEmpty());
        QCOMPARE(moved.status, old.status); QCOMPARE(moved.deletedAt, old.deletedAt);
        QCOMPARE(moved.startAt, old.startAt); QCOMPARE(moved.dueAt, old.dueAt); QCOMPARE(moved.priority, old.priority);
        QCOMPARE(moved.parentId, id == source ? QString() : old.parentId);
        verifyBusinessEvents(store.events(id));
    }
    QVERIFY(!store.entity(source).pinnedAt.isValid()); QCOMPARE(store.entity(parent).listId, a);
    QCOMPARE(store.entity(anchor).sectionId, sectionB);
    for (const EntityEvent &event : addedEvents(&store, source, before)) if (event.field == QStringLiteral("list")) {
        QCOMPARE(event.context.value(QStringLiteral("oldListName")).toString(), QStringLiteral("来源清单"));
        QCOMPARE(event.context.value(QStringLiteral("newListName")).toString(), QStringLiteral("目标清单"));
    }
    const DatabaseState state = databaseState(); eventChanged.clear();
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state); QCOMPARE(eventChanged.count(), 0);
    const QDateTime ownPin = utcNow().addSecs(-10); QVERIFY(store.setPinnedAt(visible, ownPin));
    QVERIFY2(store.moveEntityToList(visible, c), qPrintable(store.lastError()));
    for (const QString &id : {visible, hidden, trashed}) { QCOMPARE(store.entity(id).listId, c); QVERIFY(store.entity(id).sectionId.isEmpty()); }
    QVERIFY(store.entity(visible).parentId.isEmpty()); QCOMPARE(store.entity(hidden).parentId, visible);
    QCOMPARE(store.entity(visible).pinnedAt, ownPin); QCOMPARE(store.entity(trashed).deletedAt, originals.value(trashed).deletedAt);
    QCOMPARE(store.entity(source).listId, b); QCOMPARE(store.entity(completed).listId, b); QCOMPARE(store.entity(abandoned).listId, b);
    const DatabaseState moved = databaseState(); QVERIFY(store.moveEntityToList(visible, c)); QVERIFY(databaseState() == moved);
    QVERIFY(store.setListArchived(a, true)); const DatabaseState archived = databaseState();
    QVERIFY(!store.moveEntityToList(visible, a)); QVERIFY(databaseState() == archived);
}

void DropGroupTests::asChildPreservesPropertiesAndRejectsHiddenCycles()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString oldParent = store.createTask(QStringLiteral("旧父标题")), newParent = store.createTask(QStringLiteral("新父标题"));
    QVERIFY(store.setPinnedAt(oldParent, utcNow().addSecs(-60))); QVERIFY(store.setPinnedAt(newParent, utcNow().addSecs(-50)));
    const QString source = store.createSubtask(QStringLiteral("源"), oldParent);
    const QString hidden = store.createSubtask(QStringLiteral("隐藏孩子"), source);
    const QString descendant = store.createSubtask(QStringLiteral("隐藏孙项"), hidden);
    Entity value = store.entity(source); value.allDayDate = today().addDays(4); value.priority = 1;
    value.pinnedAt = utcNow().addSecs(-40); value.reminderAt = utcNow().addSecs(7200); QVERIFY(store.saveTask(value));
    value = store.entity(newParent); value.allDayDate = today(); value.priority = 3; QVERIFY(store.saveTask(value));
    const Entity before = store.entity(source); const auto reminderRows = rows(QStringLiteral("reminders"));
    const auto events = eventIds(store.events(source));
    TaskDrop::Intent intent = intentFor(store, source, GroupMode::Priority, GroupKey::pinned(), {newParent, source});
    intent.sourceGroup = GroupKey::pinned(); intent.placement = TaskDrop::Placement::AsChild; intent.targetParentId = newParent;
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    const Entity moved = store.entity(source); QCOMPARE(moved.parentId, newParent);
    QCOMPARE(moved.priority, before.priority); QCOMPARE(moved.allDayDate, before.allDayDate); QCOMPARE(moved.pinnedAt, before.pinnedAt);
    QCOMPARE(rows(QStringLiteral("reminders")), reminderRows); QCOMPARE(store.entity(hidden).parentId, source);
    QCOMPARE(store.entity(descendant).parentId, hidden);
    const auto added = addedEvents(&store, source, events); QCOMPARE(added.size(), 1); QCOMPARE(added.first().field, QStringLiteral("parent"));
    QCOMPARE(added.first().context.value(QStringLiteral("oldParentTitle")).toString(), QStringLiteral("旧父标题"));
    QCOMPARE(added.first().context.value(QStringLiteral("newParentTitle")).toString(), QStringLiteral("新父标题"));
    const DatabaseState state = databaseState(); QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == state);
    TaskDrop::Intent cycle = intentFor(store, newParent, GroupMode::Priority, GroupKey::pinned(), {descendant, newParent});
    cycle.placement = TaskDrop::Placement::AsChild; cycle.targetParentId = descendant;
    QVERIFY(!store.applyDropIntent(cycle)); QVERIFY(databaseState() == state); // orderedIds 故意省去整条中间祖先链。
    const QString otherList = store.createList(QStringLiteral("不同清单"));
    const QString foreign = store.createTask(QStringLiteral("跨清单父项"), otherList);
    const QString note = store.createNote(QStringLiteral("不可作父的笔记"));
    const QString completed = store.createTask(QStringLiteral("不可作父的终态")); QVERIFY(store.setCompleted(completed, true));
    const QString deleted = store.createTask(QStringLiteral("不可作父的删除项")); QVERIFY(store.trashTask(deleted));
    for (const QString &parent : {foreign, note, completed, deleted}) {
        TaskDrop::Intent rejected = intent; rejected.targetParentId = parent; rejected.orderedIds = QStringList{parent, source};
        const DatabaseState current = databaseState(); QVERIFY(!store.applyDropIntent(rejected)); QVERIFY(databaseState() == current);
    }
}

void DropGroupTests::createdAndInvalidIntentsMakeNoWrites()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString source = store.createTask(QStringLiteral("验证源")), anchor = store.createTask(QStringLiteral("参照"));
    const QString terminal = store.createTask(QStringLiteral("终态源")); QVERIFY(store.setCompleted(terminal, true));
    const QString note = store.createNote(QStringLiteral("笔记"));
    const QString archivedList = store.createList(QStringLiteral("已归档目标")); QVERIFY(store.setListArchived(archivedList, true));
    TaskDrop::Intent base = intentFor(store, source, GroupMode::None, GroupKey::flat(), {source, anchor});
    QVector<TaskDrop::Intent> invalid;
    TaskDrop::Intent value = base; value.mode = GroupMode::Created; invalid.append(value);
    value = base; value.mode = GroupMode::Created; value.sourceGroup = GroupKey::pinned(); value.targetGroup = GroupKey::pinned(); invalid.append(value);
    value = base; value.sourceId = newUuid(); invalid.append(value);
    value = base; value.orderedIds.append(newUuid()); invalid.append(value);
    value = base; value.orderedIds.append(anchor); invalid.append(value);
    value = base; value.orderedIds = QStringList{anchor}; invalid.append(value);
    value = base; value.orderedIds.clear(); invalid.append(value);
    value = base; value.placement = TaskDrop::Placement::Before; value.anchorId = newUuid(); invalid.append(value);
    value = base; value.placement = TaskDrop::Placement::After; value.anchorId = anchor; invalid.append(value);
    value = base; value.targetGroup = GroupKey::forPriority(3); invalid.append(value);
    value = intentFor(store, source, GroupMode::Priority, GroupKey::forPriority(4)); invalid.append(value);
    value = base; value.targetGroup = GroupKey::completed(); invalid.append(value);
    value = base; value.targetGroup = GroupKey::abandoned(); invalid.append(value);
    value = base; value.targetGroup = GroupKey::trash(); invalid.append(value);
    value = base; value.targetGroup = GroupKey::notes(); invalid.append(value);
    value = intentFor(store, terminal, GroupMode::None, GroupKey::flat()); invalid.append(value);
    value = intentFor(store, note, GroupMode::Time, GroupKey::forTime(TimeBucket::Today)); invalid.append(value);
    value = intentFor(store, source, GroupMode::List, GroupKey::forList(archivedList)); invalid.append(value);
    value = intentFor(store, source, GroupMode::Time, GroupKey::forTime(TimeBucket::Today)); value.targetDate = today().addDays(1); invalid.append(value);
    value = intentFor(store, source, GroupMode::Time, GroupKey::forTime(TimeBucket::Today)); value.targetDate = QDate(); invalid.append(value);
    value = base; value.targetDate = today(); invalid.append(value);
    const DatabaseState before = databaseState();
    QSignalSpy changed(&store, &TaskStore::taskChanged); QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    for (const TaskDrop::Intent &intent : invalid) {
        QVERIFY(!store.applyDropIntent(intent)); QVERIFY(!store.lastError().isEmpty()); QVERIFY(databaseState() == before);
    }
    QCOMPARE(changed.count(), 0); QCOMPARE(eventChanged.count(), 0);
    // 笔记只在 notes/pinned 间移动，不把任务转换为笔记或反向转换。
    const auto pinNote = intentFor(store, note, GroupMode::None, GroupKey::pinned()); QVERIFY(store.applyDropIntent(pinNote));
    const auto unpinNote = intentFor(store, note, GroupMode::None, GroupKey::notes()); QVERIFY(store.applyDropIntent(unpinNote));
    QCOMPARE(store.entity(note).kind, EntityKind::Note); QVERIFY(!store.entity(note).pinnedAt.isValid());
}

void DropGroupTests::listDropCasFailureRollsBackHiddenDescendants()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    QVERIFY(store.setLocalMeta(QStringLiteral("signed_in"), QStringLiteral("1")));
    const QString a = store.createList(QStringLiteral("CAS 来源")), b = store.createList(QStringLiteral("CAS 目标"));
    const QString source = store.createTask(QStringLiteral("CAS 根"), a);
    const QString visible = store.createSubtask(QStringLiteral("CAS 可见"), source);
    const QString hidden = store.createSubtask(QStringLiteral("CAS 隐藏终态"), visible);
    QVERIFY(store.setCompleted(hidden, true));
    const QStringList ids{source, visible, hidden};
    const QDateTime created(QDate(2026, 1, 1), QTime(0, 0), Qt::UTC);
    // 仓储按创建时刻／ID 写入，不按 sortOrder；同一毫秒创建时不能依赖 UUID 顺序。
    for (int index = 0; index < ids.size(); ++index) {
        Entity value = store.entity(ids.at(index)); value.sortOrder = index * 10; QVERIFY(store.saveTask(value));
        QVERIFY(execute(QStringLiteral("UPDATE entities SET created_at_utc_ms=%1 WHERE id='%2'")
            .arg(utcMs(created.addSecs(index))).arg(value.id)));
    }
    QVERIFY(store.load());
    // 在第一行保存后推进隐藏后代 revision，精确制造第二阶段 CAS 失败；触发器写也必须回滚。
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER invalidate_hidden_drop_revision AFTER UPDATE ON entities WHEN NEW.id='%1' "
        "BEGIN UPDATE entities SET revision=revision+1 WHERE id='%2'; END").arg(source, hidden)));
    const DatabaseState before = databaseState();
    QSignalSpy changed(&store, &TaskStore::taskChanged); QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    const auto intent = intentFor(store, source, GroupMode::List, GroupKey::forList(b), {source, visible});
    QVERIFY(!store.applyDropIntent(intent)); QVERIFY(!store.lastError().isEmpty()); QVERIFY(databaseState() == before);
    QCOMPARE(store.entity(source).listId, a); QCOMPARE(store.entity(hidden).listId, a);
    QCOMPARE(changed.count(), 0); QCOMPARE(eventChanged.count(), 0);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER invalidate_hidden_drop_revision")));
    QVERIFY2(store.applyDropIntent(intent), qPrintable(store.lastError()));
    QCOMPARE(store.entity(hidden).listId, b); QCOMPARE(store.entity(hidden).status, TaskStatus::Completed);
    const DatabaseState committed = databaseState(); changed.clear(); eventChanged.clear();
    QVERIFY(store.applyDropIntent(intent)); QVERIFY(databaseState() == committed);
    QCOMPARE(changed.count(), 0); QCOMPARE(eventChanged.count(), 0);
}

void DropGroupTests::dropReloadsLatestRepositorySnapshot()
{
    TaskStore current(Database::instance().connection()); QVERIFY(current.load());
    const QString a = current.createList(QStringLiteral("最新来源")), b = current.createList(QStringLiteral("最新目标"));
    const QString id = current.createTask(QStringLiteral("旧标题"), a);
    TaskStore stale(Database::instance().connection()); QVERIFY(stale.load());
    Entity fresh = current.entity(id); fresh.title = QStringLiteral("其他写者的最新标题");
    fresh.body = QStringLiteral("最新正文不得被拖放覆盖"); fresh.allDayDate = today().addDays(4); fresh.priority = 2;
    QVERIFY(current.saveTask(fresh));
    const QString hidden = current.createSubtask(QStringLiteral("旧快照不知道的后代"), id);
    QVERIFY(current.setCompleted(hidden, true));
    const auto intent = intentFor(stale, id, GroupMode::List, GroupKey::forList(b));
    QVERIFY2(stale.applyDropIntent(intent), qPrintable(stale.lastError()));
    QCOMPARE(stale.entity(id).title, fresh.title); QCOMPARE(stale.entity(id).body, fresh.body);
    QCOMPARE(stale.entity(id).allDayDate, fresh.allDayDate); QCOMPARE(stale.entity(id).priority, fresh.priority);
    QCOMPARE(stale.entity(hidden).listId, b); QCOMPARE(stale.entity(hidden).status, TaskStatus::Completed);
    QVERIFY(current.load()); QCOMPARE(current.entity(hidden).listId, b);
    const QString c = current.createList(QStringLiteral("刚创建的新目标"));
    QVERIFY2(stale.moveEntityToList(id, c), qPrintable(stale.lastError()));
    QCOMPARE(stale.entity(id).listId, c); QCOMPARE(stale.entity(hidden).listId, c);
}

void DropGroupTests::activityPolicyIsAnExplicitWhitelist()
{
    EntityEvent event;
    for (const char *type : {"created", "trashed", "restored", "deleted", "kind_changed", "attachment_added", "attachment_removed", "reminder_added", "reminder_removed"}) {
        event.type = QString::fromLatin1(type); QVERIFY(isVisibleActivityEvent(event));
    }
    event.type = QStringLiteral("changed");
    for (const char *field : {"title", "body", "status", "due", "start", "allDayDate", "priority", "list", "parent", "timeZone", "recurrence", "tags", "listArchived", "reminder.abc"}) {
        event.field = QString::fromLatin1(field); QVERIFY(isVisibleActivityEvent(event));
    }
    for (const char *field : {"sortOrder", "pinned", "section", "folder", "updatedAt", "unknown", ""}) {
        event.field = QString::fromLatin1(field); QVERIFY(!isVisibleActivityEvent(event));
    }
    event.type = QStringLiteral("unknown"); event.field = QStringLiteral("title"); QVERIFY(!isVisibleActivityEvent(event));
}

void DropGroupTests::activityContextsAndEventOnlySessionSignals()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString parent = store.createTask(QStringLiteral("活动主任务"));
    QSignalSpy eventChanged(&store, &TaskStore::eventsChanged);
    const QString child = store.createSubtask(QStringLiteral("活动子任务"), parent); QVERIFY(isUuid(child));
    QCOMPARE(eventChanged.count(), 1); QCOMPARE(eventChanged.first().first().toString(), child);
    const auto creation = store.events(child); QCOMPARE(creation.size(), 2);
    bool sawCreated = false, sawParent = false;
    for (const EntityEvent &event : creation) {
        QCOMPARE(event.context.value(QStringLiteral("newParentTitle")).toString(), QStringLiteral("活动主任务"));
        if (event.type == QStringLiteral("created")) sawCreated = true;
        if (event.field == QStringLiteral("parent")) { sawParent = true; QCOMPARE(event.newValue.toString(), parent); }
    }
    QVERIFY(sawCreated && sawParent);
    const QString session = store.beginEditSession(child); QVERIFY(isUuid(session)); eventChanged.clear();
    const auto before = eventIds(store.events(child));
    // 带归因的文本提交：title/body 差异推迟到会话结束统一补记（保存时零事件信号）。
    const Entity base = store.entity(child);
    TextDraft draft; draft.entityId = child; draft.sessionId = session;
    draft.baselineTitle = base.title; draft.baselineBody = base.body;
    draft.proposedTitle = QStringLiteral("会话最终标题"); draft.proposedBody = QStringLiteral("会话全文");
    QCOMPARE(store.commitTextDraft(draft).status, TextCommitStatus::Saved);
    QCOMPARE(eventChanged.count(), 0);
    const qint64 revision = scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(child)).toLongLong();
    QSignalSpy taskChanged(&store, &TaskStore::taskChanged);
    QVERIFY(store.endEditSession(session)); QCOMPARE(eventChanged.count(), 1); QCOMPARE(taskChanged.count(), 0);
    QCOMPARE(scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(child)).toLongLong(), revision);
    const auto sessionEvents = addedEvents(&store, child, before); QCOMPARE(sessionEvents.size(), 2);
    for (const EntityEvent &event : sessionEvents)
        QCOMPARE(event.context.value(QStringLiteral("newParentTitle")).toString(), QStringLiteral("活动主任务"));

    // 新口径回归：会话空闲时的直存立即记账（抑制只作用于带归因的文本提交），
    // 空闲会话结束零补记、零额外信号。
    const QString idle = store.beginEditSession(child); QVERIFY(isUuid(idle));
    eventChanged.clear(); taskChanged.clear();
    const auto beforeIdle = eventIds(store.events(child));
    Entity direct = store.entity(child); direct.title = QStringLiteral("直存标题");
    QVERIFY(store.saveTask(direct));
    QCOMPARE(eventChanged.count(), 1);                          // 直存：事件立即记账
    QCOMPARE(taskChanged.count(), 1);                           // 实体写入照常通知
    const auto directEvents = addedEvents(&store, child, beforeIdle);
    QCOMPARE(directEvents.size(), 1);
    QCOMPARE(directEvents.first().field, QStringLiteral("title"));
    QCOMPARE(directEvents.first().newValue.toString(), QStringLiteral("直存标题"));
    eventChanged.clear(); taskChanged.clear();
    QVERIFY(store.endEditSession(idle));                        // 空闲会话：结束零事件零信号
    QCOMPARE(eventChanged.count(), 0);
    QCOMPARE(taskChanged.count(), 0);
    const QString fileName = QStringLiteral("可读附件名.txt"); const QString path = m_directory->filePath(fileName);
    { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("attachment") > 0); }
    eventChanged.clear(); const auto beforeAttachment = eventIds(store.events(child));
    const Attachment attachment = store.importAttachment(child, path); QVERIFY(isUuid(attachment.id)); QCOMPARE(eventChanged.count(), 1);
    auto added = addedEvents(&store, child, beforeAttachment); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().context.value(QStringLiteral("attachmentName")).toString(), fileName);
    const auto beforeRemoval = eventIds(store.events(child)); eventChanged.clear();
    QVERIFY(store.removeAttachment(child, attachment.id)); QCOMPARE(eventChanged.count(), 1);
    added = addedEvents(&store, child, beforeRemoval); QCOMPARE(added.size(), 1);
    QCOMPARE(added.first().context.value(QStringLiteral("attachmentName")).toString(), fileName);
    const QString list = store.createList(QStringLiteral("活动新清单")); eventChanged.clear();
    const auto beforeMove = eventIds(store.events(child)); QVERIFY(store.moveEntityToList(child, list)); QCOMPARE(eventChanged.count(), 1);
    for (const EntityEvent &event : addedEvents(&store, child, beforeMove)) {
        if (event.field == QStringLiteral("list")) {
            QCOMPARE(event.context.value(QStringLiteral("oldListName")).toString(), QStringLiteral("收集箱"));
            QCOMPARE(event.context.value(QStringLiteral("newListName")).toString(), QStringLiteral("活动新清单"));
        }
    }
    eventChanged.clear(); QVERIFY(store.convertKind(child, EntityKind::Note)); QCOMPARE(eventChanged.count(), 1);
    QCOMPARE(eventChanged.first().first().toString(), child);
}

void DropGroupTests::archiveEventsAreAtomicAndListDeletionNotifies()
{
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString list = store.createList(QStringLiteral("归档清单快照"));
    const QString active = store.createTask(QStringLiteral("活动"), list);
    const QString terminal = store.createTask(QStringLiteral("终态"), list); QVERIFY(store.setCompleted(terminal, true));
    const QString note = store.createNote(QStringLiteral("笔记"), list);
    const QString trashed = store.createTask(QStringLiteral("已回收"), list); QVERIFY(store.trashTask(trashed));
    const QStringList ids{active, terminal, note, trashed};
    QVERIFY(execute(QStringLiteral("CREATE TRIGGER reject_archive_event BEFORE INSERT ON entity_events WHEN NEW.field='listArchived' "
        "BEGIN SELECT RAISE(ABORT,'archive event rejected'); END")));
    const DatabaseState before = databaseState();
    QSignalSpy eventChanged(&store, &TaskStore::eventsChanged); QSignalSpy groupsChanged(&store, &TaskStore::groupsChanged);
    QVERIFY(!store.setListArchived(list, true)); QVERIFY(databaseState() == before);
    QVERIFY(!store.isListArchived(list)); QCOMPARE(eventChanged.count(), 0); QCOMPARE(groupsChanged.count(), 0);
    QVERIFY(execute(QStringLiteral("DROP TRIGGER reject_archive_event")));
    const auto entityRows = rows(QStringLiteral("entities"));
    QHash<QString, QSet<QString>> previous;
    for (const QString &id : ids) previous.insert(id, eventIds(store.events(id)));
    QVERIFY2(store.setListArchived(list, true), qPrintable(store.lastError()));
    QVERIFY(store.isListArchived(list)); QCOMPARE(rows(QStringLiteral("entities")), entityRows);
    QCOMPARE(eventChanged.count(), ids.size()); QCOMPARE(groupsChanged.count(), 1);
    QSet<QString> batches;
    for (const QString &id : ids) {
        const auto added = addedEvents(&store, id, previous.value(id)); QCOMPARE(added.size(), 1);
        const EntityEvent &event = added.first(); QCOMPARE(event.type, QStringLiteral("changed"));
        QCOMPARE(event.field, QStringLiteral("listArchived")); QCOMPARE(event.oldValue.toString(), QStringLiteral("0"));
        QCOMPARE(event.newValue.toString(), QStringLiteral("1"));
        QCOMPARE(event.context.value(QStringLiteral("listName")).toString(), QStringLiteral("归档清单快照")); batches.insert(event.batchId);
    }
    QCOMPARE(batches.size(), 1);
    eventChanged.clear(); const DatabaseState archived = databaseState();
    QVERIFY(store.setListArchived(list, true)); QVERIFY(databaseState() == archived); QCOMPARE(eventChanged.count(), 0);
    QVERIFY(store.setListArchived(list, false)); QCOMPARE(eventChanged.count(), ids.size());
    for (const QString &id : ids) previous.insert(id, eventIds(store.events(id)));
    eventChanged.clear(); QVERIFY(store.deleteList(list)); QCOMPARE(eventChanged.count(), 3);
    for (const QString &id : {active, terminal, note}) {
        const auto added = addedEvents(&store, id, previous.value(id)); QCOMPARE(added.size(), 1);
        QCOMPARE(added.first().type, QStringLiteral("trashed"));
        QCOMPARE(added.first().context.value(QStringLiteral("listName")).toString(), QStringLiteral("归档清单快照"));
    }
    QVERIFY(addedEvents(&store, trashed, previous.value(trashed)).isEmpty());
}

QTEST_GUILESS_MAIN(DropGroupTests)
#include "tst_dropgroups.moc"
