// 五分组 + 类型化 GroupKey + TaskDrop::Intent 的纯规则与模型层测试。
// 仅覆盖本 agent 所有权（domain 分组规则 / ViewSpec 筛选口径 / TaskListModel 展示层）。
// CMake 注册：foreach(test IN ITEMS ... groupmodel)，链接 kylinlist_ui（主模型挂接）。
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/TaskDrop.h"
#include "domain/TaskGrouping.h"
#include "domain/ViewSpec.h"
#include "ui/TaskListModel.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>

using namespace kl;

namespace {

Entity task(const QString &id, const QString &listId = QStringLiteral("l1"))
{
    Entity entity;
    entity.id = id;
    entity.kind = EntityKind::Task;
    entity.title = id;
    entity.listId = listId;
    return entity;
}

} // namespace

class GroupModelTests : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void groupKeyStableIds();
    void groupKeyOrdering();
    void groupModeNames();
    void timeBucketBoundaries();
    void detailScheduleCaptionUsesCalendarWeeks();
    void detailScheduleCaptionKeepsRangeEndpointsTogether();
    void createdBucketBoundaries();
    void groupOfPrecedence();
    void taskDropTargetSemantics();
    void intentValidation();

    void rendererNoLongerFilters();
    void sectionGlobalOrder();
    void showCompletedAdmitsAbandoned();
    void displayGroupInheritance();
    void terminalChildDoesNotFollowActiveParent();
    void crossListChildStaysRoot();
    void sortOrderWithinGroups();
    void mixedPinnedRanksStayStrictlyOrdered();
    void createdGroupingBuckets();
    void priorityGroupingOrder();
    void noneGroupingFlat();
    void listGroupingByListOrder();
    void folderListGrouping();
    void folderArchivedListsExcludedAcrossModes();
    void historyScopesGroupByMode();

    void modelGroupingSwitchKeepsEntities();
    void modelCreatedModeBlocksDrag();
    void modelDragTargetHeaders();
    void modelHistoryScopeBlocksDrag();

private:
    bool open();
    QString dbPath() const { return m_directory->filePath(QStringLiteral("kylinlist.db")); }
    std::unique_ptr<QTemporaryDir> m_directory;
};

void GroupModelTests::init()
{
    Database::instance().close();
    m_directory.reset(new QTemporaryDir);
    QVERIFY(m_directory->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_directory->path().toUtf8());
}

void GroupModelTests::cleanup()
{
    Database::instance().close();
    qunsetenv("KYLINLIST_DATA_DIR");
    m_directory.reset();
}

bool GroupModelTests::open()
{
    QString error;
    const bool ok = Database::instance().openAt(dbPath(), &error);
    if (!ok) qWarning() << error;
    return ok;
}

// ------------------------------------------------------------- GroupKey --

void GroupModelTests::groupKeyStableIds()
{
    const QDate today(2026, 11, 15);
    const QVector<GroupKey> keys = {
        GroupKey::pinned(), GroupKey::flat(), GroupKey::notes(), GroupKey::completed(),
        GroupKey::abandoned(), GroupKey::trash(),
        GroupKey::forList(QStringLiteral("abc")), GroupKey::forPriority(2),
        GroupKey::forTime(TimeBucket::Overdue), GroupKey::forTime(TimeBucket::Today),
        GroupKey::forTime(TimeBucket::Tomorrow), GroupKey::forTime(TimeBucket::Next7Days),
        GroupKey::forTime(TimeBucket::Later), GroupKey::forTime(TimeBucket::Undated),
        GroupKey::forCreated(CreatedBucket::Last7Days), GroupKey::forCreated(CreatedBucket::Last30Days),
        GroupKey::forCreated(CreatedBucket::Earlier), GroupKey::forCreated(CreatedBucket::Unknown),
    };
    for (const GroupKey &key : keys) {
        QVERIFY(key.isValid());
        const QString id = key.stableId();
        QVERIFY(!id.isEmpty());
        QCOMPARE(GroupKey::fromStableId(id), key);
        QCOMPARE(GroupKey::fromStableId(id).title(today), key.title(today));
    }
    QCOMPARE(GroupKey::fromStableId(QStringLiteral("time:bogus")), GroupKey());
    QCOMPARE(GroupKey::fromStableId(QStringLiteral("priority:9")), GroupKey());
    QCOMPARE(GroupKey::fromStableId(QString()), GroupKey());
    QCOMPARE(GroupKey().stableId(), QString());
    QCOMPARE(GroupKey::pinned().title(), QStringLiteral("已置顶"));
    QCOMPARE(GroupKey::forTime(TimeBucket::Undated).title(), QStringLiteral("无日期"));
    QCOMPARE(GroupKey::forTime(TimeBucket::Later).title(), QStringLiteral("稍后"));
    QCOMPARE(GroupKey::forPriority(3).title(), QStringLiteral("高"));
    QCOMPARE(GroupKey::forPriority(0).title(), QStringLiteral("无"));
    QCOMPARE(GroupKey::flat().title(), QString());
    QCOMPARE(GroupKey::forList(QStringLiteral("x")).title(), QString());   // 无清单表时为空
    QVector<TaskList> lists{TaskList()};
    lists.last().id = QStringLiteral("x");
    lists.last().name = QStringLiteral("工作");
    QCOMPARE(GroupKey::forList(QStringLiteral("x")).title(lists), QStringLiteral("工作"));
}

void GroupModelTests::groupKeyOrdering()
{
    QVERIFY(GroupKey::pinned() < GroupKey::forTime(TimeBucket::Overdue));
    QVERIFY(GroupKey::forTime(TimeBucket::Overdue) < GroupKey::forTime(TimeBucket::Today));
    QVERIFY(GroupKey::forTime(TimeBucket::Today) < GroupKey::forTime(TimeBucket::Tomorrow));
    QVERIFY(GroupKey::forTime(TimeBucket::Tomorrow) < GroupKey::forTime(TimeBucket::Next7Days));
    QVERIFY(GroupKey::forTime(TimeBucket::Next7Days) < GroupKey::forTime(TimeBucket::Later));
    QVERIFY(GroupKey::forTime(TimeBucket::Later) < GroupKey::forTime(TimeBucket::Undated));
    QVERIFY(GroupKey::forTime(TimeBucket::Undated) < GroupKey::notes());
    QVERIFY(GroupKey::notes() < GroupKey::completed());
    QVERIFY(GroupKey::completed() < GroupKey::abandoned());
    QVERIFY(GroupKey::abandoned() < GroupKey::trash());
    // 优先级高 → 低。
    QVERIFY(GroupKey::forPriority(3) < GroupKey::forPriority(2));
    QVERIFY(GroupKey::forPriority(1) < GroupKey::forPriority(0));
    QCOMPARE(GroupKey::forPriority(2), GroupKey::forPriority(2));
    QVERIFY(GroupKey::forList(QStringLiteral("a")) < GroupKey::forList(QStringLiteral("b")));
    QVERIFY(GroupKey::pinned().isModeGroup() == false);
    QVERIFY(GroupKey::forTime(TimeBucket::Today).isModeGroup());
    QVERIFY(GroupKey::completed().isTerminal());
    QVERIFY(!GroupKey::notes().isTerminal());
}

