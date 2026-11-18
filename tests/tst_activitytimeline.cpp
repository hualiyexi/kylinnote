#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>
#include <QtTest>
#include <memory>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "db/TaskRepository.h"
#include "domain/Entity.h"
#include "ui/ActivityTimelineWidget.h"

using namespace kl;

namespace {
// 布局项顺序 = 行创建顺序 = 最新在前（findChildren 不保证顺序，故走布局）。
QList<QLabel *> orderedSummaries(const ActivityTimelineWidget &widget)
{
    QList<QLabel *> result;
    QWidget *rows = widget.findChild<QWidget *>(QStringLiteral("ActivityTimelineRows"));
    if (!rows || !rows->layout()) return result;
    const QLayout *layout = rows->layout();
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *row = layout->itemAt(i)->widget();
        if (!row) continue;
        QLabel *label = row->findChild<QLabel *>(QStringLiteral("ActivityTimelineRowSummary"));
        if (label) result.append(label);
    }
    return result;
}
int rowCount(const ActivityTimelineWidget &widget)
{
    QWidget *rows = widget.findChild<QWidget *>(QStringLiteral("ActivityTimelineRows"));
    return (rows && rows->layout()) ? rows->layout()->count() : 0;
}
} // namespace

// 任务动态时间线组件：数据正确性、跨实体隔离、同 id 不重置、上下文与关闭信号。
// 可见性与业务写入端共用 ActivityPolicy，历史展示噪声不进入时间线。
class TestActivityTimeline : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void objectNames();
    void showsNewestFirstAndReadableSummaries();
    void equalTimestampsUseStableIdOrder();
    void sameEntityIdKeepsRows();
    void emptyEntityIdClearsRows();
    void entitiesDoNotBleed();
    void closeEmitsSignalOnly();
    void noTechnicalNoiseInSummaries();
    void eventsChangedFiltersAndDirtyPath();
    void contextSnapshotsAndReadableDates();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
    QString m_taskA;
    QString m_taskB;
};

void TestActivityTimeline::initTestCase()
{
    m_dir.reset(new QTemporaryDir());
    QVERIFY(m_dir->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
    QString error;
    QVERIFY2(Database::instance().openAt(m_dir->filePath(QStringLiteral("kylinlist.db")), &error),
             qPrintable(error));
    m_store.reset(new TaskStore(Database::instance().connection()));
    QVERIFY2(m_store->load(), qPrintable(m_store->lastError()));

    // A：创建 → 改标题 → 完成；B：只创建。事件按发生顺序入库（旧在前）。
    m_taskA = m_store->createTask(QStringLiteral("标题A"));
    m_taskB = m_store->createTask(QStringLiteral("标题B"));
    QVERIFY(!m_taskA.isEmpty());
    QVERIFY(!m_taskB.isEmpty());
    QVERIFY(m_taskA != m_taskB);
    // Chronological assertions need distinct timestamps; same-millisecond ties
    // intentionally use UUID order and are covered separately below.
    QTRY_VERIFY(appNow() > m_store->entity(m_taskA).createdAt);
    Entity entity = m_store->entity(m_taskA);
    entity.title = QStringLiteral("标题B改");
    QVERIFY(m_store->saveTask(entity));
    QTRY_VERIFY(appNow() > m_store->entity(m_taskA).updatedAt);
    QVERIFY(m_store->setCompleted(m_taskA, true));
}

void TestActivityTimeline::cleanupTestCase()
{
    m_store.reset();
    Database::instance().close();
    qunsetenv("KYLINLIST_DATA_DIR");
}

void TestActivityTimeline::objectNames()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    QCOMPARE(widget.objectName(), QStringLiteral("ActivityTimeline"));
    QVERIFY(widget.findChild<QWidget *>(QStringLiteral("ActivityTimelineHeader")) != nullptr);
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("ActivityTimelineClose")) != nullptr);
    QVERIFY(widget.findChild<QWidget *>(QStringLiteral("ActivityTimelineRows")) != nullptr);
    QVERIFY(rowCount(widget) > 0);
    QCOMPARE(widget.entityId(), m_taskA);
}

