#include <QApplication>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QToolButton>
#include <QtTest>
#include <memory>
#include "app/NoteManager.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "ui/AppStyle.h"
#include "ui/AttachmentWidget.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/NoteChecklist.h"
#include "ui/NoteWidget.h"
#include "ui/ResizeGrip.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskFilter.h"
#include "ui/TaskListModel.h"
#include "ui/TaskPanel.h"
using namespace kn;

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINNOTE_DATA_DIR", m_dir->path().toUtf8());
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, m_dir->path() + QStringLiteral("/settings"));
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/notes.db"), &error),
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
        editor.setContent(QStringLiteral("hello"), ContentFormat::Markdown);
        editor.textEditor()->selectAll();
        auto* bold = editor.findChild<QToolButton*>(QStringLiteral("format0"));
        QVERIFY(bold);
        QTest::mouseClick(bold, Qt::LeftButton);
        QCOMPARE(editor.content(), QStringLiteral("**hello**"));
        editor.textEditor()->undo();
        QCOMPARE(editor.content(), QStringLiteral("hello"));
    }
    void filtersUseLocalCalendarAndSearch() {
        const QDateTime now(QDate(2026, 11, 6), QTime(12, 0), Qt::LocalTime);
        Task task;
        task.title = QStringLiteral("整理材料");
        task.startAt = now.addDays(-1);
        task.dueAt = now.addDays(1);
        task.tags << QStringLiteral("工作");
        TaskFilter filter;
        filter.view = TaskView::Today;
        QVERIFY(filter.matches(task, now));
        filter.search = QStringLiteral("材料 工作");
        QVERIFY(filter.matches(task, now));
        filter.search = QStringLiteral("其他");
        QVERIFY(!filter.matches(task, now));
        filter.search.clear();
        task.completed = true;
        QVERIFY(!filter.matches(task, now));
        filter.view = TaskView::Completed;
        QVERIFY(filter.matches(task, now));
        task.deleted = true;
        QVERIFY(!filter.matches(task, now));
        filter.view = TaskView::Trash;
        QVERIFY(filter.matches(task, now));
    }
    void quickAddAndDetailPersistWithoutLosingPresentation() {
        TaskPanel panel(m_store.get());
        panel.show();
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(quick);
        quick->setText(QStringLiteral("明天提交材料"));
        QTest::keyClick(quick, Qt::Key_Return);
        QCOMPARE(m_store->tasks().size(), 1);
        const qint64 id = m_store->tasks().first().id;
        auto* detail = panel.findChild<TaskDetailWidget*>();
        QVERIFY(detail);
        QCOMPARE(detail->taskId(), id);
        auto* description = detail->findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        description->setPlainText(QStringLiteral("**核对附件**"));
        Task external = m_store->task(id);
        external.geometry = QRect(45, 60, 340, 360);
        external.color = NoteColor::Blue;
        QVERIFY(m_store->saveTask(external));
        QVERIFY(detail->flush());
        QCOMPARE(m_store->task(id).description, QStringLiteral("**核对附件**"));
        QCOMPARE(m_store->task(id).geometry, external.geometry);
        QCOMPARE(m_store->task(id).color, NoteColor::Blue);
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
        QCOMPARE(m_store->task(id).dueAt, start->dateTime().addSecs(3600).toUTC());
        due->setDateTime(start->dateTime().addSecs(7200));
        QVERIFY(detail->flush());
        auto* list = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        QVERIFY(list);
        auto* model = qobject_cast<TaskListModel*>(list->model());
        QVERIFY(model);
        const auto index = model->indexForTask(id);
        QVERIFY(index.isValid());
        QVERIFY(model->setData(index, Qt::Checked, Qt::CheckStateRole));
        QVERIFY(m_store->task(id).completed);
    }
    void desktopProjectionUsesSameTaskAndCloseOnlyUnpins() {
        const qint64 id = m_store->createTask(QStringLiteral("桌面任务"));
        Task task = m_store->task(id);
        task.pinned = true;
        task.description = QStringLiteral("原有说明");
        task.dueAt = QDateTime::currentDateTime().addDays(1);
        QVERIFY(m_store->saveTask(task));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll());
        NoteWidget* note = notes.note(id);
        QVERIFY(note);
        note->focusEditor();
        auto* editor = note->findChild<MarkdownEditor*>();
        QVERIFY(editor);
        editor->textEditor()->setPlainText(QStringLiteral("**修改后的说明**"));
        QVERIFY(note->flush());
        QCOMPARE(m_store->task(id).description, QStringLiteral("**修改后的说明**"));
        QCOMPARE(m_store->task(id).dueAt, task.dueAt);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!m_store->task(id).pinned);
        QVERIFY(!m_store->task(id).deleted);
        QCOMPARE(notes.count(), 0);
    }
    void noteResizeAndAttachmentPreview() {
        const qint64 id = m_store->createTask(QStringLiteral("带图片的桌面便签"));
        Task task = m_store->task(id);
        task.description = QStringLiteral("## 参考材料\n\n可在这里查看图片与文档。");
        task.pinned = true;
        QVERIFY(m_store->saveTask(task));
        const QString source = m_dir->path() + QStringLiteral("/sample.png");
        QPixmap image(160, 90);
        image.fill(QColor(QStringLiteral("#668BE3")));
        QVERIFY(image.save(source));
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        QCOMPARE(m_store->attachments(id).size(), 1);
        NoteWidget note(m_store->task(id));
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

    void visualPreview() {
        const qint64 group = m_store->createGroup(QStringLiteral("工作安排"));
        const auto now = QDateTime::currentDateTime();
        Task task;
        const auto id = m_store->createTask(QStringLiteral("完成本周项目复盘"), group);
        task = m_store->task(id);
        task.startAt = QDateTime(now.date(), QTime(9, 0));
        task.dueAt = QDateTime(now.date(), QTime(17, 30));
        task.reminderAt = task.dueAt.addSecs(-600);
        task.priority = 1;
        task.favorite = true;
        task.pinned = true;
        task.description = QStringLiteral(
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
        for (const auto& title : {QStringLiteral("预约下周会议室"),
                                  QStringLiteral("整理个人学习笔记"),
                                  QStringLiteral("确认下周工作清单")})
            QVERIFY(m_store->createTask(title));
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.showTask(id);
        QTest::qWait(100);
        const QString dir = qEnvironmentVariable("KYLINNOTE_SCREENSHOT_DIR");
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