void GroupModelTests::groupModeNames()
{
    QCOMPARE(defaultGrouping(ViewScope::Folder), GroupMode::List);
    QCOMPARE(defaultGrouping(ViewScope::AllLists), GroupMode::Time);
    QCOMPARE(defaultGrouping(ViewScope::List), GroupMode::Time);
    QCOMPARE(defaultGrouping(ViewScope::Today), GroupMode::Time);
    const QVector<GroupMode> modes = {GroupMode::List, GroupMode::Time, GroupMode::Created,
                                      GroupMode::Priority, GroupMode::None};
    for (GroupMode mode : modes)
        QCOMPARE(groupModeFromName(groupModeName(mode)), mode);
    QCOMPARE(groupModeFromName(QStringLiteral("junk"), GroupMode::Priority), GroupMode::Priority);
    QVERIFY(groupingAllowsDrag(GroupMode::Time));
    QVERIFY(groupingAllowsDrag(GroupMode::None));
    QVERIFY(!groupingAllowsDrag(GroupMode::Created));
}

void GroupModelTests::timeBucketBoundaries()
{
    const QDate today(2026, 11, 15);
    const auto bucketFor = [&today](int offset) {
        Entity entity = task(QStringLiteral("t"));
        entity.allDayDate = offset == 999 ? QDate() : today.addDays(offset);
        return timeBucketOf(entity, today);
    };
    QCOMPARE(bucketFor(-1), TimeBucket::Overdue);
    QCOMPARE(bucketFor(-30), TimeBucket::Overdue);
    QCOMPARE(bucketFor(0), TimeBucket::Today);
    QCOMPARE(bucketFor(1), TimeBucket::Tomorrow);
    QCOMPARE(bucketFor(2), TimeBucket::Next7Days);
    QCOMPARE(bucketFor(6), TimeBucket::Next7Days);
    QCOMPARE(bucketFor(7), TimeBucket::Later);
    QCOMPARE(bucketFor(60), TimeBucket::Later);
    QCOMPARE(bucketFor(999), TimeBucket::Undated);

    Entity range = task(QStringLiteral("range"));
    range.allDayDate = today.addDays(-1); range.allDayEndDate = today.addDays(1);
    range.hasScheduleRange = true;
    QCOMPARE(timeBucketOf(range, today), TimeBucket::Tomorrow); // 互斥分桶看结束日
    ViewSpec todayScope; todayScope.scope = ViewScope::Today;
    ViewSpec tomorrowScope; tomorrowScope.scope = ViewScope::Tomorrow;
    QVERIFY(contains(range, todayScope, today));    // 智能筛选仍按范围相交
    QVERIFY(contains(range, tomorrowScope, today));
    range.allDayEndDate = today.addDays(5);
    QCOMPARE(timeBucketOf(range, today), TimeBucket::Next7Days);
    range.allDayEndDate = today.addDays(-1);
    QCOMPARE(timeBucketOf(range, today), TimeBucket::Overdue);
}

void GroupModelTests::detailScheduleCaptionUsesCalendarWeeks()
{
    const QDate today(2026, 11, 19); // 周四，离下周日还有十天。
    const QDateTime now = QDateTime(today, QTime(0, 30), appTimeZone()).toUTC();
    QCOMPARE(now.date(), today.addDays(-1));
    Entity entity = task(QStringLiteral("caption"));
    const auto undated = detailScheduleCaption(entity, now);
    QCOMPARE(undated.text, QStringLiteral("设置日期"));
    QVERIFY(!undated.scheduled && !undated.overdue);
    const struct { int offset; const char* text; } cases[] = {
        {-3, "11月16日, 延期3天"}, {-1, "11月18日, 昨天"}, {0, "11月19日, 今天"},
        {1, "11月20日, 明天"}, {2, "11月21日, 后天"}, {3, "11月22日, 周日"},
        {4, "11月23日, 下周一"}, {5, "11月24日, 下周二"}, {10, "11月29日, 下周日"},
        {11, "11月30日, 剩余11天"}, {43, "2027/01/01, 剩余43天"}
    };
    for (const auto &value : cases) {
        entity.allDayDate = today.addDays(value.offset);
        const auto caption = detailScheduleCaption(entity, now);
        QCOMPARE(caption.text, QString::fromUtf8(value.text));
        QVERIFY(caption.scheduled);
        QCOMPARE(caption.overdue, value.offset < 0);
    }
    entity.allDayDate = today.addDays(3);
    QCOMPARE(detailScheduleCaption(entity, now, Qt::Sunday).text, QStringLiteral("11月22日, 下周日"));
    entity.allDayDate = today.addDays(1);
    const auto tomorrowAtMidnight = detailScheduleCaption(entity, now.addDays(1));
    QCOMPARE(tomorrowAtMidnight.text, QStringLiteral("11月20日, 今天"));
    entity.allDayDate = QDate();
    entity.dueAt = QDateTime(today, QTime(0, 0), appTimeZone()).toUTC();
    const auto midnight = detailScheduleCaption(entity, now);
    QCOMPARE(midnight.text, QStringLiteral("11月19日, 00:00, 今天"));
    QVERIFY(midnight.overdue);

    entity.dueAt = QDateTime();
    entity.allDayDate = today.addDays(-3);
    for (TaskStatus status : {TaskStatus::Completed, TaskStatus::Abandoned}) {
        entity.status = status;
        const auto caption = detailScheduleCaption(entity, now);
        QCOMPARE(caption.text, QStringLiteral("11月16日"));
        QVERIFY(!caption.overdue);
    }
    entity.allDayDate = today.addDays(43);
    QCOMPARE(detailScheduleCaption(entity, now).text, QStringLiteral("2027/01/01"));
}