void TestActivityTimeline::showsNewestFirstAndReadableSummaries()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    widget.resize(360, 200);

    const QList<QLabel *> summaries = orderedSummaries(widget);
    QVERIFY(summaries.size() >= 3);
    // 最新在前：完成事件是最后一次操作。
    QCOMPARE(summaries.first()->text(), QStringLiteral("完成了任务"));
    QVERIFY(summaries.at(1)->text().contains(QStringLiteral("将标题修改为「标题B改」")));
    QCOMPARE(summaries.last()->text(), QStringLiteral("创建了任务「标题A」"));
    // 时间行始终有可读内容。
    for (QLabel *time : widget.findChildren<QLabel *>(QStringLiteral("ActivityTimelineRowTime")))
        QVERIFY(!time->text().isEmpty());
}

void TestActivityTimeline::equalTimestampsUseStableIdOrder()
{
    const QString id = m_store->createTask(QStringLiteral("同刻事件"));
    QVERIFY(!id.isEmpty());
    QString lowId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString highId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (lowId > highId) qSwap(lowId, highId);
    EntityEvent low;
    low.id = lowId;
    low.entityId = id;
    low.entityKind = EntityKind::Task;
    low.entityTitle = QStringLiteral("同刻事件");
    low.batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    low.type = QStringLiteral("changed");
    low.field = QStringLiteral("title");
    low.oldValue = QStringLiteral("同刻事件");
    low.newValue = QStringLiteral("较小ID事件");
    low.occurredAt = appNow().addSecs(1);
    EntityEvent high = low;
    high.id = highId;
    high.newValue = QStringLiteral("较大ID事件");
    TaskRepository repository(Database::instance().connection());
    QString error;
    QVERIFY2(repository.appendEvents({high, low}, &error), qPrintable(error));
    const QVector<EntityEvent> events = m_store->events(id);
    QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(1).id, lowId);
    QCOMPARE(events.at(2).id, highId);

    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(id);
    const QList<QLabel *> summaries = orderedSummaries(widget);
    QCOMPARE(summaries.size(), 3);
    QVERIFY(summaries.first()->text().contains(QStringLiteral("较大ID事件")));
    QVERIFY(summaries.at(1)->text().contains(QStringLiteral("较小ID事件")));
}

void TestActivityTimeline::sameEntityIdKeepsRows()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    const int rowsBefore = rowCount(widget);
    QVERIFY(rowsBefore > 0);
    QPointer<QLabel> firstSummary = orderedSummaries(widget).first();

    widget.setEntityId(m_taskA);   // 同 id：不重置
    QCOMPARE(rowCount(widget), rowsBefore);
    QVERIFY(firstSummary != nullptr);

    widget.refresh();              // 显式刷新：允许重建
    QCOMPARE(rowCount(widget), rowsBefore);
    QVERIFY(firstSummary == nullptr);   // 行已重建
}

void TestActivityTimeline::emptyEntityIdClearsRows()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    QVERIFY(rowCount(widget) > 0);
    widget.setEntityId(QString());
    QCOMPARE(rowCount(widget), 0);
    QVERIFY(widget.entityId().isEmpty());
}

void TestActivityTimeline::entitiesDoNotBleed()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    const QList<QLabel *> summariesA = orderedSummaries(widget);
    QVERIFY(summariesA.size() >= 3);

    widget.setEntityId(m_taskB);
    const QList<QLabel *> summariesB = orderedSummaries(widget);
    QVERIFY(summariesB.size() < summariesA.size());
    for (QLabel *label : summariesB)
        QVERIFY(label->text().contains(QStringLiteral("创建了任务")));
}

void TestActivityTimeline::closeEmitsSignalOnly()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    widget.show();
    QSignalSpy spy(&widget, SIGNAL(closeRequested()));
    QToolButton *close = widget.findChild<QToolButton *>(QStringLiteral("ActivityTimelineClose"));
    QVERIFY(close != nullptr);
    QTest::mouseClick(close, Qt::LeftButton);
    QCOMPARE(spy.count(), 1);
    // 组件只发信号，不隐藏自己。
    QVERIFY(widget.isVisible());
}

void TestActivityTimeline::noTechnicalNoiseInSummaries()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    const QRegularExpression uuidPattern(
        QStringLiteral("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"));
    for (QLabel *label : orderedSummaries(widget)) {
        const QString text = label->text();
        QVERIFY(!text.contains(QStringLiteral("NULL")));
        QVERIFY(!text.contains(QStringLiteral("批次")));
        QVERIFY(!text.contains(uuidPattern));
        QVERIFY(!text.contains(QStringLiteral("{")));
    }
}

