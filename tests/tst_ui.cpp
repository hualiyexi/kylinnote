#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QtTest>
#include <memory>
#include "app/NoteManager.h"
#include "app/KylinListApp.h"
#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "domain/ViewSpec.h"
#include "ui/AppStyle.h"
#include "ui/ActivityLogDialog.h"
#include "ui/EntityHoverPreview.h"
#include "ui/EntityPicker.h"
#include "ui/AttachmentWidget.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/NoteChecklist.h"
#include "ui/NoteWidget.h"
#include "ui/ReminderPopup.h"
#include "ui/ResizeGrip.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskDelegate.h"
#include "ui/TaskListModel.h"
#include "ui/TaskPanel.h"
using namespace kl;

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, m_dir->path() + QStringLiteral("/settings"));
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/kylinlist.db"), &error),
                 qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY(m_store->load(&error));
    }
    void cleanup() {
        m_store.reset();
        Database::instance().close();
        m_dir.reset();
    }
    void markdownFormattingAndSafety() {
        const QString html = Markdown::toHtml(QStringLiteral(
            "## 标题\n\n**加粗** *斜体* ~~删除~~\n\n- [ ] 未完成\n- [x] 完成\n\n<script>bad()</script>"));
        QVERIFY(html.contains(QStringLiteral("<strong>加粗</strong>")));
        QVERIFY(html.contains(QStringLiteral("<em>斜体</em>")));
        QVERIFY(html.contains(QStringLiteral("<del>删除</del>")));
        QVERIFY(!html.contains(QStringLiteral("<script>")));
        QVERIFY(html.contains(QStringLiteral("&#9744;")));
        QVERIFY(html.contains(QStringLiteral("&#9745;")));
        MarkdownEditor editor;
        editor.setContent(QStringLiteral("hello"));
        editor.textEditor()->selectAll();
        auto* bold = editor.findChild<QToolButton*>(QStringLiteral("format0"));
        QVERIFY(bold);
        QTest::mouseClick(bold, Qt::LeftButton);
        QCOMPARE(editor.content(), QStringLiteral("**hello**"));
        editor.textEditor()->undo();
        QCOMPARE(editor.content(), QStringLiteral("hello"));
    }
    void internalLinkParsingAndRendering() {
        const QString first = newUuid(), second = newUuid();
        const QString body = QStringLiteral("[一](kl:%1) [二](kl:%2) [重复](kl:%1) ![图片](kl:%2) "
                                            "[大写](kl:%3) [外链](https://example.com) 纯文本 kl:%1")
            .arg(first, second, first.toUpper());
        QCOMPARE(internalTargets(body), QStringList({first, second}));
        QVERIFY(isValidLinkTarget(first)); QVERIFY(isValidLinkTarget(QStringLiteral("kl:") + first));
        QVERIFY(!isValidLinkTarget(first.toUpper())); QVERIFY(!isValidLinkTarget(QStringLiteral("https://example.com")));
        QVERIFY(!isValidLinkTarget(QStringLiteral("kl:/") + first)); QVERIFY(!isValidLinkTarget(QStringLiteral("kl:bad")));

        const QString listA = m_store->createList(QStringLiteral("目标清单"));
        const QString listB = m_store->createList(QStringLiteral("来源清单"));
        const QString target = m_store->createTask(QStringLiteral("旧标题"), listA);
        const QString source = m_store->createNote(QStringLiteral("来源笔记"), listB);
        Entity sourceEntity = m_store->entity(source);
        sourceEntity.body = QStringLiteral("[存储文字](kl:%1)").arg(target); QVERIFY(m_store->saveTask(sourceEntity));
        MarkdownEditor editor; editor.setStore(m_store.get()); editor.setContent(sourceEntity.body);
        QVERIFY(editor.preview()->document()->toPlainText().contains(QStringLiteral("旧标题")));
        Entity renamed = m_store->entity(target); renamed.title = QStringLiteral("新标题"); QVERIFY(m_store->saveTask(renamed));
        const QString html = editor.preview()->toHtml();
        QVERIFY(html.contains(QStringLiteral("新标题"))); QVERIFY(!html.contains(QStringLiteral("旧标题")));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body); // 预览刷新不改数据库原文
        QVERIFY(Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities())).contains(QStringLiteral("class=\"internal-link\"")));
        const QString external = Markdown::toHtml(QStringLiteral("[安全](https://example.com) <script>bad()</script>"));
        QVERIFY(external.contains(QStringLiteral("href=\"https://example.com\""))); QVERIFY(!external.contains(QStringLiteral("<script>")));
        const QString missing = newUuid();
        const QString broken = Markdown::toHtml(QStringLiteral("[保留文字](kl:%1)").arg(missing), Markdown::entityMap(m_store->entities()));
        QVERIFY(broken.contains(QStringLiteral("<span class=\"broken-link\">保留文字</span>")));
        QVERIFY(!broken.contains(QStringLiteral("href=\"kl:%1\"").arg(missing)));
        QVERIFY(m_store->trashTask(target));
        const QString trash = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(trash.contains(QStringLiteral("trash-link"))); QVERIFY(trash.contains(QStringLiteral("回收站")));
        QVERIFY(trash.contains(QStringLiteral("href=\"kl:%1\"").arg(target)));
        QVERIFY(m_store->deletePermanently(target));
        const QString deleted = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(deleted.contains(QStringLiteral("<span class=\"broken-link\">存储文字</span>")));
        QVERIFY(!deleted.contains(QStringLiteral("href=\"kl:%1\"").arg(target)));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body);
    }
    void documentPreviewSignalsCrossListsAndHover() {
        const QString listA = m_store->createList(QStringLiteral("甲清单"));
        const QString listB = m_store->createList(QStringLiteral("乙清单"));
        const QString target = m_store->createTask(QStringLiteral("悬浮目标"), listA);
        const QString source = m_store->createNote(QStringLiteral("链接来源"), listB);
        Entity edited = m_store->entity(target); edited.body = QStringLiteral("摘要 **加粗**\n\n第二行");
        edited.dueAt = appNow().addDays(-1); QVERIFY(m_store->saveTask(edited));
        edited = m_store->entity(source); edited.body = QStringLiteral("[目标](kl:%1)").arg(target); QVERIFY(m_store->saveTask(edited));
        DocumentPreview preview; preview.setStore(m_store.get()); preview.resize(500, 240); preview.show();
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        const QTextCursor anchor = preview.document()->find(QStringLiteral("悬浮目标")); QVERIFY(!anchor.isNull());
        QCOMPARE(preview.internalLinkAt(preview.cursorRect(anchor).center()), target);
        QSignalSpy activated(&preview, &DocumentPreview::internalLinkActivated);
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(QStringLiteral("kl:") + target)));
        QCOMPARE(activated.count(), 1); QCOMPARE(activated.takeFirst().at(0).toString(), target);
        QVERIFY(m_store->trashTask(target));
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(QStringLiteral("kl:") + target)));
        QCOMPARE(activated.count(), 1); // 回收站链接仍可激活
        QVERIFY(m_store->restoreTask(target));

        TaskPanel panel(m_store.get()); panel.showTask(source);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail); QCOMPARE(detail->entityId(), source);
        auto* editor = detail->findChild<MarkdownEditor*>(); QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "internalLinkActivated", Qt::DirectConnection, Q_ARG(QString, target));
        QCOMPARE(detail->entityId(), target); // 跨清单通过同一信号跳转
        QCOMPARE(m_store->entity(source).body, edited.body);

        EntityHoverPreview* hover = preview.findChild<EntityHoverPreview*>(); QVERIFY(hover);
        QTimer* timer = hover->findChild<QTimer*>(QStringLiteral("HoverDelay")); QVERIFY(timer); QCOMPARE(timer->interval(), 500);
        hover->schedule(target, preview.mapToGlobal(QPoint(20, 20))); QVERIFY(!hover->isVisible());
        QTest::qWait(560); QVERIFY(hover->isVisible());
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverTitle"))->text(), QStringLiteral("悬浮目标"));
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverMeta"))->text(), QStringLiteral("任务 · 已过期"));
        const QString summary = hover->findChild<QLabel*>(QStringLiteral("HoverSummary"))->text();
        QVERIFY(summary.contains(QStringLiteral("摘要"))); QVERIFY(summary.contains(QStringLiteral("加粗")));
        QVERIFY(summary.contains(QStringLiteral("第二行"))); QVERIFY(!summary.contains(QStringLiteral("**")));
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&preview, &leave); QVERIFY(!hover->isVisible());
        const QString note = m_store->createNote(QStringLiteral("笔记悬浮"));
        hover->schedule(note, preview.mapToGlobal(QPoint(20, 20))); QTest::qWait(560); QVERIFY(hover->isVisible());
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverMeta"))->text(), QStringLiteral("笔记"));
        hover->schedule(target, preview.mapToGlobal(QPoint(20, 20)));
        QVERIFY(m_store->trashTask(target)); QVERIFY(m_store->deletePermanently(target));
        QTest::qWait(560); QVERIFY(!hover->isVisible()); // 目标消失取消待显示卡片
    }
    void entityPickerSearchAndReferenceInsertion() {
        const QString task = m_store->createTask(QStringLiteral("目标 [A]"));
        const QString note = m_store->createNote(QStringLiteral("另一笔记"));
        const QString deleted = m_store->createTask(QStringLiteral("已删除选项")); QVERIFY(m_store->trashTask(deleted));
        EntityPickerDialog dialog(m_store.get());
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("EntityPickerList"));
        auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("EntityPickerSearch"));
        QVERIFY(list && search); QCOMPARE(list->count(), 2);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("任务")) || list->item(0)->text().contains(QStringLiteral("笔记")));
        search->setText(QStringLiteral("另一")); QCOMPARE(list->count(), 1);
        QCOMPARE(dialog.selectedEntity().id, note); QCOMPARE(dialog.selectedEntity().kind, EntityKind::Note);
        search->setText(QStringLiteral("[A]")); QCOMPARE(list->count(), 1); QCOMPARE(dialog.selectedEntity().id, task);
        MarkdownEditor editor; editor.setStore(m_store.get());
        auto* button = editor.findChild<QToolButton*>(QStringLiteral("insertEntityReference")); QVERIFY(button); QVERIFY(button->isEnabled());
        QCOMPARE(button->text(), QStringLiteral("实体引用"));
        QVERIFY(editor.insertEntityReference(task));
        QCOMPARE(editor.content(), QStringLiteral("[目标 \\[A\\]](kl:%1)").arg(task));
        QVERIFY(editor.preview()->toHtml().contains(QStringLiteral("目标 [A]")));
        QVERIFY(!editor.insertEntityReference(newUuid()));
        QCOMPARE(editor.textEditor()->acceptRichText(), false);
    }
    void filtersUseShanghaiCalendarAndSearch() {
        const QDateTime now = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(12, 0), Qt::LocalTime));
        const QDate today = toAppTime(now).date();
        Entity task; task.id = newUuid(); task.title = QStringLiteral("整理材料"); task.listId = newUuid();
        task.startAt = now.addDays(-1); task.dueAt = now; task.tags << QStringLiteral("工作");
        ChecklistItem item; item.text = QStringLiteral("核对清单"); task.checklist.append(item);
        ViewSpec spec; spec.scope = ViewScope::Today;
        QVERIFY(contains(task, spec, today));
        spec.search = QStringLiteral("材料 工作 清单");
        QVERIFY(contains(task, spec, today));
        spec.search = QStringLiteral("材料 其他"); QVERIFY(!contains(task, spec, today));
        spec.search.clear(); task.status = TaskStatus::Completed; task.completed = true;
        QVERIFY(!contains(task, spec, today));
        spec.scope = ViewScope::Completed; QVERIFY(contains(task, spec, today));
        task.deleted = true; QVERIFY(!contains(task, spec, today));
        spec.scope = ViewScope::Trash; QVERIFY(contains(task, spec, today));
    }
    void filtersUseShanghaiDatesAcrossUtcBoundary() {
        const QDateTime now(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        QCOMPARE(toAppTime(now).date(), QDate(2026, 11, 6));
        Entity early; early.id = newUuid(); early.title = QStringLiteral("跨 UTC 日期的上海任务");
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(0, 15), Qt::UTC));
        QCOMPARE(early.dueAt, QDateTime(QDate(2026, 11, 5), QTime(16, 15), Qt::UTC));
        Entity previous = early; previous.id = newUuid();
        previous.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 5), QTime(23, 45), Qt::LocalTime));
        ViewSpec spec; spec.scope = ViewScope::Today;
        QVERIFY(contains(early, spec, QDate(2026, 11, 6)));
        QVERIFY(contains(previous, spec, QDate(2026, 11, 6))); // 今天视图包含有效日期早于今天的任务
        previous.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 7), QTime(0, 0), Qt::LocalTime));
        QVERIFY(!contains(previous, spec, QDate(2026, 11, 6)));
        QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        QCOMPARE(scheduleLabel(early), QStringLiteral("截止 11月6日 00:15"));
        early.allDayDate = QDate(2025, 1, 1); QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        early.dueAt = QDateTime(); QCOMPARE(effectiveDate(early), QDate(2025, 1, 1));
        early.allDayDate = QDate(); early.startAt = now; QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        spec.scope = ViewScope::Next7Days;
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 13), QTime(23, 59)));
        QVERIFY(contains(early, spec, QDate(2026, 11, 6)));
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 14), QTime(0, 0)));
        QVERIFY(!contains(early, spec, QDate(2026, 11, 6)));
    }
    void viewSpecNotesStayOutOfTaskScopes() {
        const QDate today(2026, 11, 6);
        Entity note; note.id = newUuid(); note.kind = EntityKind::Note; note.title = QStringLiteral("保留状态的笔记");
        note.body = QStringLiteral("完整正文"); note.listId = newUuid(); note.dueAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        note.startAt = fromAppTime(QDateTime(today.addDays(-2), QTime(9, 0))); note.priority = 3;
        note.tags << QStringLiteral("标签"); ChecklistItem item; item.text = QStringLiteral("清单内容"); note.checklist.append(item);
        for (const TaskStatus status : {TaskStatus::Completed, TaskStatus::Abandoned}) {
            note.status = status; note.completed = status == TaskStatus::Completed;
            ViewSpec all; QVERIFY(contains(note, all, today));
            ViewSpec list = all; list.scope = ViewScope::List; list.listId = note.listId; QVERIFY(contains(note, list, today));
            list.search = QStringLiteral("正文 标签 清单"); QVERIFY(contains(note, list, today));
            list.search = QStringLiteral("正文 缺失"); QVERIFY(!contains(note, list, today));
            for (const ViewScope scope : {ViewScope::Today, ViewScope::Next7Days, ViewScope::Overdue,
                                          ViewScope::Completed, ViewScope::Abandoned}) {
                ViewSpec spec; spec.scope = scope; QVERIFY(!contains(note, spec, today));
                spec.renderer = ViewRenderer::Time; QVERIFY(!contains(note, spec, today));
            }
        }
        ViewSpec time; time.renderer = ViewRenderer::Time; QVERIFY(!contains(note, time, today));
        note.deleted = true; ViewSpec trash; trash.scope = ViewScope::Trash; QVERIFY(contains(note, trash, today));
    }
    void viewSpecSectionsOrderingAndArchives() {
        const QDate today(2026, 11, 6);
        const QDateTime base = fromAppTime(QDateTime(today, QTime(12, 0)));
        const auto entity = [&](const char* id, EntityKind kind = EntityKind::Task) {
            Entity value; value.id = QLatin1String(id); value.kind = kind; value.title = value.id; value.createdAt = base; return value;
        };
        Entity pinnedNote = entity("00000000-0000-4000-8000-000000000001", EntityKind::Note);
        Entity pinnedTask = entity("00000000-0000-4000-8000-000000000002");
        Entity futurePinned = entity("00000000-0000-4000-8000-000000000003");
        pinnedNote.status = TaskStatus::Completed; // 残留状态不影响普通清单
        pinnedNote.pinnedAt = base; pinnedTask.pinnedAt = base; futurePinned.pinnedAt = base.addSecs(60);
        pinnedTask.dueAt = base; futurePinned.dueAt = base.addDays(1);
        Entity overdueA = entity("00000000-0000-4000-8000-000000000010");
        Entity overdueB = entity("00000000-0000-4000-8000-000000000011");
        overdueA.dueAt = base.addDays(-1); overdueB.dueAt = overdueA.dueAt;
        Entity todayNine = entity("00000000-0000-4000-8000-000000000020");
        todayNine.dueAt = fromAppTime(QDateTime(today, QTime(9, 0))); // 当天 09:00 不按钟点过期
        Entity noDateA = entity("00000000-0000-4000-8000-000000000030");
        Entity noDateB = entity("00000000-0000-4000-8000-000000000031");
        Entity noteA = entity("00000000-0000-4000-8000-000000000040", EntityKind::Note);
        Entity noteB = entity("00000000-0000-4000-8000-000000000041", EntityKind::Note);
        const QVector<Entity> all{todayNine, noteB, overdueB, noDateA, pinnedTask, futurePinned,
                                  overdueA, pinnedNote, noDateB, noteA};
        ViewSpec list;
        const QVector<ViewSection> sections = buildSections(all, list, today);
        QCOMPARE(sections.size(), 5);
        QCOMPARE(sections.at(0).title, QStringLiteral("置顶")); QCOMPARE(sections.at(1).title, QStringLiteral("已过期"));
        QCOMPARE(sections.at(2).title, QStringLiteral("未过期")); QCOMPARE(sections.at(3).title, QStringLiteral("无日期"));
        QCOMPARE(sections.at(4).title, QStringLiteral("笔记"));
        QCOMPARE(sections.at(0).items.size(), 3);
        QCOMPARE(sections.at(0).items.at(0).id, futurePinned.id);
        QCOMPARE(sections.at(0).items.at(1).id, pinnedNote.id); QCOMPARE(sections.at(0).items.at(2).id, pinnedTask.id);
        QCOMPARE(sections.at(1).items.at(0).id, overdueA.id); QCOMPARE(sections.at(1).items.at(1).id, overdueB.id);
        QCOMPARE(sections.at(2).items.size(), 1); QCOMPARE(sections.at(2).items.first().id, todayNine.id);
        QCOMPARE(sections.at(3).items.at(0).id, noDateA.id); QCOMPARE(sections.at(3).items.at(1).id, noDateB.id);
        QCOMPARE(sections.at(4).items.at(0).id, noteA.id); QCOMPARE(sections.at(4).items.at(1).id, noteB.id);

        ViewSpec time; time.scope = ViewScope::Today; time.renderer = ViewRenderer::Time;
        QVERIFY(!contains(futurePinned, time, today)); // 置顶不豁免今天范围
        QVERIFY(!contains(noDateA, time, today)); QVERIFY(!contains(noteA, time, today));
        const QVector<ViewSection> timed = buildSections(all, time, today);
        QCOMPARE(timed.size(), 3); QCOMPARE(timed.at(0).title, QStringLiteral("置顶"));
        QCOMPARE(timed.at(0).items.size(), 1); QCOMPARE(timed.at(0).items.first().id, pinnedTask.id);
        QCOMPARE(timed.at(1).title, QStringLiteral("已过期")); QCOMPARE(timed.at(2).title, QStringLiteral("未过期"));
        QCOMPARE(timed.at(2).items.first().id, todayNine.id);
        time.scope = ViewScope::Next7Days; QVERIFY(contains(futurePinned, time, today));
        time.scope = ViewScope::Overdue; QVERIFY(!contains(todayNine, time, today));

        Entity completedA = entity("00000000-0000-4000-8000-000000000050");
        Entity completedB = entity("00000000-0000-4000-8000-000000000051");
        completedA.status = completedB.status = TaskStatus::Completed; completedA.completed = completedB.completed = true;
        completedA.completedAt = base; completedB.completedAt = base.addSecs(10); completedA.pinnedAt = base.addSecs(20);
        ViewSpec completed; completed.scope = ViewScope::Completed;
        const QVector<ViewSection> archive = buildSections({completedA, completedB, pinnedNote}, completed, today);
        QCOMPARE(archive.size(), 1); QCOMPARE(archive.first().title, QStringLiteral("已完成"));
        QCOMPARE(archive.first().items.size(), 2); QCOMPARE(archive.first().items.at(0).id, completedB.id);
        QCOMPARE(archive.first().items.at(1).id, completedA.id); // 归档不设置顶段
        completedB.completedAt = completedA.completedAt;
        const QVector<ViewSection> tied = buildSections({completedB, completedA}, completed, today);
        QCOMPARE(tied.first().items.at(0).id, completedA.id); QCOMPARE(tied.first().items.at(1).id, completedB.id);
        Entity abandoned = completedA; abandoned.status = TaskStatus::Abandoned; abandoned.completed = false; abandoned.abandonedAt = base;
        ViewSpec abandonedSpec; abandonedSpec.scope = ViewScope::Abandoned;
        QCOMPARE(buildSections({abandoned}, abandonedSpec, today).first().title, QStringLiteral("已放弃"));
        Entity trashedNote = noteA; trashedNote.deleted = true; trashedNote.deletedAt = base;
        Entity trashedTask = todayNine; trashedTask.deleted = true; trashedTask.deletedAt = base.addSecs(1);
        ViewSpec trash; trash.scope = ViewScope::Trash;
        const QVector<ViewSection> trashSections = buildSections({trashedNote, trashedTask}, trash, today);
        QCOMPARE(trashSections.size(), 1); QCOMPARE(trashSections.first().items.at(0).id, trashedTask.id);
    }
    void taskListModelSectionsAndNoteRoles() {
        const QString task = m_store->createTask(QStringLiteral("模型任务"));
        const QString note = m_store->createNote(QStringLiteral("模型笔记"));
        Entity edited = m_store->entity(task); edited.body = QStringLiteral("- [ ] 正文不是清单"); edited.dueAt = appNow().addDays(-1);
        QVERIFY(m_store->saveTask(edited));
        TaskListModel model(m_store.get()); QCOMPARE(model.entityCount(), 2);
        QVERIFY(model.rowCount() >= 3); // 段头不侵占实体 ID 映射
        bool sawHeader = false;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, 0);
            if (model.data(index, TaskListModel::HeaderRole).toBool()) {
                sawHeader = true; QVERIFY(model.entityId(index).isEmpty());
                QVERIFY(!(model.flags(index) & Qt::ItemIsSelectable));
            }
        }
        QVERIFY(sawHeader);
        const QModelIndex noteIndex = model.indexForTask(note); QVERIFY(noteIndex.isValid());
        QCOMPARE(model.entityId(noteIndex), note);
        QCOMPARE(model.data(noteIndex, TaskListModel::KindRole).toInt(), static_cast<int>(EntityKind::Note));
        QVERIFY(!(model.flags(noteIndex) & Qt::ItemIsUserCheckable));
        QVERIFY(!model.setData(noteIndex, Qt::Checked, Qt::CheckStateRole));
        QVERIFY(!model.data(noteIndex, TaskListModel::CompletedRole).toBool());
        QVERIFY(model.data(noteIndex, TaskListModel::MetaRole).toString().contains(QStringLiteral("笔记")));
        const QModelIndex taskIndex = model.indexForTask(task); QVERIFY(taskIndex.isValid());
        QVERIFY(model.data(taskIndex, TaskListModel::OverdueRole).toBool());
        QVERIFY(!model.data(taskIndex, TaskListModel::MetaRole).toString().contains(QStringLiteral("清单 0/")));
        QVERIFY(model.setData(taskIndex, Qt::Checked, Qt::CheckStateRole)); QVERIFY(m_store->entity(task).completed);
        QVERIFY(m_store->setPinnedAt(note, appNow()));
        QVERIFY(model.data(model.indexForTask(note), TaskListModel::PinnedRole).toBool());
    }
    void shanghai0030DetailInputIsHostTimeZoneIndependent() {
        const QString id = m_store->createTask(QStringLiteral("上海墙上时间"));
        QVERIFY(isUuid(id));
        TaskDetailWidget detail(m_store.get());
        QVERIFY(detail.setTask(id));
        auto* start = detail.findChild<QDateTimeEdit*>(QStringLiteral("start"));
        auto* reminder = detail.findChild<QDateTimeEdit*>(QStringLiteral("reminder"));
        auto* hasStart = detail.findChild<QCheckBox*>(QStringLiteral("startEnabled"));
        auto* hasReminder = detail.findChild<QCheckBox*>(QStringLiteral("reminderEnabled"));
        QVERIFY(start && reminder && hasStart && hasReminder);

        const QDateTime wall(QDate(2026, 11, 6), QTime(0, 30), Qt::LocalTime);
        hasStart->setChecked(true);
        start->setDateTime(wall);
        hasReminder->setChecked(true);
        reminder->setDateTime(QDateTime(QDate(2026, 11, 6), QTime(0, 15), Qt::LocalTime));
        QVERIFY(detail.flush());
        const QDateTime expectedStart(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        const QDateTime expectedReminder(QDate(2026, 11, 5), QTime(16, 15), Qt::UTC);
        QCOMPARE(m_store->entity(id).startAt, expectedStart);
        QCOMPARE(m_store->entity(id).reminderAt, expectedReminder);

        QVERIFY(detail.setTask(QString()));
        QVERIFY(detail.setTask(id));
        QCOMPARE(start->dateTime().date(), QDate(2026, 11, 6));
        QCOMPARE(start->dateTime().time(), QTime(0, 30));
        QCOMPARE(reminder->dateTime().date(), QDate(2026, 11, 6));
        QCOMPARE(reminder->dateTime().time(), QTime(0, 15));
    }
    void quickAddAndDetailPersistWithoutLosingPresentation() {
        TaskPanel panel(m_store.get());
        panel.show();
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(quick);
        quick->setText(QStringLiteral("明天提交材料"));
        QTest::keyClick(quick, Qt::Key_Return);
        QCOMPARE(m_store->entities().size(), 1);
        const QString id = m_store->entities().first().id;
        QVERIFY(isUuid(id));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        QVERIFY(detail);
        QCOMPARE(detail->entityId(), id);
        auto* description = detail->findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        description->setPlainText(QStringLiteral("**核对附件**"));
        Entity external = m_store->entity(id);
        external.geometry = QRect(45, 60, 340, 360);
        external.color = NoteColor::Blue;
        QVERIFY(m_store->saveTask(external));
        QVERIFY(detail->flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("**核对附件**"));
        QCOMPARE(m_store->entity(id).geometry, external.geometry);
        QCOMPARE(m_store->entity(id).color, NoteColor::Blue);
        auto *start = detail->findChild<QDateTimeEdit*>(QStringLiteral("start")),
             *due = detail->findChild<QDateTimeEdit*>(QStringLiteral("due"));
        auto *hasStart = detail->findChild<QCheckBox*>(QStringLiteral("startEnabled")),
             *hasDue = detail->findChild<QCheckBox*>(QStringLiteral("dueEnabled"));
        hasStart->setChecked(true);
        hasDue->setChecked(true);
        start->setDateTime(QDateTime(QDate(2026, 11, 6), QTime(10, 0)));
        due->setDateTime(start->dateTime().addSecs(3600));
        QVERIFY(detail->flush());
        due->setDateTime(start->dateTime().addSecs(-3600));
        QVERIFY(!detail->flush());
        QCOMPARE(m_store->entity(id).dueAt, fromAppTime(start->dateTime().addSecs(3600)));
        due->setDateTime(start->dateTime().addSecs(7200));
        QVERIFY(detail->flush());
        auto* list = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        QVERIFY(list);
        auto* model = qobject_cast<TaskListModel*>(list->model());
        QVERIFY(model);
        const auto index = model->indexForTask(id);
        QVERIFY(index.isValid());
        QCOMPARE(model->data(index, TaskListModel::IdRole).toString(), id);
        QCOMPARE(model->data(index, TaskListModel::IdRole).type(), QVariant::String);
        QVERIFY(model->setData(index, Qt::Checked, Qt::CheckStateRole));
        QVERIFY(m_store->entity(id).completed);
    }
    void organizationNavigationAndQuickAddKind() {
        const QString folder = m_store->createFolder(QStringLiteral("单层文件夹"));
        const QString noteList = m_store->createList(QStringLiteral("笔记清单"), folder, EntityKind::Note);
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        auto* renderer = panel.findChild<QComboBox*>(QStringLiteral("ViewRenderer"));
        QVERIFY(navigation && quick && renderer);
        int noteRow = -1, todayRow = -1, allRow = -1;
        for (int row = 0; row < navigation->count(); ++row) {
            const QListWidgetItem* item = navigation->item(row);
            if (item->data(Qt::UserRole + 1).toString() == noteList) noteRow = row;
            if (item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::Today)) todayRow = row;
            if (!item->data(NavigationDelegate::SectionRole).toBool()
                && item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::AllLists)) allRow = row;
        }
        QVERIFY(noteRow >= 0 && todayRow >= 0 && allRow >= 0);
        navigation->setCurrentRow(noteRow);
        QVERIFY(quick->placeholderText().contains(QStringLiteral("笔记"))); QVERIFY(!renderer->isEnabled());
        quick->setText(QStringLiteral("默认新建笔记")); QTest::keyClick(quick, Qt::Key_Return);
        QString noteId;
        for (const Entity& entity : m_store->entities()) if (entity.title == QStringLiteral("默认新建笔记")) noteId = entity.id;
        QVERIFY(isUuid(noteId)); QCOMPARE(m_store->entity(noteId).kind, EntityKind::Note);
        QCOMPARE(m_store->entity(noteId).listId, noteList);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail);
        QCOMPARE(detail->entityId(), noteId);
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted"))->isHidden());
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskPinned"))->isHidden());
        // g02 对齐后置顶不再是常驻 checkbox，而是收进详情「更多」菜单的命令；
        // 兼容控件保留 objectName 但隐藏，业务由菜单 QAction 触发。
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskPinnedAt"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskScheduleHost"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskPriorityHost"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskAppearanceHost"))->isHidden());
        QCOMPARE(detail->findChild<QPushButton*>(QStringLiteral("ConvertKind"))->text(), QStringLiteral("转为任务"));
        navigation->setCurrentRow(todayRow); QVERIFY(renderer->isEnabled());
        renderer->setCurrentIndex(static_cast<int>(ViewRenderer::Time));
        auto* model = qobject_cast<TaskListModel*>(panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QCOMPARE(model->filter().renderer, ViewRenderer::Time);
        navigation->setCurrentRow(allRow); QVERIFY(!renderer->isEnabled());
        QCOMPARE(model->filter().renderer, ViewRenderer::List);
        QVERIFY(m_store->setListFolder(noteList, QString()));
        QVERIFY(m_store->setListFolder(noteList, folder)); QVERIFY(m_store->deleteFolder(folder));
        for (const TaskList& list : m_store->lists()) if (list.id == noteList) QVERIFY(list.folderId.isEmpty());
        bool listStillVisible = false;
        for (int row = 0; row < navigation->count(); ++row)
            if (navigation->item(row)->data(Qt::UserRole + 1).toString() == noteList) listStillVisible = true;
        QVERIFY(listStillVisible);
    }
    void reminderPopupSnoozeUsesExactReminderUuid() {
        Entity task;
        task.id = newUuid();
        task.title = QStringLiteral("多提醒任务");
        TaskReminder first;
        first.id = newUuid();
        first.entityId = task.id;
        first.triggerAt = fromUtcMs(1000);
        TaskReminder second = first;
        second.id = newUuid();
        second.triggerAt = fromUtcMs(2000);
        task.reminders << first << second;

        ReminderPopup popup(task, second.id);
        QSignalSpy snoozed(&popup, &ReminderPopup::snoozeRequested);
        QPushButton* snooze = nullptr;
        for (QPushButton* button : popup.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("稍后提醒"))
                snooze = button;
        QVERIFY(snooze);
        QTest::mouseClick(snooze, Qt::LeftButton);
        QCOMPARE(snoozed.count(), 1);
        const QList<QVariant> args = snoozed.takeFirst();
        QCOMPARE(args.size(), 3);
        QCOMPARE(args.at(0).toString(), task.id);
        QCOMPARE(args.at(1).toString(), second.id);
        QVERIFY(args.at(1).toString() != first.id);
        QCOMPARE(args.at(2).toInt(), 10);
    }
    void desktopProjectionUsesSameTaskAndCloseOnlyCloses() {
        const QString id = m_store->createTask(QStringLiteral("桌面任务"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("原有说明");
        task.dueAt = appNow().addDays(1);
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        DesktopNote other;
        other.entityId = id;
        other.deviceId = newUuid();
        other.geometry = QRect(900, 700, 300, 280);
        other.color = NoteColor::Pink;
        other.open = true;
        QVERIFY(m_store->saveDesktopNote(other));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll());
        NoteWidget* note = notes.note(id);
        QVERIFY(note);
        note->focusEditor();
        auto* editor = note->findChild<MarkdownEditor*>();
        QVERIFY(editor);
        const DesktopNote beforeTextFlush = m_store->desktopNote(id);
        editor->textEditor()->setPlainText(QStringLiteral("**修改后的说明**"));
        QVERIFY(note->flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("**修改后的说明**"));
        QCOMPARE(m_store->entity(id).dueAt, task.dueAt);
        const DesktopNote savedDesktop = m_store->desktopNote(id);
        QVERIFY(!savedDesktop.id.isEmpty());
        QCOMPARE(savedDesktop.deviceId, m_store->deviceId());
        QCOMPARE(savedDesktop.updatedAt, beforeTextFlush.updatedAt);
        QVERIFY(!savedDesktop.geometry.isValid()); // 文本 flush 不触碰桌面状态
        QVERIFY(savedDesktop.open);
        QCOMPARE(m_store->desktopNote(id, other.deviceId).geometry, other.geometry);
        QCOMPARE(m_store->desktopNote(id, other.deviceId).color, NoteColor::Pink);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!m_store->desktopNote(id).id.isEmpty());
        QVERIFY(!m_store->desktopNote(id).open);
        QCOMPARE(m_store->desktopNote(id).geometry, note->geometry());
        QVERIFY(!m_store->entity(id).deleted);
        QVERIFY(m_store->desktopNote(id, other.deviceId).open);
        QCOMPARE(notes.count(), 0);
    }
    void convertedNoteClosesAndRestoresDesktopProjection() {
        const QString id = m_store->createTask(QStringLiteral("转换桌面实体"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Green, QRect(30, 40, 320, 300),
                                          1, NoteLayer::Normal, true));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll()); QCOMPARE(notes.count(), 1);
        QVERIFY(m_store->convertKind(id, EntityKind::Note));
        QCOMPARE(notes.count(), 0); QVERIFY(m_store->desktopNote(id).open);
        QVERIFY(m_store->convertKind(id, EntityKind::Task));
        QCOMPARE(notes.count(), 1); QVERIFY(m_store->desktopNote(id).open);
        QVERIFY(m_store->trashTask(id)); QCOMPARE(notes.count(), 0);
    }
    void abandonedTaskLeavesActiveViewsAndDesktop() {
        const QString id = m_store->createTask(QStringLiteral("已放弃任务"));
        QVERIFY(isUuid(id));
        Entity task = m_store->entity(id);
        task.dueAt = appNow().addDays(-1);
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 20, 340, 360),
                                          0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll());
        QCOMPARE(notes.count(), 1);
        QVERIFY(m_store->abandonTask(id));
        QCOMPARE(notes.count(), 0);

        ViewSpec filter;
        QVERIFY(!contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        filter.scope = ViewScope::Completed;
        QVERIFY(!contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        filter.scope = ViewScope::Abandoned;
        QVERIFY(contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        TaskListModel model(m_store.get());
        model.setFilter(filter);
        const QModelIndex index = model.indexForTask(id);
        QVERIFY(index.isValid());
        QVERIFY(!model.data(index, TaskListModel::OverdueRole).toBool());
    }

    void noteResizeAndAttachmentPreview() {
        const QString id = m_store->createTask(QStringLiteral("带图片的桌面便签"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("## 参考材料\n\n可在这里查看图片与文档。");
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        const QString source = m_dir->path() + QStringLiteral("/sample.png");
        QPixmap image(160, 90);
        image.fill(QColor(QStringLiteral("#668BE3")));
        QVERIFY(image.save(source));
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        QCOMPARE(m_store->attachments(id).size(), 1);
        NoteWidget note(m_store->entity(id));
        note.setStore(m_store.get());
        note.show();
        auto* attachments = note.findChild<AttachmentWidget*>();
        QVERIFY(attachments);
        QVERIFY(attachments->isVisible());
        const auto* thumbnail = note.findChild<AttachmentWidget*>()
                                    ->findChild<QLabel*>(QStringLiteral("AttachmentThumbnail"));
        QVERIFY(thumbnail);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QVERIFY(!thumbnail->pixmap(Qt::ReturnByValue).isNull());
#else
        QVERIFY(thumbnail->pixmap() && !thumbnail->pixmap()->isNull());
#endif
        auto* grip = note.findChild<ResizeGrip*>();
        QVERIFY(grip);
        const QPoint start = grip->mapToGlobal(QPoint(8, 8));
        QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        for (int step = 1; step <= 12; ++step) {
            const QPoint local(8 + step * 4, 8 + step * 2);
            QMouseEvent move(QEvent::MouseMove,
                             local,
                             grip->mapToGlobal(local),
                             Qt::NoButton,
                             Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(grip, &move);
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, QPoint(56, 32));
        QVERIFY(note.width() > DefaultWidth);
        QVERIFY(note.grab().toImage().format() != QImage::Format_Invalid);
    }

    void applicationRestoreCoordinatesEditorsAndReminders() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString id = m_store->createTask(QStringLiteral("应用级恢复"));
        const TaskReminder past = m_store->addReminder(id, now.addSecs(-3600));
        const TaskReminder future = m_store->addReminder(id, now.addSecs(3600));
        QVERIFY(!past.id.isEmpty() && !future.id.isEmpty());
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 30, 360, 320), 0, NoteLayer::Normal, true));
        const QString backup = m_dir->path() + QStringLiteral("/app-restore.klb"); QVERIFY(m_store->exportBackup(backup));
        const QString sentinel = m_store->createTask(QStringLiteral("应用恢复覆盖哨兵")); QVERIFY(!sentinel.isEmpty());

        KylinListApp app; QString error; QVERIFY2(app.init(&error, true), qPrintable(error));
        auto* notes = app.findChild<NoteManager*>(); auto* scheduler = app.findChild<ReminderScheduler*>();
        QVERIFY(notes && scheduler); QCOMPARE(notes->count(), 1);
        QSignalSpy triggered(scheduler, &ReminderScheduler::reminderTriggered);
        QVERIFY2(app.importBackup(backup), qPrintable(error));
        QCOMPARE(notes->count(), 0); // 旧桌面编辑器被销毁，不以旧实体 flush 新库
        {
            TaskStore restored(Database::instance().connection()); QVERIFY(restored.load());
            QCOMPARE(restored.entity(id).id, id); QVERIFY(restored.entity(sentinel).id.isEmpty());
            QVERIFY(restored.desktopNote(id).id.isEmpty());
            QVERIFY(restored.isReminderDelivered(past.id)); QVERIFY(!restored.isReminderDelivered(future.id));
        }
        QTest::qWait(600); QCOMPARE(triggered.count(), 0); // 启动调度器不补发已恢复的历史提醒
        scheduler->checkDue(now.addSecs(7200)); QCOMPARE(triggered.count(), 1);
        QCOMPARE(triggered.takeFirst().at(1).toString(), future.id);
        m_store.reset(); app.shutdown();
    }

    void newChecklistItemKeepsIdentityForDoneEvent() {
        const QString id = m_store->createTask(QStringLiteral("新清单项日志"));
        TaskDetailWidget detail(m_store.get()); QVERIFY(detail.setTask(id));
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(input && checklist);
        QPushButton* add = nullptr;
        for (QPushButton* button : detail.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加")) add = button;
        QVERIFY(add);
        QTest::keyClicks(input, QStringLiteral("new item")); QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(checklist->count(), 1); QVERIFY(detail.flush());
        const QString itemId = m_store->entity(id).checklist.first().id;
        QVERIFY(isUuid(itemId)); QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), itemId);

        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).checklist.size(), 1);
        QCOMPARE(m_store->entity(id).checklist.first().id, itemId);
        QVERIFY(m_store->entity(id).checklist.first().done);
        QVector<EntityEvent> doneEvents;
        for (const EntityEvent& event : m_store->events(id))
            if (event.field == QStringLiteral("checklist.%1.done").arg(itemId)) doneEvents.append(event);
        QCOMPARE(doneEvents.size(), 1);
        QCOMPARE(doneEvents.first().oldValue.toString(), QStringLiteral("0"));
        QCOMPARE(doneEvents.first().newValue.toString(), QStringLiteral("1"));
    }

    void detailActivitySessionBoundariesAndDialog() {
        const auto withField = [this](const QString& id, const QString& field) {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == field) result.append(event);
            return result;
        };
        const QString id = m_store->createTask(QStringLiteral("UI活动"));
        Entity initial = m_store->entity(id); ChecklistItem check; check.text = QStringLiteral("原始清单");
        initial.checklist.append(check); QVERIFY(m_store->saveTask(initial));
        const QString checkId = m_store->entity(id).checklist.first().id;
        TaskDetailWidget detail(m_store.get()); detail.resize(800, 700); detail.show(); QVERIFY(detail.setTask(id));
        QLineEdit focusSink(&detail); focusSink.show();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* body = detail.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(title && body && checklist); detail.activateWindow(); detail.focusTitle(); QApplication::processEvents(); QVERIFY(title->hasFocus());
        QTest::keyClicks(title, QStringLiteral("abcde")); QTest::qWait(520); // 450ms autosave 不结束会话
        QCOMPARE(m_store->entity(id).title, QStringLiteral("abcde")); QCOMPARE(withField(id, QStringLiteral("title")).size(), 0);
        focusSink.setFocus(); QApplication::processEvents(); QVERIFY(!title->hasFocus());
        QVector<EntityEvent> events = withField(id, QStringLiteral("title")); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), QStringLiteral("abcde")); QVERIFY(!events.first().oldValue.isValid());

        body->setFocus(); QApplication::processEvents(); QVERIFY(body->hasFocus());
        const QString fullBody = QStringLiteral("**完整正文**\n第二行\n第三行"); body->setPlainText(fullBody);
        QTest::qWait(520); QCOMPARE(m_store->entity(id).body, fullBody); QCOMPARE(withField(id, QStringLiteral("body")).size(), 0);
        focusSink.setFocus(); QApplication::processEvents(); events = withField(id, QStringLiteral("body"));
        QCOMPARE(events.size(), 1); QCOMPARE(events.first().newValue.toString(), fullBody);

        checklist->setFocus(); QApplication::processEvents(); checklist->item(0)->setText(QStringLiteral("失焦后的清单全文"));
        QVERIFY(detail.flush()); focusSink.setFocus(); QApplication::processEvents();
        events = withField(id, QStringLiteral("checklist.%1.text").arg(checkId)); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), QStringLiteral("失焦后的清单全文"));
        QCOMPARE(m_store->entity(id).checklist.first().id, checkId);

        const QString other = m_store->createTask(QStringLiteral("切换目标"));
        title->setFocus(); QApplication::processEvents(); QTest::keyClick(title, Qt::Key_X);
        const int beforeSwitch = withField(id, QStringLiteral("title")).size();
        QVERIFY(detail.setTask(other)); QCOMPARE(withField(id, QStringLiteral("title")).size(), beforeSwitch + 1); // 切换实体前提交
        QCOMPARE(detail.entityId(), other); QVERIFY(detail.setTask(QString()));

        Entity scalars = m_store->entity(id); scalars.priority = 3;
        scalars.tags = QStringList{QStringLiteral("活动"), QStringLiteral("测试")}; QVERIFY(m_store->saveTask(scalars));
        QVERIFY(m_store->convertKind(id, EntityKind::Note));
        ActivityLogDialog dialog(m_store.get(), id);
        auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ActivityTree"));
        auto* text = dialog.findChild<QTextEdit*>(QStringLiteral("ActivityDetail")); QVERIFY(tree && text);
        QSet<QString> batches; QTreeWidgetItem* bodyItem = nullptr; QTreeWidgetItem* kindItem = nullptr; bool foldedBatch = false;
        for (const EntityEvent& event : m_store->events(id)) batches.insert(event.batchId);
        QCOMPARE(tree->topLevelItemCount(), batches.size());
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* batch = tree->topLevelItem(i); QVERIFY(!batch->isExpanded()); if (batch->childCount() > 1) foldedBatch = true;
            for (int j = 0; j < batch->childCount(); ++j) {
                QTreeWidgetItem* item = batch->child(j);
                if (item->text(2) == QStringLiteral("body") && item->text(4) == fullBody) bodyItem = item;
                if (item->text(1) == QStringLiteral("类型转换")) kindItem = item;
            }
        }
        QVERIFY(foldedBatch); QVERIFY(bodyItem && kindItem);
        tree->setCurrentItem(bodyItem); QApplication::processEvents(); QVERIFY(text->toPlainText().contains(fullBody));
        QVERIFY(kindItem->text(4).contains(QStringLiteral("字段已保留")));
        tree->setCurrentItem(kindItem); QApplication::processEvents(); QVERIFY(text->toPlainText().contains(QStringLiteral("字段已保留")));
        const int openBatchCount = tree->topLevelItemCount(); scalars = m_store->entity(id); scalars.title = QStringLiteral("打开后新修改");
        QVERIFY(m_store->saveTask(scalars)); QCOMPARE(tree->topLevelItemCount(), openBatchCount); // 已打开对话框不主动轮询
    }

    void noteActivityAutosaveFlushAndClose() {
        const auto bodyEvents = [this](const QString& id) {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == QStringLiteral("body")) result.append(event);
            return result;
        };
        const QString id = m_store->createTask(QStringLiteral("便签活动"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(30, 40, 360, 320), 0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get()); QVERIFY(notes.restoreAll()); NoteWidget* note = notes.note(id); QVERIFY(note);
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(editor); note->focusEditor(); QApplication::processEvents();
        const QString autosaved = QStringLiteral("自动保存中的完整正文\n不会在300ms时写日志");
        editor->textEditor()->setPlainText(autosaved); QTest::qWait(380);
        QCOMPARE(m_store->entity(id).body, autosaved); QCOMPARE(bodyEvents(id).size(), 0);
        QVERIFY(notes.flushAll()); QVector<EntityEvent> events = bodyEvents(id); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), autosaved); QVERIFY(!events.first().oldValue.isValid());
        QVERIFY(m_store->desktopNote(id).open); QCOMPARE(notes.count(), 1);

        QLineEdit focusSink; focusSink.show(); focusSink.setFocus(); QApplication::processEvents(); note->focusEditor(); QApplication::processEvents();
        const QString finalText = QStringLiteral("关闭便签前的最后一条全文"); editor->textEditor()->setPlainText(finalText);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton")); QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton); QApplication::processEvents();
        events = bodyEvents(id); QCOMPARE(events.size(), 2); QCOMPARE(events.last().newValue.toString(), finalText);
        QCOMPARE(m_store->entity(id).body, finalText); QVERIFY(!m_store->desktopNote(id).open); // 关闭路径 flush 后结束会话，不丢最后修改
    }

    void visualPreview() {
        const QString group = m_store->createGroup(QStringLiteral("工作安排"));
        const QDate today = toAppTime(appNow()).date();
        Entity task;
        const auto id = m_store->createTask(QStringLiteral("完成本周项目复盘"), group);
        task = m_store->entity(id);
        task.startAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        task.dueAt = fromAppTime(QDateTime(today, QTime(17, 30)));
        task.reminderAt = task.dueAt.addSecs(-600);
        task.priority = 1;
        task.body = QStringLiteral(
            "## 本周要点\n\n整理 **完成情况**，记录需要协调的事项。\n\n- 汇总本周进展\n- 确认下周安排\n\n> "
            "把下一步写清楚，让事情更容易推进。");
        ChecklistItem one;
        one.text = QStringLiteral("汇总材料");
        one.done = true;
        task.checklist << one;
        one.text = QStringLiteral("核对时间表");
        one.done = false;
        task.checklist << one;
        task.tags << QStringLiteral("本周") << QStringLiteral("协作");
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        for (const auto& title : {QStringLiteral("预约下周会议室"),
                                  QStringLiteral("整理个人学习笔记"),
                                  QStringLiteral("确认下周工作清单")})
            QVERIFY(!m_store->createTask(title).isEmpty());
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.showTask(id);
        QTest::qWait(100);
        const QString dir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel.png")));
            NoteWidget note(task);
            note.show();
            QTest::qWait(50);
            QVERIFY(note.grab().save(dir + QStringLiteral("/desktop-note.png")));
        }
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};
QTEST_MAIN(UiTest)
#include "tst_ui.moc"