void GroupModelTests::detailScheduleCaptionKeepsRangeEndpointsTogether()
{
    const auto at = [](const QDate &date, int hour, int minute) {
        return QDateTime(date, QTime(hour, minute), appTimeZone()).toUTC();
    };
    const QDate today(2026, 12, 31), tomorrow = today.addDays(1);
    const QDateTime now = at(today, 12, 0);
    Entity entity = task(QStringLiteral("range-caption"));
    entity.hasScheduleRange = true;
    entity.startAt = at(today.addDays(-2), 9, 17);
    entity.dueAt = at(tomorrow, 0, 30);
    const auto crossing = detailScheduleCaption(entity, now);
    QCOMPARE(crossing.text, QStringLiteral("12月29日, 09:17 - 2027/01/01, 00:30, 明天"));
    QVERIFY(!crossing.overdue);

    entity.startAt = at(today, 9, 0);
    entity.dueAt = at(today, 10, 30);
    const auto sameDay = detailScheduleCaption(entity, now);
    QCOMPARE(sameDay.text, QStringLiteral("12月31日, 09:00 - 10:30, 今天"));
    QVERIFY(sameDay.overdue);

    entity.startAt = entity.dueAt = QDateTime();
    entity.allDayDate = today.addDays(-2);
    entity.allDayEndDate = today.addDays(-1);
    const auto allDay = detailScheduleCaption(entity, now);
    QCOMPARE(allDay.text, QStringLiteral("12月29日 - 12月30日, 昨天"));
    QVERIFY(allDay.overdue);
    entity.allDayEndDate = entity.allDayDate;
    QCOMPARE(detailScheduleCaption(entity, now).text, QStringLiteral("12月29日, 延期2天"));
}

void GroupModelTests::createdBucketBoundaries()
{
    const QDate today(2026, 11, 15);
    const auto bucketFor = [&today](int ageDays) {
        Entity entity = task(QStringLiteral("t"));
        if (ageDays >= 0) entity.createdAt = QDateTime(today.addDays(-ageDays), QTime(8, 0), Qt::UTC);
        return createdBucketOf(entity, today);
    };
    QCOMPARE(bucketFor(0), CreatedBucket::Last7Days);
    QCOMPARE(bucketFor(6), CreatedBucket::Last7Days);
    QCOMPARE(bucketFor(7), CreatedBucket::Last30Days);
    QCOMPARE(bucketFor(29), CreatedBucket::Last30Days);
    QCOMPARE(bucketFor(30), CreatedBucket::Earlier);
    QCOMPARE(bucketFor(400), CreatedBucket::Earlier);
    QCOMPARE(bucketFor(1000), CreatedBucket::Earlier);
    QCOMPARE(bucketFor(-3), CreatedBucket::Unknown);   // 未来兜底
    Entity noCreated = task(QStringLiteral("t"));
    QCOMPARE(createdBucketOf(noCreated, today), CreatedBucket::Unknown);
}

void GroupModelTests::groupOfPrecedence()
{
    const QDate today(2026, 11, 15);
    Entity deleted = task(QStringLiteral("d"));
    deleted.deleted = true;
    QCOMPARE(groupOf(deleted, GroupMode::Time, today), GroupKey::trash());

    Entity done = task(QStringLiteral("c"));
    done.status = TaskStatus::Completed;
    done.pinnedAt = QDateTime::currentDateTimeUtc();
    QCOMPARE(groupOf(done, GroupMode::Time, today), GroupKey::completed());

    Entity abandoned = task(QStringLiteral("a"));
    abandoned.status = TaskStatus::Abandoned;
    QCOMPARE(groupOf(abandoned, GroupMode::Priority, today), GroupKey::abandoned());

    Entity pinned = task(QStringLiteral("p"));
    pinned.pinnedAt = QDateTime::currentDateTimeUtc();
    pinned.allDayDate = today;
    QCOMPARE(groupOf(pinned, GroupMode::Time, today), GroupKey::pinned());

    Entity pinnedNote = task(QStringLiteral("pn"));
    pinnedNote.kind = EntityKind::Note;
    pinnedNote.pinnedAt = QDateTime::currentDateTimeUtc();
    QCOMPARE(groupOf(pinnedNote, GroupMode::Time, today), GroupKey::pinned());
    Entity plainNote = pinnedNote;
    plainNote.pinnedAt = QDateTime();
    QCOMPARE(groupOf(plainNote, GroupMode::Priority, today), GroupKey::notes());

    Entity low = task(QStringLiteral("l"));
    low.priority = 5;   // 越界收敛到 3
    QCOMPARE(groupOf(low, GroupMode::Priority, today), GroupKey::forPriority(3));

    Entity undated = task(QStringLiteral("u"));
    QCOMPARE(groupOf(undated, GroupMode::Time, today), GroupKey::forTime(TimeBucket::Undated));
    QCOMPARE(groupOf(undated, GroupMode::None, today), GroupKey::flat());
    QCOMPARE(groupOf(undated, GroupMode::List, today), GroupKey::forList(QStringLiteral("l1")));
}

// ------------------------------------------------------------- TaskDrop --

void GroupModelTests::taskDropTargetSemantics()
{
    const QDate today(2026, 11, 15);
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Overdue), today), QDate(2026, 11, 14));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Today), today), today);
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Tomorrow), today), QDate(2026, 11, 16));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Next7Days), today), QDate(2026, 11, 17));
    QCOMPARE(TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Later), today), QDate(2026, 11, 22));
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Undated), today).isValid());
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::pinned(), today).isValid());
    QVERIFY(!TaskDrop::dateForGroup(GroupKey::forTime(TimeBucket::Today), QDate()).isValid());
    QCOMPARE(TaskDrop::priorityForGroup(GroupKey::forPriority(2)), 2);
    QCOMPARE(TaskDrop::priorityForGroup(GroupKey::pinned()), -1);
    QCOMPARE(TaskDrop::priorityForGroup(GroupKey::forTime(TimeBucket::Today)), -1);
    QCOMPARE(TaskDrop::listForGroup(GroupKey::forList(QStringLiteral("L"))), QStringLiteral("L"));
    QVERIFY(TaskDrop::listForGroup(GroupKey::notes()).isEmpty());
}