void TestActivityTimeline::eventsChangedFiltersAndDirtyPath()
{
    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(m_taskA);
    const int rowsBefore = rowCount(widget);
    QVERIFY(rowsBefore > 0);

    // 可见前的显示不重置（未置脏）。
    QPointer<QLabel> firstSummary = orderedSummaries(widget).first();
    emit m_store->eventsChanged(m_taskB);   // 其它实体：既不刷新也不置脏
    widget.show();
    QVERIFY(firstSummary != nullptr);

    // 隐藏时当前实体事件置脏，showEvent 时补偿刷新。
    widget.hide();
    emit m_store->eventsChanged(m_taskA);
    QVERIFY(firstSummary != nullptr);
    widget.show();
    QVERIFY(firstSummary == nullptr);
    QCOMPARE(rowCount(widget), rowsBefore);

    // 可见时当前实体事件立即刷新。
    QPointer<QLabel> rebuilt = orderedSummaries(widget).first();
    emit m_store->eventsChanged(m_taskA);
    QVERIFY(rebuilt == nullptr);
    QCOMPARE(rowCount(widget), rowsBefore);
}

void TestActivityTimeline::contextSnapshotsAndReadableDates()
{
    const QString id = m_store->createTask(QStringLiteral("上下文展示测试"));
    QVERIFY(!id.isEmpty());
    const Entity entity = m_store->entity(id);
    const QString batchId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QDateTime occurredAt = appNow().addSecs(1);
    QVector<EntityEvent> events;
    const auto append = [&](const QString& type, const QString& field,
                            const QVariant& before, const QVariant& after,
                            const QJsonObject& context = QJsonObject()) {
        EntityEvent event;
        event.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        event.entityId = id;
        event.entityKind = entity.kind;
        event.entityTitle = entity.title;
        event.batchId = batchId;
        event.type = type;
        event.field = field;
        event.oldValue = before;
        event.newValue = after;
        event.context = context;
        event.occurredAt = occurredAt;
        events.append(event);
    };
    const QString parentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    append(QStringLiteral("changed"), QStringLiteral("parent"), QString(), parentId,
           QJsonObject{{QStringLiteral("newParentTitle"), QStringLiteral("主任务<原样文字>")}});
    const QDate firstDay(toAppTime(appNow()).date().year(), 1, 1);
    append(QStringLiteral("changed"), QStringLiteral("allDayDate"),
           firstDay.toString(Qt::ISODate), firstDay.addDays(1).toString(Qt::ISODate));
    append(QStringLiteral("changed"), QStringLiteral("listArchived"),
           QStringLiteral("123"), QStringLiteral("0"),
           QJsonObject{{QStringLiteral("listName"), QStringLiteral("资料<清单>")}});
    append(QStringLiteral("attachment_added"), QStringLiteral("attachment"),
           QVariant(), QStringLiteral("attachment-id"),
           QJsonObject{{QStringLiteral("attachmentName"), QStringLiteral("报告<final>.pdf")}});
    append(QStringLiteral("changed"), QStringLiteral("sortOrder"), QStringLiteral("0"), QStringLiteral("1"));
    append(QStringLiteral("changed"), QStringLiteral("pinned"), QString(), QStringLiteral("123"));
    TaskRepository repository(Database::instance().connection());
    QString error;
    QVERIFY2(repository.appendEvents(events, &error), qPrintable(error));

    ActivityTimelineWidget widget(m_store.get());
    widget.setEntityId(id);
    QCOMPARE(rowCount(widget), 5); // 创建 + 四条实质活动，历史排序和置顶不展示。
    QStringList summaries;
    for (QLabel* label : orderedSummaries(widget)) {
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(!label->text().contains(parentId));
        summaries.append(label->text());
    }
    QVERIFY(summaries.contains(QStringLiteral("添加了主任务「主任务<原样文字>」")));
    QVERIFY(summaries.contains(QStringLiteral("日期：1月1日 → 1月2日")));
    QVERIFY(summaries.contains(QStringLiteral("取消了清单「资料<清单>」的归档")));
    QVERIFY(summaries.contains(QStringLiteral("添加了附件「报告<final>.pdf」")));
}

QTEST_MAIN(TestActivityTimeline)
#include "tst_activitytimeline.moc"