void GroupModelTests::intentValidation()
{
    const QStringList ordered = {QStringLiteral("a"), QStringLiteral("s"), QStringLiteral("b")};
    TaskDrop::Intent base;
    base.sourceId = QStringLiteral("s");
    base.mode = GroupMode::Time;
    base.sourceGroup = GroupKey::forTime(TimeBucket::Today);
    base.targetGroup = GroupKey::forTime(TimeBucket::Today);
    base.placement = TaskDrop::Placement::After;
    base.anchorId = QStringLiteral("a");
    base.orderedIds = ordered;
    QVERIFY(base.isValid());

    TaskDrop::Intent bad = base;
    bad.anchorId.clear();
    QVERIFY(!bad.isValid());   // Before/After 必须有锚
    bad = base;
    bad.anchorId = base.sourceId;
    QVERIFY(!bad.isValid());   // 不能以自身为锚
    bad = base;
    bad.placement = TaskDrop::Placement::AsChild;
    bad.anchorId = QStringLiteral("a");
    bad.targetParentId = QStringLiteral("a");
    QVERIFY(bad.isValid());
    bad.targetParentId.clear();
    QVERIFY(!bad.isValid());
    bad = base;
    bad.targetParentId = QStringLiteral("a");   // 非 AsChild 不带 targetParentId
    QVERIFY(!bad.isValid());
    bad = base;
    bad.mode = GroupMode::Created;
    QVERIFY(!bad.isValid());   // Created 禁拖
    bad = base;
    bad.orderedIds.clear();
    bad.orderedIds << QStringLiteral("s") << QStringLiteral("s");
    QVERIFY(!bad.isValid());   // 重复 id
    bad = base;
    bad.orderedIds.removeOne(QStringLiteral("s"));
    QVERIFY(!bad.isValid());   // orderedIds 必含 source
    bad = base;
    bad.targetGroup = GroupKey::completed();
    QVERIFY(!bad.isValid());   // 终态不是拖放目标
    bad = base;
    bad.targetGroup = GroupKey::notes();
    QVERIFY(!bad.isValid());   // 任务不能隐式转笔记
    bad = base;
    bad.sourceGroup = GroupKey::notes();
    bad.targetGroup = GroupKey::notes();
    bad.placement = TaskDrop::Placement::IntoGroup;
    bad.anchorId.clear();
    QVERIFY(bad.isValid());    // 笔记同组排序合法
}

// --------------------------------------------------- 分组构建（纯规则） --

void GroupModelTests::rendererNoLongerFilters()
{
    // 回归：renderer==Time 且非时间作用域不再把普通清单筛空（分组与筛选正交）。
    const QDate today(2026, 11, 15);
    Entity t = task(QStringLiteral("t"));
    t.allDayDate = today;
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.renderer = ViewRenderer::Time;
    spec.grouping = GroupMode::Time;
    QVERIFY(contains(t, spec, today));
    const Grouping grouping = groupEntities({t}, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 1);
    QCOMPARE(grouping.sections.first().group, GroupKey::forTime(TimeBucket::Today));
}

void GroupModelTests::sectionGlobalOrder()
{
    const QDate today(2026, 11, 15);
    Entity pinned = task(QStringLiteral("p"));
    pinned.pinnedAt = QDateTime::currentDateTimeUtc();
    Entity dated = task(QStringLiteral("d"));
    dated.allDayDate = today;
    Entity note = task(QStringLiteral("n"));
    note.kind = EntityKind::Note;
    Entity done = task(QStringLiteral("c"));
    done.status = TaskStatus::Completed;
    done.completedAt = QDateTime(today, QTime(9, 0), Qt::UTC);
    Entity gone = task(QStringLiteral("a"));
    gone.status = TaskStatus::Abandoned;
    gone.abandonedAt = QDateTime(today, QTime(9, 0), Qt::UTC);

    ViewSpec spec;
    spec.scope = ViewScope::AllLists;
    spec.grouping = GroupMode::Time;
    spec.showCompleted = true;
    const Grouping grouping = groupEntities({gone, note, dated, done, pinned}, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 5);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::pinned());
    QCOMPARE(grouping.sections.at(1).group, GroupKey::forTime(TimeBucket::Today));
    QCOMPARE(grouping.sections.at(2).group, GroupKey::notes());
    QCOMPARE(grouping.sections.at(3).group, GroupKey::completed());
    QCOMPARE(grouping.sections.at(4).group, GroupKey::abandoned());

    spec.showCompleted = false;
    const Grouping hidden = groupEntities({gone, note, dated, done, pinned}, {}, spec, today);
    QCOMPARE(hidden.sections.size(), 3);   // 终态两组被收起
}

void GroupModelTests::showCompletedAdmitsAbandoned()
{
    const QDate today(2026, 11, 15);
    Entity done = task(QStringLiteral("c"));
    done.status = TaskStatus::Completed;
    Entity gone = task(QStringLiteral("a"));
    gone.status = TaskStatus::Abandoned;
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.showCompleted = true;
    QVERIFY(contains(done, spec, today));
    QVERIFY(contains(gone, spec, today));   // showCompleted 同时放行 Abandoned
    spec.showCompleted = false;
    QVERIFY(!contains(done, spec, today));
    QVERIFY(!contains(gone, spec, today));
}

void GroupModelTests::displayGroupInheritance()
{
    const QDate today(2026, 11, 15);
    Entity parent = task(QStringLiteral("p"));
    parent.allDayDate = today.addDays(3);   // Next7Days
    Entity child = task(QStringLiteral("c"));
    child.parentId = parent.id;
    child.allDayDate = today;               // 自身属性是 Today
    Entity grandchild = task(QStringLiteral("g"));
    grandchild.parentId = child.id;
    grandchild.allDayDate = today.addDays(-5);

    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::Time;
    const Grouping grouping = groupEntities({grandchild, child, parent}, {}, spec, today);
    // 同状态同清单：子级全部跟随顶端父级的显示组。
    QCOMPARE(grouping.sections.size(), 1);
    QCOMPARE(grouping.sections.first().group, GroupKey::forTime(TimeBucket::Next7Days));
    QCOMPARE(grouping.sections.first().items.size(), 1);
    QCOMPARE(grouping.sections.first().items.first().id, parent.id);
    QCOMPARE(grouping.children.value(parent.id).first().id, child.id);
    QCOMPARE(grouping.children.value(child.id).first().id, grandchild.id);

    // 置顶父项同样把子项带进置顶组。
    parent.pinnedAt = QDateTime::currentDateTimeUtc();
    const Grouping pinnedGrouping = groupEntities({grandchild, child, parent}, {}, spec, today);
    QCOMPARE(pinnedGrouping.sections.size(), 1);
    QCOMPARE(pinnedGrouping.sections.first().group, GroupKey::pinned());
}

void GroupModelTests::terminalChildDoesNotFollowActiveParent()
{
    const QDate today(2026, 11, 15);
    Entity parent = task(QStringLiteral("p"));
    parent.allDayDate = today;
    Entity child = task(QStringLiteral("c"));
    child.parentId = parent.id;
    child.allDayDate = today;
    child.status = TaskStatus::Completed;
    child.completedAt = QDateTime(today, QTime(9, 0), Qt::UTC);

    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::Time;
    spec.showCompleted = true;
    const Grouping grouping = groupEntities({parent, child}, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 2);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::forTime(TimeBucket::Today));
    QCOMPARE(grouping.sections.at(0).items.first().id, parent.id);
    QCOMPARE(grouping.sections.at(1).group, GroupKey::completed());
    QCOMPARE(grouping.sections.at(1).items.first().id, child.id);   // 终态子级不跟随 active 父组
    QVERIFY(!grouping.children.contains(parent.id));
}

void GroupModelTests::crossListChildStaysRoot()
{
    const QDate today(2026, 11, 15);
    Entity parent = task(QStringLiteral("p"), QStringLiteral("l2"));
    parent.allDayDate = today;
    Entity child = task(QStringLiteral("c"));
    child.parentId = parent.id;
    child.allDayDate = today.addDays(9);
    ViewSpec spec;
    spec.scope = ViewScope::AllLists;
    spec.grouping = GroupMode::Time;
    const Grouping grouping = groupEntities({parent, child}, {}, spec, today);
    // 跨清单父子不继承：各自成根（父 Today，子 Later）。
    QCOMPARE(grouping.sections.size(), 2);
    QVERIFY(!grouping.children.contains(parent.id));
}

void GroupModelTests::sortOrderWithinGroups()
{
    const QDate today(2026, 11, 15);
    // 三个都在 next7days 桶（today+2..today+6）。
    Entity a = task(QStringLiteral("a"));
    a.allDayDate = today.addDays(2);
    a.sortOrder = 5;
    Entity b = task(QStringLiteral("b"));
    b.allDayDate = today.addDays(3);
    b.sortOrder = 5;
    Entity c = task(QStringLiteral("c"));
    c.allDayDate = today.addDays(5);
    c.sortOrder = 1;   // sortOrder 最小者排最前，与日期无关
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::Time;
    const Grouping grouping = groupEntities({a, b, c}, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 1);
    QCOMPARE(grouping.sections.first().items.size(), 3);
    QStringList ids;
    for (const Entity &entity : grouping.sections.first().items) ids.append(entity.id);
    QCOMPARE(ids, QStringList() << QStringLiteral("c") << QStringLiteral("a") << QStringLiteral("b"));
}

void GroupModelTests::mixedPinnedRanksStayStrictlyOrdered()
{
    const QDate today(2026, 11, 15);
    Entity first = task(QStringLiteral("first-task")); first.sortOrder = 0; first.allDayDate = today;
    Entity note = task(QStringLiteral("middle-note")); note.kind = EntityKind::Note; note.sortOrder = 1;
    Entity last = task(QStringLiteral("last-task")); last.sortOrder = 2; last.allDayDate = today.addDays(-1);
    Entity tiedNote = task(QStringLiteral("tied-note")); tiedNote.kind = EntityKind::Note; tiedNote.sortOrder = 2;
    QVector<Entity> pinned{last, tiedNote, first, note};
    for (Entity &entity : pinned) {
        entity.pinnedAt = QDateTime(today, QTime(9, 0), Qt::UTC);
        entity.createdAt = QDateTime(today, QTime(8, 0), Qt::UTC);
    }
    ViewSpec spec;
    const Grouping grouping = groupEntities(pinned, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 1);
    QCOMPARE(grouping.sections.first().group, GroupKey::pinned());
    QStringList ids;
    for (const Entity &entity : grouping.sections.first().items) ids.append(entity.id);
    QCOMPARE(ids, (QStringList{first.id, note.id, last.id, tiedNote.id}));
    // 混合类别仍必须满足严格弱序，不能用仅针对某一对的 rank 特判制造比较环。
    Entity completed = task(QStringLiteral("completed")); completed.status = TaskStatus::Completed;
    completed.completedAt = QDateTime(today, QTime(10, 0), Qt::UTC);
    completed.createdAt = completed.completedAt; completed.sortOrder = 0;
    pinned.append(completed);
    const auto less = [&](const Entity &a, const Entity &b) { return groupLessThan(a, b, GroupMode::Time, today); };
    for (const Entity &a : pinned) {
        QVERIFY(!less(a, a));
        for (const Entity &b : pinned) {
            QVERIFY(!(less(a, b) && less(b, a)));
            for (const Entity &c : pinned) if (less(a, b) && less(b, c)) QVERIFY(less(a, c));
        }
    }
}

void GroupModelTests::createdGroupingBuckets()
{
    const QDate today(2026, 11, 15);
    QVector<Entity> entities;
    for (int i = 0; i < 6; ++i) {
        Entity entity = task(QStringLiteral("e%1").arg(i));
        entity.createdAt = QDateTime(today.addDays(-i * 10), QTime(8, 0), Qt::UTC);
        entities.append(entity);
    }
    Entity future = task(QStringLiteral("future"));
    future.createdAt = QDateTime(today.addDays(3), QTime(8, 0), Qt::UTC);
    entities.append(future);
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::Created;
    const Grouping grouping = groupEntities(entities, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 4);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::forCreated(CreatedBucket::Last7Days));
    QCOMPARE(grouping.sections.at(0).items.size(), 1);   // 0 天前
    QCOMPARE(grouping.sections.at(1).group, GroupKey::forCreated(CreatedBucket::Last30Days));
    QCOMPARE(grouping.sections.at(1).items.size(), 2);   // 10/20 天前
    QCOMPARE(grouping.sections.at(2).group, GroupKey::forCreated(CreatedBucket::Earlier));
    QCOMPARE(grouping.sections.at(2).items.size(), 3);   // 30/40/50 天前
    QCOMPARE(grouping.sections.at(3).group, GroupKey::forCreated(CreatedBucket::Unknown));
    QCOMPARE(grouping.sections.at(3).items.size(), 1);   // 未来兜底
}

void GroupModelTests::priorityGroupingOrder()
{
    const QDate today(2026, 11, 15);
    QVector<Entity> entities;
    const int priorities[] = {0, 3, 1, 2};
    for (int i = 0; i < 4; ++i) {
        Entity entity = task(QStringLiteral("p%1").arg(priorities[i]));
        entity.priority = priorities[i];
        entities.append(entity);
    }
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::Priority;
    const Grouping grouping = groupEntities(entities, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 4);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::forPriority(3));
    QCOMPARE(grouping.sections.at(1).group, GroupKey::forPriority(2));
    QCOMPARE(grouping.sections.at(2).group, GroupKey::forPriority(1));
    QCOMPARE(grouping.sections.at(3).group, GroupKey::forPriority(0));
}

void GroupModelTests::noneGroupingFlat()
{
    const QDate today(2026, 11, 15);
    Entity a = task(QStringLiteral("a"));
    Entity b = task(QStringLiteral("b"));
    b.allDayDate = today;
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = QStringLiteral("l1");
    spec.grouping = GroupMode::None;
    const Grouping grouping = groupEntities({a, b}, {}, spec, today);
    QCOMPARE(grouping.sections.size(), 1);
    QCOMPARE(grouping.sections.first().group, GroupKey::flat());
    QVERIFY(grouping.sections.first().title.isEmpty());   // 无标题但可排序
}

void GroupModelTests::listGroupingByListOrder()
{
    const QDate today(2026, 11, 15);
    TaskList first; first.id = QStringLiteral("la"); first.name = QStringLiteral("A"); first.sortOrder = 1;
    TaskList second; second.id = QStringLiteral("lb"); second.name = QStringLiteral("B"); second.sortOrder = 0;
    Entity inA = task(QStringLiteral("a"), QStringLiteral("la"));
    Entity inB = task(QStringLiteral("b"), QStringLiteral("lb"));
    ViewSpec spec;
    spec.scope = ViewScope::AllLists;
    spec.grouping = GroupMode::List;
    const Grouping grouping = groupEntities({inA, inB}, {first, second}, spec, today);
    QCOMPARE(grouping.sections.size(), 2);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::forList(QStringLiteral("lb")));
    QCOMPARE(grouping.sections.at(0).title, QStringLiteral("B"));
    QCOMPARE(grouping.sections.at(1).group, GroupKey::forList(QStringLiteral("la")));
}

void GroupModelTests::folderListGrouping()
{
    const QDate today(2026, 11, 15);
    TaskList inside; inside.id = QStringLiteral("in"); inside.folderId = QStringLiteral("f"); inside.sortOrder = 0;
    TaskList outside; outside.id = QStringLiteral("out"); outside.sortOrder = 0;
    Entity pinnedOutside = task(QStringLiteral("po"), QStringLiteral("out"));
    pinnedOutside.pinnedAt = QDateTime::currentDateTimeUtc();
    Entity pinnedInside = task(QStringLiteral("pi"), QStringLiteral("in"));
    pinnedInside.pinnedAt = QDateTime::currentDateTimeUtc();
    Entity member = task(QStringLiteral("m"), QStringLiteral("in"));
    ViewSpec spec;
    spec.scope = ViewScope::Folder;
    spec.folderId = QStringLiteral("f");
    spec.grouping = GroupMode::List;   // 文件夹默认 List 分组
    const Grouping grouping = groupEntities({pinnedOutside, pinnedInside, member}, {inside, outside}, spec, today);
    QCOMPARE(grouping.sections.size(), 2);
    QCOMPARE(grouping.sections.at(0).group, GroupKey::pinned());
    QCOMPARE(grouping.sections.at(0).items.size(), 1);   // 不带入文件夹以外的置顶任务
    QCOMPARE(grouping.sections.at(0).items.first().id, QStringLiteral("pi"));
    QCOMPARE(grouping.sections.at(1).group, GroupKey::forList(QStringLiteral("in")));
    QCOMPARE(grouping.sections.at(1).items.first().id, QStringLiteral("m"));
}

void GroupModelTests::folderArchivedListsExcludedAcrossModes()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection()); QVERIFY(store.load());
    const QString folder = store.createFolder(QStringLiteral("活动文件夹"));
    const QString liveList = store.createList(QStringLiteral("活动清单"), folder);
    const QString archivedList = store.createList(QStringLiteral("待归档清单"), folder);
    const QString liveTask = store.createTask(QStringLiteral("仍可见任务"), liveList);
    const QString liveNote = store.createNote(QStringLiteral("仍可见笔记"), liveList);
    const QString active = store.createTask(QStringLiteral("归档活动任务"), archivedList);
    const QString note = store.createNote(QStringLiteral("归档置顶笔记"), archivedList);
    const QString completed = store.createTask(QStringLiteral("归档完成任务"), archivedList);
    const QString abandoned = store.createTask(QStringLiteral("归档放弃任务"), archivedList);
    const QString trashed = store.createTask(QStringLiteral("归档回收站任务"), archivedList);
    const QString outside = store.createTask(QStringLiteral("文件夹外任务"));
    QVERIFY(store.setPinnedAt(note, appNow())); QVERIFY(store.setCompleted(completed, true));
    QVERIFY(store.abandonTask(abandoned)); QVERIFY(store.trashTask(trashed));
    QVERIFY(store.setListArchived(archivedList, true));
    const QStringList excluded{active, note, completed, abandoned, trashed, outside};
    TaskListModel model(&store);
    ViewSpec spec; spec.scope = ViewScope::Folder; spec.folderId = folder; spec.showCompleted = true;
    const QVector<GroupMode> modes{GroupMode::List, GroupMode::Time, GroupMode::Created,
                                    GroupMode::Priority, GroupMode::None};
    for (GroupMode mode : modes) {
        spec.grouping = mode; model.setFilter(spec);
        QCOMPARE(model.entityCount(), 2);
        QVERIFY(model.indexForTask(liveTask).isValid()); QVERIFY(model.indexForTask(liveNote).isValid());
        for (const QString &id : excluded) QVERIFY(!model.indexForTask(id).isValid());
        // 兼容包装的缺省调用也必须从清单元数据排除归档成员，而不是被 leftovers 加回。
        QSet<QString> ids;
        for (const ViewSection &section : buildFolderSections(store.entities(), store.lists(), spec, toAppTime(appNow()).date()))
            for (const Entity &entity : section.items) ids.insert(entity.id);
        QCOMPARE(ids, (QSet<QString>{liveTask, liveNote}));
        for (const QString &id : {active, note, completed, abandoned})
            QVERIFY(!contains(store.entity(id), spec, toAppTime(appNow()).date(), store.archivedListIds(),
                              folderListIds(store.lists(), folder)));
    }
    // 只修活动聚合：直接归档清单与历史作用域仍保留原有可见性。
    spec.scope = ViewScope::List; spec.listId = archivedList; spec.grouping = GroupMode::Time;
    model.setFilter(spec); QCOMPARE(model.entityCount(), 4);
    for (const QString &id : {active, note, completed, abandoned}) QVERIFY(model.indexForTask(id).isValid());
    QVERIFY(!model.indexForTask(trashed).isValid());
    spec.scope = ViewScope::Completed; model.setFilter(spec);
    QCOMPARE(model.entityCount(), 1); QVERIFY(model.indexForTask(completed).isValid());
    spec.scope = ViewScope::Abandoned; model.setFilter(spec);
    QCOMPARE(model.entityCount(), 1); QVERIFY(model.indexForTask(abandoned).isValid());
    spec.scope = ViewScope::Trash; model.setFilter(spec);
    QCOMPARE(model.entityCount(), 1); QVERIFY(model.indexForTask(trashed).isValid());
}

void GroupModelTests::historyScopesGroupByMode()
{
    const QDate today(2026, 11, 15);
    QCOMPARE(viewClassOf(ViewScope::Completed), ViewClass::History);
    QCOMPARE(viewClassOf(ViewScope::Abandoned), ViewClass::History);
    QCOMPARE(viewClassOf(ViewScope::Trash), ViewClass::History);
    QCOMPARE(viewClassOf(ViewScope::AllLists), ViewClass::Active);
    QCOMPARE(viewClassOf(ViewScope::List), ViewClass::Active);
    QCOMPARE(viewClassOf(ViewScope::Today), ViewClass::Active);
    QCOMPARE(viewClassOf(ViewScope::Folder), ViewClass::Active);

    Entity high = task(QStringLiteral("high"));
    high.status = TaskStatus::Completed;
    high.priority = 3;
    high.completedAt = QDateTime(today, QTime(9, 0), Qt::UTC);
    Entity low = task(QStringLiteral("low"));
    low.status = TaskStatus::Completed;
    low.priority = 1;
    low.completedAt = QDateTime(today.addDays(-1), QTime(9, 0), Qt::UTC);
    Entity dated = task(QStringLiteral("dated"));
    dated.status = TaskStatus::Completed;
    dated.allDayDate = today;
    dated.completedAt = QDateTime(today, QTime(8, 0), Qt::UTC);
    const QVector<Entity> pool = {high, low, dated};

    ViewSpec spec;
    spec.scope = ViewScope::Completed;

    // Priority：结果集内按优先级分节，无 completed/pinned/notes 页脚。
    spec.grouping = GroupMode::Priority;
    const Grouping byPriority = groupEntities(pool, {}, spec, today);
    QCOMPARE(byPriority.sections.size(), 3);
    QCOMPARE(byPriority.sections.at(0).group, GroupKey::forPriority(3));
    QCOMPARE(byPriority.sections.at(1).group, GroupKey::forPriority(1));
    QCOMPARE(byPriority.sections.at(2).group, GroupKey::forPriority(0));   // dated 默认无优先级

    // Time：仍按实体日期分桶，桶序照旧（today 在 undated 前）。
    spec.grouping = GroupMode::Time;
    const Grouping byTime = groupEntities(pool, {}, spec, today);
    QStringList timeKeys;
    for (const GroupedSection &section : byTime.sections) timeKeys.append(section.group.stableId());
    QCOMPARE(timeKeys, QStringList() << QStringLiteral("time:today") << QStringLiteral("time:undated"));

    // List：组内终态时间新→旧排序保持。
    spec.grouping = GroupMode::List;
    const Grouping byList = groupEntities(pool, {}, spec, today);
    QCOMPARE(byList.sections.size(), 1);
    QCOMPARE(byList.sections.first().group, GroupKey::forList(QStringLiteral("l1")));
    QCOMPARE(byList.sections.first().items.first().id, QStringLiteral("high"));   // completedAt 最新

    // None：单一无标题平铺组。
    spec.grouping = GroupMode::None;
    const Grouping flat = groupEntities(pool, {}, spec, today);
    QCOMPARE(flat.sections.size(), 1);
    QCOMPARE(flat.sections.first().group, GroupKey::flat());
    QVERIFY(flat.sections.first().title.isEmpty());

    // Trash + List 分组包含归档清单组。
    TaskList live; live.id = QStringLiteral("live"); live.sortOrder = 1;
    TaskList archived; archived.id = QStringLiteral("old"); archived.sortOrder = 0;
    archived.archivedAtUtcMs = 1;
    Entity inLive = task(QStringLiteral("in-live"), QStringLiteral("live"));
    inLive.deleted = true;
    Entity inOld = task(QStringLiteral("in-old"), QStringLiteral("old"));
    inOld.deleted = true;
    ViewSpec trashSpec;
    trashSpec.scope = ViewScope::Trash;
    trashSpec.grouping = GroupMode::List;
    const Grouping trashGrouping = groupEntities({inLive, inOld}, {live, archived}, trashSpec, today,
                                                 QSet<QString>() << QStringLiteral("old"));
    QCOMPARE(trashGrouping.sections.size(), 2);
    QCOMPARE(trashGrouping.sections.at(0).group, GroupKey::forList(QStringLiteral("old")));
    QCOMPARE(trashGrouping.sections.at(1).group, GroupKey::forList(QStringLiteral("live")));

    // 同状态同清单父子继承在历史视角仍然生效（completed 子级跟随 completed 父级）。
    Entity parent = task(QStringLiteral("hp"));
    parent.status = TaskStatus::Completed;
    parent.priority = 2;
    parent.completedAt = QDateTime(today, QTime(9, 0), Qt::UTC);
    Entity child = task(QStringLiteral("hc"));
    child.parentId = parent.id;
    child.status = TaskStatus::Completed;
    child.priority = 3;
    child.completedAt = QDateTime(today, QTime(10, 0), Qt::UTC);
    spec.grouping = GroupMode::Priority;
    const Grouping nested = groupEntities({parent, child}, {}, spec, today);
    QCOMPARE(nested.sections.size(), 1);
    QCOMPARE(nested.sections.first().group, GroupKey::forPriority(2));   // 子级继承父级显示组
    QCOMPARE(nested.children.value(parent.id).size(), 1);

    // 模式切换不改 ID 集合（历史视角同样保持不变量）。
    QSet<QString> reference;
    for (const Entity &entity : pool) reference.insert(entity.id);
    reference.insert(parent.id);
    reference.insert(child.id);
    const QVector<Entity> full = {high, low, dated, parent, child};
    const QVector<GroupMode> modes = {GroupMode::List, GroupMode::Time, GroupMode::Created,
                                      GroupMode::Priority, GroupMode::None};
    for (GroupMode mode : modes) {
        spec.grouping = mode;
        const Grouping grouping = groupEntities(full, {}, spec, today);
        QSet<QString> ids;
        std::function<void(const Entity &)> collect = [&](const Entity &entity) {
            ids.insert(entity.id);
            for (const Entity &descendant : grouping.children.value(entity.id)) collect(descendant);
        };
        for (const GroupedSection &section : grouping.sections)
            for (const Entity &entity : section.items) collect(entity);
        QCOMPARE(ids, reference);
    }
}

// ------------------------------------------------------------ 模型层 --

void GroupModelTests::modelGroupingSwitchKeepsEntities()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    const QString listId = store.inboxListId();
    const QString a = store.createTask(QStringLiteral("甲"), listId);
    const QString b = store.createTask(QStringLiteral("乙"), listId);
    Entity pinnedDraft = store.entity(store.createTask(QStringLiteral("丙"), listId));
    pinnedDraft.pinnedAt = appNow();
    store.saveTask(pinnedDraft);
    Entity datedDraft = store.entity(b);
    datedDraft.allDayDate = toAppTime(appNow()).date();
    store.saveTask(datedDraft);
    QVERIFY(!a.isEmpty());

    TaskListModel model(&store);
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = listId;
    const QVector<GroupMode> modes = {GroupMode::List, GroupMode::Time, GroupMode::Created,
                                      GroupMode::Priority, GroupMode::None};
    const int expected = model.entityCount();
    QCOMPARE(expected, 3);
    for (GroupMode mode : modes) {
        spec.grouping = mode;
        model.setFilter(spec);
        QCOMPARE(model.entityCount(), expected);   // 切换分组不改实体集合
        QCOMPARE(model.grouping(), mode);
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, 0);
            if (index.data(TaskListModel::HeaderRole).toBool()) continue;
            const GroupKey key = index.data(TaskListModel::GroupKeyRole).value<GroupKey>();
            QVERIFY(key.isValid());
            QCOMPARE(index.data(TaskListModel::SectionKeyRole).toString(), key.stableId());
            QCOMPARE(model.groupKeyAt(index), key);
        }
    }
    // 切换分组是纯展示：不写实体、不改排序值。
    QCOMPARE(store.entities().size(), 3);
    QHash<QString, qint64> orders;
    for (const Entity &entity : store.entities()) orders.insert(entity.id, entity.sortOrder);
    spec.grouping = GroupMode::Time;
    model.setFilter(spec);
    for (const Entity &entity : store.entities()) QCOMPARE(entity.sortOrder, orders.value(entity.id));
}

void GroupModelTests::modelCreatedModeBlocksDrag()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    const QString listId = store.inboxListId();
    const QString active = store.createTask(QStringLiteral("进行中"), listId);
    const QString done = store.createTask(QStringLiteral("已完成"), listId);
    store.setCompleted(done, true);

    TaskListModel model(&store);
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = listId;
    spec.grouping = GroupMode::Time;
    spec.showCompleted = true;   // 终态行可见才能校验其禁拖反馈
    model.setFilter(spec);
    const QModelIndex activeIndex = model.indexForTask(active);
    const QModelIndex doneIndex = model.indexForTask(done);
    QVERIFY(activeIndex.isValid());
    QVERIFY(doneIndex.isValid());
    QVERIFY(activeIndex.flags() & Qt::ItemIsDragEnabled);
    QVERIFY(model.dragBlockReason(activeIndex).isEmpty());
    QVERIFY(!(doneIndex.flags() & Qt::ItemIsDragEnabled));
    QCOMPARE(model.dragBlockReason(doneIndex), QStringLiteral("已完成的任务不能拖动"));

    spec.grouping = GroupMode::Created;
    model.setFilter(spec);
    QVERIFY(!(model.indexForTask(active).flags() & Qt::ItemIsDragEnabled));
    QVERIFY(!model.dragBlockReason(model.indexForTask(active)).isEmpty());
    QVERIFY(model.mimeData(QModelIndexList{model.indexForTask(active)}) == nullptr);
}

void GroupModelTests::modelDragTargetHeaders()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    const QString listId = store.inboxListId();
    const QString source = store.createTask(QStringLiteral("源"), listId);
    Entity dated = store.entity(source);
    dated.allDayDate = toAppTime(appNow()).date();
    store.saveTask(dated);

    TaskListModel model(&store);
    ViewSpec spec;
    spec.scope = ViewScope::List;
    spec.listId = listId;
    spec.grouping = GroupMode::Time;
    model.setFilter(spec);
    const int baseRows = model.rowCount();
    QStringList keys;
    for (int row = 0; row < model.rowCount(); ++row)
        keys.append(model.index(row, 0).data(TaskListModel::SectionKeyRole).toString());
    QVERIFY(keys.contains(QStringLiteral("time:today")));

    model.setDragActive(true, source);
    QStringList dragKeys;
    for (int row = 0; row < model.rowCount(); ++row)
        dragKeys.append(model.index(row, 0).data(TaskListModel::SectionKeyRole).toString());
    // 拖动时六个时间桶全部可见（空组也有投放目标），且不丢原有组。
    for (const char *key : {"time:overdue", "time:today", "time:tomorrow", "time:next7days",
                            "time:later", "time:undated", "pinned"})
        QVERIFY2(dragKeys.contains(QString::fromUtf8(key)), key);
    QVERIFY(model.rowCount() > baseRows);

    model.setDragActive(false);
    QCOMPARE(model.rowCount(), baseRows);   // 离开拖动恢复原状

    // Created 分组拖动被整体禁止：不物化任何投放区。
    spec.grouping = GroupMode::Created;
    model.setFilter(spec);
    const int createdRows = model.rowCount();
    model.setDragActive(true, source);
    QCOMPARE(model.rowCount(), createdRows);
}

void GroupModelTests::modelHistoryScopeBlocksDrag()
{
    QVERIFY(open());
    TaskStore store(Database::instance().connection());
    QVERIFY2(store.load(), qPrintable(store.lastError()));
    const QString listId = store.inboxListId();
    const QString done = store.createTask(QStringLiteral("完成"), listId);
    store.setCompleted(done, true);

    TaskListModel model(&store);
    ViewSpec spec;
    spec.scope = ViewScope::Completed;
    spec.grouping = GroupMode::Time;
    model.setFilter(spec);
    const QModelIndex index = model.indexForTask(done);
    QVERIFY(index.isValid());
    QCOMPARE(model.dragBlockReason(index), QStringLiteral("历史视图不能拖动"));
    QVERIFY(!(index.flags() & Qt::ItemIsDragEnabled));
    QVERIFY(model.mimeData(QModelIndexList{index}) == nullptr);
    const int rows = model.rowCount();
    model.setDragActive(true, done);
    QCOMPARE(model.rowCount(), rows);   // 历史视角不物化任何投放区
    model.setDragActive(false);
    QCOMPARE(model.rowCount(), rows);
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    GroupModelTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_groupmodel.moc"
