#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QSqlQuery>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QtTest>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include "app/NoteManager.h"
#include "app/KylinListApp.h"
#include "app/ReminderScheduler.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "domain/ViewSpec.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/ActivityLogDialog.h"
#include "ui/EntityHoverPreview.h"
#include "ui/EntityPicker.h"
#include "ui/AttachmentWidget.h"
#include "ui/DetailPopups.h"
#include "ui/DatePickerPopup.h"
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
#include "ui/ToastIsland.h"
#include "ui/WindowChrome.h"
#include "ui/design/TypeSystem.h"
using namespace kl;

namespace {
QRect rectIn(const QWidget* widget, const QWidget* ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint()), widget->size());
}

QString referenceImage(const QString& directory) {
    QImage image(320, 160, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#F1F5FA")));
    QPainter painter(&image);
    painter.fillRect(QRect(16, 18, 88, 124), QColor(QStringLiteral("#5374BD")));
    painter.fillRect(QRect(120, 24, 174, 14), QColor(QStringLiteral("#B9C9E4")));
    painter.fillRect(QRect(120, 52, 140, 10), QColor(QStringLiteral("#D1DCEB")));
    painter.fillRect(QRect(120, 74, 160, 10), QColor(QStringLiteral("#D1DCEB")));
    painter.setPen(QColor(QStringLiteral("#435675")));
    painter.drawText(QRect(120, 104, 180, 24), Qt::AlignLeft, QStringLiteral("LOCAL SAMPLE"));
    painter.end();
    const QString path = directory + QStringLiteral("/reference.png");
    return image.save(path) ? path : QString();
}

bool nativePopupShadowDisabled(QWidget* popup) {
#ifdef Q_OS_WIN
    if (QApplication::platformName() == QLatin1String("windows")) {
        const HWND handle = reinterpret_cast<HWND>(popup->winId());
        return IsWindow(handle) && !(GetClassLongPtrW(handle, GCL_STYLE) & CS_DROPSHADOW);
    }
#else
    Q_UNUSED(popup);
#endif
    return true;
}

bool savePopupPreview(QWidget* detail, QWidget* popup, const QString& path) {
    const QRect detailBounds(detail->mapToGlobal(QPoint()), detail->size());
    const QRect popupBounds(popup->mapToGlobal(QPoint()), popup->size());
    const QRect bounds = detailBounds.united(popupBounds);
    QImage image(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(detail->palette().window().color());
    QPainter painter(&image);
    painter.drawPixmap(detailBounds.topLeft() - bounds.topLeft(), detail->grab());
    painter.drawPixmap(popupBounds.topLeft() - bounds.topLeft(), popup->grab());
    painter.end();
    return image.save(path);
}

// The detail menu may use exec() or popup(). Drive its real QAction in either case,
// and close the menu on a missing action so a failed assertion cannot hang the suite.
bool triggerDetailCommand(TaskDetailWidget& detail, const QString& actionName) {
    auto* button = detail.findChild<QToolButton*>(QStringLiteral("DetailMore"));
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    bool settled = false, triggered = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        auto* menu = detail.findChild<QMenu*>(QStringLiteral("DetailMoreMenu"));
        if (menu && menu->isVisible()) {
            auto* action = menu->findChild<QAction*>(actionName);
            settled = true;
            triggered = action && action->isVisible() && action->isEnabled();
            poll.stop();
            menu->close();
            if (triggered) action->trigger();
        } else if (elapsed.elapsed() > 1000) {
            settled = true;
            poll.stop();
            if (auto* active = qobject_cast<QMenu*>(QApplication::activePopupWidget())) active->close();
        }
    });
    poll.start();
    QTest::mouseClick(button, Qt::LeftButton);
    while (!settled && elapsed.elapsed() < 1200) QTest::qWait(10);
    return triggered;
}
} // namespace

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
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
    void markdownInlineExtensionsRender_data() {
        QTest::addColumn<QString>("color");
        QTest::newRow("underline") << QString();
        for (const QString& color : {QStringLiteral("yellow"), QStringLiteral("green"),
                                      QStringLiteral("blue"), QStringLiteral("pink"), QStringLiteral("purple")})
            QTest::newRow(qPrintable(color)) << color;
    }
    void markdownInlineExtensionsRender() {
        QFETCH(QString, color);
        const QString opening = color.isEmpty() ? QStringLiteral("<u>")
            : QStringLiteral("<mark data-color=\"%1\">").arg(color);
        const QString closing = color.isEmpty() ? QStringLiteral("</u>") : QStringLiteral("</mark>");
        const QString source = QStringLiteral("prefix ") + opening + QStringLiteral("formatted")
            + closing + QStringLiteral(" suffix");
        MarkdownEditor editor;
        editor.setContent(source);
        QCOMPARE(editor.content(), source);
        QCOMPARE(editor.plainTextContent(), QStringLiteral("prefix formatted suffix"));
        for (QTextDocument* document : {editor.textEditor()->document(), editor.preview()->document()}) {
            QCOMPARE(document->toPlainText(), QStringLiteral("prefix formatted suffix"));
            const QTextCursor styled = document->find(QStringLiteral("formatted"));
            QVERIFY(!styled.isNull());
            const QTextCharFormat format = styled.charFormat();
            if (color.isEmpty()) {
                QVERIFY(format.fontUnderline());
            } else {
                QVERIFY(format.background().style() != Qt::NoBrush);
                QCOMPARE(format.background().color(), Markdown::highlightColor(color));
            }
            const QTextCursor ordinary = document->find(QStringLiteral("prefix"));
            QVERIFY(!ordinary.isNull());
            QVERIFY(!ordinary.charFormat().fontUnderline());
            QVERIFY(ordinary.charFormat().background().style() == Qt::NoBrush);
        }
    }
    void markdownInlineExtensionsRejectOtherHtml_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("literal");
        const QStringList unsafe{
            QStringLiteral("<script>alert(1)</script>"),
            QStringLiteral("<img src=\"https://example.com/image.png\" onerror=\"bad()\">"),
            QStringLiteral("<a href=\"javascript:bad()\">literal</a>"),
            QStringLiteral("<mark data-color=\"yellow\" onclick=\"bad()\">literal</mark>"),
            QStringLiteral("<mark data-color=\"orange\">literal</mark>"),
            QStringLiteral("<u style=\"color:red\" onclick=\"bad()\">literal</u>")};
        for (int index = 0; index < unsafe.size(); ++index)
            QTest::newRow(qPrintable(QStringLiteral("untrusted-%1").arg(index))) << unsafe.at(index) << unsafe.at(index);
        const QString code = QStringLiteral("<mark data-color=\"yellow\">literal</mark> <u>code</u>");
        QTest::newRow("inline-code") << (QLatin1Char('`') + code + QLatin1Char('`')) << code;
        QTest::newRow("fenced-code") << (QStringLiteral("```html\n") + code + QStringLiteral("\n```")) << code;
    }
    void markdownInlineExtensionsRejectOtherHtml() {
        QFETCH(QString, source);
        QFETCH(QString, literal);
        const QString html = Markdown::toHtml(source);
        QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!html.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
        QTextDocument preview;
        preview.setHtml(html);
        QCOMPARE(preview.toPlainText().trimmed(), literal);
        MarkdownEditor editor;
        editor.setContent(source);
        QCOMPARE(editor.plainTextContent().trimmed(), literal);
        QCOMPARE(editor.content(), source);
        for (QTextDocument* document : {&preview, editor.textEditor()->document()}) {
            for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
                for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat format = it.fragment().charFormat();
                    QVERIFY(!format.isImageFormat());
                    QVERIFY(!format.isAnchor());
                    QVERIFY(!format.fontUnderline());
                }
            }
        }
    }
    void markdownNewFormatsPersistAndUndo_data() {
        markdownInlineExtensionsRender_data();
    }
    void markdownNewFormatsPersistAndUndo() {
        QFETCH(QString, color);
        const QString id = m_store->createTask(QStringLiteral("格式持久化与撤销"));
        Entity task = m_store->entity(id); task.body = QStringLiteral("hello"); QVERIFY(m_store->saveTask(task));
        const auto bodyEventCount = [this, &id]() {
            int count = 0;
            for (const EntityEvent& event : m_store->events(id)) if (event.field == QStringLiteral("body")) ++count;
            return count;
        };
        const int beforeEvents = bodyEventCount();
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor);
        auto* body = editor->textEditor(); detail.activateWindow(); body->setFocus(); QApplication::processEvents();
        QVERIFY(body->hasFocus()); body->selectAll();
        const QString expected = color.isEmpty() ? QStringLiteral("<u>hello</u>")
            : QStringLiteral("<mark data-color=\"%1\">hello</mark>").arg(color);
        if (color.isEmpty()) editor->applyFormat(MarkdownEditor::FormatAction::Underline);
        else editor->applyHighlight(color);
        QCOMPARE(editor->content(), expected); QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, expected);
        QCOMPARE(bodyEventCount(), beforeEvents); // Explicit save keeps the active editing session open.
        body->undo(); QCOMPARE(editor->content(), QStringLiteral("hello"));
        QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, QStringLiteral("hello"));
        body->redo(); QCOMPARE(editor->content(), expected);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(id).body, expected);
        QCOMPARE(bodyEventCount(), beforeEvents + 1);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), expected); QCOMPARE(editor->plainTextContent(), QStringLiteral("hello"));
        for (QTextDocument* document : {body->document(), editor->preview()->document()}) {
            QCOMPARE(document->toPlainText(), QStringLiteral("hello"));
            const QTextCursor rendered = document->find(QStringLiteral("hello")); QVERIFY(!rendered.isNull());
            if (color.isEmpty()) QVERIFY(rendered.charFormat().fontUnderline());
            else QCOMPARE(rendered.charFormat().background().color(), Markdown::highlightColor(color));
        }
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, expected);
    }
    void markdownBlocksKeepIndependentFonts() {
        const QString source = QStringLiteral("## Heading\nPlain paragraph\n\n*italic*\nAnother paragraph\n\n```cpp\nint value;\n```\nLast paragraph");
        MarkdownEditor editor;
        editor.setContent(source);
        const QFont bodyFont = ds::qfont(ds::FontScene::Body);
        const auto verifyParagraphs = [&]() {
            for (const QString& text : {QStringLiteral("Plain paragraph"), QStringLiteral("Another paragraph"), QStringLiteral("Last paragraph")}) {
                const QTextCursor cursor = editor.textEditor()->document()->find(text);
                QVERIFY(!cursor.isNull());
                const QTextCharFormat format = cursor.charFormat();
                QCOMPARE(format.fontWeight(), int(QFont::Normal));
                QVERIFY(!format.fontItalic());
                const qreal size = format.fontPointSize() > 0 ? format.fontPointSize()
                    : editor.textEditor()->document()->defaultFont().pointSizeF();
                QCOMPARE(size, bodyFont.pointSizeF());
            }
        };
        verifyParagraphs();
        QCOMPARE(editor.content(), source);
        editor.setFlowLayout(true);
        editor.resize(360, 500);
        editor.show(); QApplication::processEvents();
        verifyParagraphs();
        QCOMPARE(editor.content(), source);
    }
    void markdownRenderedReselectionPreservesLiteralSource() {
        const QString literal = QStringLiteral("A&B <literal> \"quote\" ") + QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString yellow = QStringLiteral("<mark data-color=\"yellow\">") + literal + QStringLiteral("</mark>");
        const QString blue = QStringLiteral("<mark data-color=\"blue\">") + literal + QStringLiteral("</mark>");
        const QString underlined = QStringLiteral("<mark data-color=\"blue\"><u>") + literal + QStringLiteral("</u></mark>");
        const QString id = m_store->createTask(QStringLiteral("格式切换保留字面内容"));
        Entity task = m_store->entity(id); task.body = literal; QVERIFY(m_store->saveTask(task));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(editor && title);
        auto* body = editor->textEditor(); detail.activateWindow(); body->setFocus(); QApplication::processEvents();
        QVERIFY(body->hasFocus()); body->selectAll(); editor->applyHighlight(QStringLiteral("yellow"));
        QCOMPARE(editor->content(), yellow);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), yellow); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QCOMPARE(editor->preview()->document()->toPlainText(), literal);

        // Select the displayed characters after rendering, rather than selecting source markup.
        QTextCursor selection = body->document()->find(literal); QVERIFY(!selection.isNull());
        QCOMPARE(selection.selectedText(), literal);
        QCOMPARE(selection.charFormat().background().color(), Markdown::highlightColor(QStringLiteral("yellow")));
        body->setTextCursor(selection); editor->applyHighlight(QStringLiteral("blue"));
        QCOMPARE(editor->content(), blue); // Recolor replaces the old mark, retaining every literal character.
        body->undo(); QCOMPARE(editor->content(), yellow);
        body->redo(); QCOMPARE(editor->content(), blue);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), blue); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QCOMPARE(editor->preview()->document()->toPlainText(), literal);

        selection = body->document()->find(literal); QVERIFY(!selection.isNull());
        QCOMPARE(selection.selectedText(), literal); body->setTextCursor(selection);
        editor->applyFormat(MarkdownEditor::FormatAction::Underline);
        QCOMPARE(editor->content(), underlined);
        body->undo(); QCOMPARE(editor->content(), blue);
        body->redo(); QCOMPARE(editor->content(), underlined);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), underlined); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(id).body, underlined);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), underlined); QCOMPARE(editor->plainTextContent(), literal);
        for (QTextDocument* document : {body->document(), editor->preview()->document()}) {
            QCOMPARE(document->toPlainText(), literal);
            const QTextCursor restored = document->find(literal); QVERIFY(!restored.isNull());
            QVERIFY(restored.charFormat().fontUnderline());
            QCOMPARE(restored.charFormat().background().color(), Markdown::highlightColor(QStringLiteral("blue")));
        }
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, underlined);
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
    void workspaceUsesFourColumnsAndNoPermanentStatusBar() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
        auto* navigationPane = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* detailPane = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* controls = panel.findChild<WindowControls*>();
        QVERIFY(rail && navigationPane && listPane && detailPane && controls);
        QVERIFY(panel.windowFlags() & Qt::FramelessWindowHint);
        QVERIFY(panel.windowFlags() & Qt::NoDropShadowWindowHint);
        QVERIFY(panel.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(panel.mask().isEmpty());
        auto* surface = panel.findChild<QWidget*>(QStringLiteral("WorkspaceRoot"));
        QVERIFY(surface && surface->mask().isEmpty());
        QCOMPARE(rectIn(rail, &panel).top(), 21);
        QCOMPARE(rectIn(navigationPane, &panel).top(), 21);
        QCOMPARE(rectIn(listPane, &panel).top(), 21);
        QVERIFY(!panel.findChild<QStatusBar*>());
        QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("QuickAddButton"))->isVisible());
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch"))->isVisible());
        QCOMPARE(rail->width(), 52);
        QCOMPARE(rectIn(rail, &panel).left(), 21);
        QCOMPARE(rectIn(navigationPane, &panel).left(), rectIn(rail, &panel).right() + 1);
        QCOMPARE(rectIn(listPane, &panel).left(), rectIn(navigationPane, &panel).right() + 2);
        QVERIFY(detailPane->isVisible());
        QCOMPARE(rectIn(detailPane, &panel).left(), rectIn(listPane, &panel).right() + 2);
        QCOMPARE(rectIn(detailPane, &panel).right(), panel.rect().right() - 21);
        QCOMPARE(rectIn(controls, &panel).top(), 21);
        QCOMPARE(rectIn(controls, &panel).right(), panel.rect().right() - 21);

        const QString id = m_store->createTask(QStringLiteral("展开第四列"));
        panel.showTask(id);
        QTRY_VERIFY(detailPane->isVisible());
        QCOMPARE(rectIn(detailPane, &panel).left(), rectIn(listPane, &panel).right() + 2);
        QCOMPARE(rectIn(detailPane, &panel).right(), panel.rect().right() - 21);
        auto* info = detailPane->findChild<QToolButton*>(QStringLiteral("DetailInfo"));
        QVERIFY(info);
        QVERIFY(rectIn(info, &panel).top() >= rectIn(controls, &panel).bottom());
        QTest::mouseClick(detailPane->findChild<QToolButton*>(QStringLiteral("DetailClose")), Qt::LeftButton);
        QTRY_VERIFY(detailPane->isVisible());
        QVERIFY(panel.findChild<TaskDetailWidget*>()->entityId().isEmpty());
        QCOMPARE(rectIn(listPane, &panel).right(), rectIn(detailPane, &panel).left() - 2);
        panel.showTask(id);
        QTRY_VERIFY(detailPane->isVisible());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(panel.windowState() & Qt::WindowMaximized);
        QVERIFY(panel.mask().isEmpty());
        QVERIFY(surface->mask().isEmpty());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!(panel.windowState() & Qt::WindowMaximized));
        QVERIFY(panel.mask().isEmpty());
    }

    void responsiveColumnsAndOverlayDetail() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
        auto* sidebar = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* list = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* splitter = panel.findChild<QSplitter*>(QStringLiteral("WorkspaceSplitter"));
        auto* toggle = panel.findChild<QToolButton*>(QStringLiteral("SidebarToggle"));
        QVERIFY(rail && sidebar && list && detail && splitter && toggle);
        QCOMPARE(splitter->count(), 3);
        QCOMPARE(splitter->handleWidth(), 1);
        QVERIFY(splitter->handle(1) && splitter->handle(2));
        QCOMPARE(rail->width(), 52);
        QCOMPARE(panel.minimumSize(), QSize(620, 560));
        splitter->setSizes({280, 500, 370});
        QApplication::processEvents();
        QCOMPARE(sidebar->width(), 280);
        QCOMPARE(detail->width(), 370);
        QCOMPARE(rail->width(), 52);

        panel.resize(900, 700);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        QVERIFY(list->isVisible());
        const QRect listBeforeOverlay = rectIn(list, &panel);
        const QString id = m_store->createTask(QStringLiteral("窄窗覆盖详情"));
        panel.showTask(id);
        QTRY_VERIFY(detail->isVisible());
        QCOMPARE(detail->width(), 300);
        QCOMPARE(detail->parentWidget(), panel.findChild<QWidget*>(QStringLiteral("WorkspaceRoot")));
        QCOMPARE(rectIn(list, &panel), listBeforeOverlay);
        QCOMPARE(rectIn(detail, &panel).right(), panel.rect().right() - 21);
        QVERIFY(rectIn(detail, &panel).left() > rectIn(list, &panel).left());
        auto* close = detail->findChild<QToolButton*>(QStringLiteral("DetailClose"));
        auto* reminder = detail->findChild<QToolButton*>(QStringLiteral("DetailReminder"));
        QVERIFY(close && reminder);
        QVERIFY(rectIn(close, &panel).left() >= rectIn(reminder, &panel).right());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!detail->isVisible());
        QCOMPARE(rectIn(list, &panel), listBeforeOverlay);

        panel.resize(700, 620);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        QVERIFY(list->isVisible());
        QCOMPARE(rail->width(), 52);
        panel.resize(900, 700);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        panel.resize(1240, 820);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(detail->isVisible());
        QCOMPARE(detail->parentWidget(), splitter);
        QCOMPARE(detail->width(), 370);

        QTest::mouseClick(toggle, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        panel.resize(700, 620);
        panel.resize(1240, 820);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
    }

    void toastIslandSupportsUndoAndHoverLifetime() {
        ToastIsland toast;
        toast.showMessage(QStringLiteral("普通提示"), 120);
        QVERIFY(toast.isVisible());
        QVERIFY(!toast.findChild<QToolButton*>(QStringLiteral("ToastAction"))->isVisible());
        QTest::qWait(180);
        QVERIFY(!toast.isVisible());

        int calls = 0;
        toast.showAction(QStringLiteral("任务已完成"), QStringLiteral("撤销"), [&calls]() {
            ++calls;
            return true;
        }, 180);
        auto* action = toast.findChild<QToolButton*>(QStringLiteral("ToastAction"));
        QVERIFY(action && action->isVisible());
        QTest::qWait(60);
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(&toast, &enter);
        const int paused = toast.remainingTime();
        QTest::qWait(230);
        QVERIFY(toast.isVisible());
        QVERIFY(toast.remainingTime() <= paused);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&toast, &leave);
        QTest::qWait(70);
        QVERIFY(toast.isVisible());
        QTest::mouseClick(action, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QVERIFY(!toast.isVisible());
        toast.showMessage(QStringLiteral("再次提示"), 100);
        QTest::qWait(150);
        QVERIFY(!toast.isVisible());
    }

    void taskStatusToastRestoresPreviousState() {
        const QString id = m_store->createTask(QStringLiteral("可撤销的任务状态"));
        TaskPanel panel(m_store.get());
        panel.showTask(id);
        auto* toast = panel.findChild<ToastIsland*>(QStringLiteral("WorkspaceToast"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        QVERIFY(toast && listPane);
        QVERIFY(m_store->setCompleted(id, true));
        QTRY_VERIFY(toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("任务已完成"));
        QVERIFY(toast->parentWidget() == listPane);
        QVERIFY(rectIn(toast, listPane).bottom() < listPane->rect().bottom());
        QTest::mouseClick(toast->findChild<QToolButton*>(QStringLiteral("ToastAction")), Qt::LeftButton);
        QTRY_COMPARE(m_store->entity(id).status, TaskStatus::Active);
        QVERIFY(!m_store->entity(id).completed);
        QVERIFY(m_store->abandonTask(id));
        QTRY_VERIFY(toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("任务已放弃"));
        QTest::mouseClick(toast->findChild<QToolButton*>(QStringLiteral("ToastAction")), Qt::LeftButton);
        QTRY_COMPARE(m_store->entity(id).status, TaskStatus::Active);
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

    void detailShortBodyKeepsChecklistAndAttachmentsAdjacent() {
        const QString id = m_store->createTask(QStringLiteral("紧凑详情"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("A short body.");
        ChecklistItem item; item.text = QStringLiteral("First item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        const Attachment attachment = m_store->importAttachment(id, source); QVERIFY(isUuid(attachment.id));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 720); detail.show(); QVERIFY(detail.setTask(id));
        QTest::qWait(20); // Settle document wrapping and deferred attachment-row replacement before measuring geometry.
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        QVERIFY(scroll && form && title && editor && checklist && attachments && input);
        QCOMPARE(scroll->widget(), form);
        QVERIFY(form->isAncestorOf(title)); QVERIFY(form->isAncestorOf(editor));
        QTRY_VERIFY(checklist->isVisible()); QTRY_VERIFY(attachments->isVisible());
        QVERIFY(input->isHidden());
        QCOMPARE(checklist->frameShape(), QFrame::NoFrame);
        QTRY_COMPARE(checklist->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        const QTextCursor last = editor->textEditor()->document()->find(QStringLiteral("body."));
        QVERIFY(!last.isNull());
        const QRect line = editor->textEditor()->cursorRect(last);
        const int bodyBottom = editor->textEditor()->viewport()->mapTo(form, line.bottomLeft()).y();
        const int checklistGap = rectIn(checklist, form).top() - bodyBottom;
        QVERIFY2(checklistGap >= 0 && checklistGap <= 64,
                 qPrintable(QStringLiteral("Checklist starts %1 px after the short body").arg(checklistGap)));
        const int attachmentGap = rectIn(attachments, form).top() - rectIn(checklist, form).bottom();
        QVERIFY2(attachmentGap >= 0 && attachmentGap <= 64,
                 qPrintable(QStringLiteral("Attachments start %1 px after the checklist").arg(attachmentGap)));
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QVERIFY(scroll->viewport()->rect().contains(rectIn(row, scroll->viewport())));

        const QString empty = m_store->createTask(QStringLiteral("没有子项"));
        Entity shortTask = m_store->entity(empty); shortTask.body = task.body; QVERIFY(m_store->saveTask(shortTask));
        QVERIFY(detail.setTask(empty));
        QTRY_VERIFY(checklist->isHidden()); QTRY_VERIFY(attachments->isHidden()); QVERIFY(input->isHidden());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QVERIFY(editor->height() < 100);
        QVERIFY2(form->layout()->sizeHint().height() <= title->height() + editor->height() + 80,
                 "Empty checklist/attachment sections must not reserve layout space");

        // Removing the final managed file collapses its slot immediately, without reopening the entity.
        QVERIFY(detail.setTask(id)); QTRY_VERIFY(attachments->isVisible());
        QVERIFY(m_store->removeAttachment(id, attachment.id));
        QTRY_VERIFY(attachments->isHidden());
        QCOMPARE(m_store->entity(id).checklist.first().id, checklist->item(0)->data(Qt::UserRole).toString());
    }

    void detailChecklistCommandsRevealInputAndGrowWithoutInnerScroll() {
        const QString id = m_store->createTask(QStringLiteral("按需添加清单"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(checklist && input && editor);
        QVERIFY(checklist->isHidden()); QVERIFY(input->isHidden());
        QPushButton* legacyAdd = nullptr;
        for (QPushButton* button : detail.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加")) legacyAdd = button;
        QVERIFY(legacyAdd); QVERIFY(legacyAdd->isHidden());
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("AddChecklist")));
        QTRY_VERIFY(input->isVisible()); QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, QStringLiteral("First item")); QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(checklist->count(), 1); QTRY_VERIFY(checklist->isVisible()); QVERIFY(detail.flush());
        const QString firstId = m_store->entity(id).checklist.first().id; QVERIFY(isUuid(firstId));
        QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), firstId);
        const int oneRowHeight = checklist->height();
        for (int row = 2; row <= 12; ++row) {
            input->setText(QStringLiteral("Item %1").arg(row)); QTest::keyClick(input, Qt::Key_Return);
        }
        QVERIFY(detail.flush()); QTRY_COMPARE(checklist->count(), 12);
        QTRY_VERIFY(checklist->height() > oneRowHeight * 5);
        QTRY_COMPARE(checklist->verticalScrollBar()->maximum(), 0);
        for (int row = 0; row < checklist->count(); ++row) {
            const QRect itemRect = checklist->visualItemRect(checklist->item(row));
            QVERIFY2(checklist->viewport()->rect().contains(itemRect), "Every checklist row must fit its own viewport");
            QVERIFY(isUuid(checklist->item(row)->data(Qt::UserRole).toString()));
        }
        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).checklist.first().id, firstId);
        QVERIFY(m_store->entity(id).checklist.first().done);
        int doneEvents = 0;
        for (const EntityEvent& event : m_store->events(id))
            if (event.field == QStringLiteral("checklist.%1.done").arg(firstId)) ++doneEvents;
        QCOMPARE(doneEvents, 1);

        const QString other = m_store->createTask(QStringLiteral("工具栏添加清单"));
        QVERIFY(detail.setTask(other)); QVERIFY(input->isHidden());
        QSignalSpy requested(editor, &MarkdownEditor::checklistRequested);
        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
        QTest::mouseClick(format, Qt::LeftButton);
        auto* panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        auto* addChecklist = panel->findChild<QToolButton*>(QStringLiteral("FormatChecklist")); QVERIFY(addChecklist);
        QTest::mouseClick(addChecklist, Qt::LeftButton);
        QCOMPARE(requested.count(), 1); QTRY_VERIFY(input->isVisible()); QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, QStringLiteral("Toolbar item")); QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(other).checklist.size(), 1);
        QCOMPARE(m_store->entity(other).checklist.first().text, QStringLiteral("Toolbar item"));
        QVERIFY(m_store->entity(other).body.isEmpty()); // The command adds a real checklist item, not Markdown syntax.
    }

    void detailHeaderCommandsPersistWithoutSteadySaveLabel() {
        const QString id = m_store->createTask(QStringLiteral("详情菜单操作"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* completed = detail.findChild<QCheckBox*>(QStringLiteral("TaskCompleted"));
        auto* priority = detail.findChild<QComboBox*>(QStringLiteral("TaskPriority"));
        QVERIFY(header && date && info && title && completed && priority);
        QVERIFY(!header->isAncestorOf(title)); QVERIFY(header->isAncestorOf(date)); QVERIFY(header->isAncestorOf(info));
        QVERIFY(completed->isHidden()); QVERIFY(priority->isHidden());
        QVERIFY(date->isVisible()); QVERIFY(info->isVisible());
        QVERIFY(rectIn(info, header).left() > rectIn(date, header).right());
        for (QPushButton* button : header->findChildren<QPushButton*>())
            if (button->isVisible()) QCOMPARE(button, date);
        for (QToolButton* button : header->findChildren<QToolButton*>())
            if (button->isVisible() && button != info && button->objectName() != QLatin1String("DetailClose"))
                QVERIFY(button->accessibleName().contains(QStringLiteral("提醒")) || button->toolTip().contains(QStringLiteral("提醒")));
        for (QLineEdit* field : header->findChildren<QLineEdit*>()) QVERIFY(!field->isVisible());
        for (QComboBox* field : header->findChildren<QComboBox*>()) QVERIFY(!field->isVisible());
        for (QCheckBox* field : header->findChildren<QCheckBox*>()) QVERIFY(!field->isVisible());

        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Menu keeps this title"));
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdPriority3")));
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Menu keeps this title"));
        QCOMPARE(m_store->entity(id).priority, 3);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdToggleCompleted")));
        QVERIFY(m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdToggleCompleted")));
        QVERIFY(!m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdPriority0")));
        QCOMPARE(m_store->entity(id).priority, 0); QVERIFY(detail.flush());
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus"));
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), QStringLiteral("Menu keeps this title"));
        QVERIFY(completed->isHidden()); QVERIFY(priority->isHidden());
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
    }

    void detailDateChipUsesShanghaiDateAndActiveStatus() {
        const QDate today = toAppTime(appNow()).date();
        const QDate overdue = today.addDays(-3);
        const QString id = m_store->createTask(QStringLiteral("逾期日期展示"));
        Entity task = m_store->entity(id);
        task.dueAt = fromAppTime(QDateTime(overdue, QTime(0, 30), Qt::LocalTime));
        task.allDayDate = today.addDays(5); // An explicit due time takes precedence over an all-day fallback.
        QVERIFY(m_store->saveTask(task));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(date);
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("00:30")));
        QVERIFY(date->text().contains(QStringLiteral("延期3天")));
        QCOMPARE(date->accessibleName(), date->text());
        QVERIFY(m_store->setCompleted(id, true));
        QTRY_VERIFY(!date->text().contains(QStringLiteral("延期")));
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(m_store->setStatus(id, TaskStatus::Abandoned));
        QTRY_VERIFY(!date->text().contains(QStringLiteral("延期")));
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(m_store->setStatus(id, TaskStatus::Active));
        task = m_store->entity(id); task.dueAt = QDateTime(); task.allDayDate = overdue;
        task.startAt = fromAppTime(QDateTime(today.addDays(5), QTime(11, 45), Qt::LocalTime));
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("延期3天")));
        QVERIFY(!date->text().contains(QStringLiteral("11:45")));
        task = m_store->entity(id); task.startAt = QDateTime();
        task.dueAt = fromAppTime(QDateTime(today.addDays(2), QTime(8, 30), Qt::LocalTime));
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(today.addDays(2).toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("08:30"))); QVERIFY(!date->text().contains(QStringLiteral("延期")));
    }

    void detailLongDocumentUsesOneScrollAtNarrowWidth() {
        const QString id = m_store->createTask(QStringLiteral("标题与长正文一起滚动"));
        Entity task = m_store->entity(id);
        QStringList paragraphs;
        for (int line = 0; line < 60; ++line)
            paragraphs.append(QStringLiteral("Paragraph %1: a long line that wraps at narrow widths and remains editable.").arg(line));
        task.body = paragraphs.join(QLatin1Char('\n'));
        ChecklistItem item; item.text = QStringLiteral("Last checklist item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        TaskDetailWidget detail(m_store.get()); detail.resize(360, 560); detail.show(); QVERIFY(detail.setTask(id));
        QTest::qWait(20);
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        QVERIFY(scroll && form && title && editor && header && footer && attachments);
        QTRY_VERIFY(detail.width() <= 380);
        QVERIFY(form->isAncestorOf(title)); QVERIFY(form->isAncestorOf(editor)); QVERIFY(editor->flowLayout());
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > scroll->viewport()->height());
        QCOMPARE(editor->textEditor()->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(editor->textEditor()->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(editor->contentHeight() > scroll->viewport()->height());
        scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
        const QPoint titleBefore = title->mapTo(scroll->viewport(), QPoint());
        const QPoint editorBefore = editor->mapTo(scroll->viewport(), QPoint());
        const QRect headerBefore = header->geometry(), footerBefore = footer->geometry();
        scroll->verticalScrollBar()->setValue(180); QApplication::processEvents();
        QCOMPARE(title->mapTo(scroll->viewport(), QPoint()).y(), titleBefore.y() - 180);
        QCOMPARE(editor->mapTo(scroll->viewport(), QPoint()).y(), editorBefore.y() - 180);
        QVERIFY(rectIn(title, scroll->viewport()).bottom() < 0);
        QCOMPARE(header->geometry(), headerBefore); QCOMPARE(footer->geometry(), footerBefore);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QApplication::processEvents();
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QVERIFY(scroll->viewport()->rect().contains(rectIn(row, scroll->viewport())));
        QCOMPARE(editor->content(), task.body);

        QSignalSpy heightChanges(editor, &MarkdownEditor::contentHeightChanged);
        editor->textEditor()->setPlainText(QStringLiteral("Short again."));
        QTRY_VERIFY(heightChanges.count() > 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QVERIFY(editor->contentHeight() < 100); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Short again."));
    }

    void detailInfoPopupShowsRenderedLiveStatisticsAndEntityDates() {
        const QString id = m_store->createTask(QStringLiteral("Two words"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("**bold** <u>under</u> <mark data-color=\"yellow\">lit</mark> ")
            + QString::fromUtf8("\xF0\x9F\x98\x80");
        ChecklistItem item; item.text = QStringLiteral("check item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        // An existing entity with distinct dates also exercises Shanghai's UTC day boundary.
        const QDateTime created(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        const QDateTime updated(QDate(2026, 11, 8), QTime(16, 15), Qt::UTC);
        QSqlQuery dates(Database::instance().connection());
        dates.prepare(QStringLiteral("UPDATE entities SET created_at_utc_ms=?,updated_at_utc_ms=? WHERE id=?"));
        dates.addBindValue(created.toMSecsSinceEpoch()); dates.addBindValue(updated.toMSecsSinceEpoch()); dates.addBindValue(id);
        QVERIFY(dates.exec()); QVERIFY(m_store->load());
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
        QTest::mouseClick(info, Qt::LeftButton);
        auto* popup = detail.findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup")); QVERIFY(popup);
        QTRY_VERIFY(popup->isVisible()); QVERIFY(popup->windowFlags().testFlag(Qt::Popup));
        auto* words = popup->findChild<QLabel*>(QStringLiteral("DetailWordCount"));
        auto* characters = popup->findChild<QLabel*>(QStringLiteral("DetailCharacterCount"));
        auto* createdLabel = popup->findChild<QLabel*>(QStringLiteral("DetailCreatedAt"));
        auto* updatedLabel = popup->findChild<QLabel*>(QStringLiteral("DetailUpdatedAt"));
        QVERIFY(words && characters && createdLabel && updatedLabel);
        QCOMPARE(words->text(), QStringLiteral("7"));
        QCOMPARE(characters->text(), QStringLiteral("35")); // Includes spaces and one non-BMP codepoint, no markup/newlines/filename.
        QCOMPARE(createdLabel->text(), QStringLiteral("2026年11月6日"));
        QCOMPARE(updatedLabel->text(), QStringLiteral("2026年11月9日"));
        QCOMPARE(createdLabel->toolTip(), toAppTime(created).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        QCOMPARE(updatedLabel->toolTip(), toAppTime(updated).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        popup->hide();

        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* body = detail.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor")); QVERIFY(title && body);
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Only"));
        body->setFocus(); body->setPlainText(QStringLiteral("**changed** text"));
        QTest::mouseClick(info, Qt::LeftButton); QTRY_VERIFY(popup->isVisible());
        QCOMPARE(words->text(), QStringLiteral("5")); QCOMPARE(characters->text(), QStringLiteral("26"));
        popup->hide(); QVERIFY(detail.finishEditing());
        const Entity saved = m_store->entity(id);
        QCOMPARE(saved.title, QStringLiteral("Only")); QCOMPARE(saved.body, QStringLiteral("**changed** text"));
        QTest::mouseClick(info, Qt::LeftButton); QTRY_VERIFY(popup->isVisible());
        QCOMPARE(createdLabel->text(), QStringLiteral("2026年11月6日"));
        QCOMPARE(updatedLabel->toolTip(), toAppTime(saved.updatedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        popup->hide();
    }

    void nativePopupWindowsDisableSystemShadow() {
        const QString id = m_store->createTask(QStringLiteral("浮层阴影检查"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 640); detail.show();
        QVERIFY(detail.setTask(id)); QApplication::processEvents();
        const auto verify = [](QWidget* popup) {
            // 自有浮层显式声明标志；真实 Windows 窗口再核对系统样式。
            QVERIFY(popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
            QVERIFY(nativePopupShadowDisabled(popup));
        };
        const auto verifyNativeOnly = [](QWidget* popup) {
            // Qt 内部菜单不声明 NoDropShadowWindowHint，仅由应用级过滤器清除系统样式。
            QVERIFY(nativePopupShadowDisabled(popup));
        };

        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
        QTest::mouseClick(info, Qt::LeftButton);
        auto* infoPopup = detail.findChild<EntityInfoPopup*>(); QVERIFY(infoPopup);
        QTRY_VERIFY(infoPopup->isVisible()); verify(infoPopup); infoPopup->hide();

        auto* group = detail.findChild<QToolButton*>(QStringLiteral("GroupChip")); QVERIFY(group);
        QTest::mouseClick(group, Qt::LeftButton);
        auto* listPopup = detail.findChild<ListSelectPopup*>(); QVERIFY(listPopup);
        QTRY_VERIFY(listPopup->isVisible()); verify(listPopup); listPopup->hide();

        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
        QTest::mouseClick(format, Qt::LeftButton);
        auto* formatPanel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(formatPanel);
        QTRY_VERIFY(formatPanel->isVisible()); verify(formatPanel); formatPanel->hide();

        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(date);
        QTest::mouseClick(date, Qt::LeftButton);
        DatePickerPopup* datePopup = nullptr;
        QTRY_VERIFY((datePopup = detail.findChild<DatePickerPopup*>()) != nullptr);
        QTRY_VERIFY(datePopup->isVisible()); verify(datePopup); datePopup->reject();

        // 菜单、下拉列表等 Qt 内部弹出窗口也走同一应用级拦截。
        QMenu menu;
        menu.addAction(QStringLiteral("菜单阴影检查"));
        menu.popup(detail.mapToGlobal(QPoint(24, 24)));
        QTRY_VERIFY(menu.isVisible()); verifyNativeOnly(&menu); menu.hide();
    }

    void detailListSearchMovesByUuidAndPreservesEditingSession() {
        const QString sourceFolder = m_store->createFolder(QStringLiteral("SourceFolder"));
        const QString targetFolder = m_store->createFolder(QStringLiteral("TargetFolder"));
        const QString sourceList = m_store->createList(QStringLiteral("同名清单"), sourceFolder);
        const QString targetList = m_store->createList(QStringLiteral("同名清单"), targetFolder);
        const QString section = m_store->createSection(sourceList, QStringLiteral("原分段"));
        const QString id = m_store->createTask(QStringLiteral("待移动任务"), sourceList, section);
        QVERIFY(isUuid(id)); QVERIFY(isUuid(section));
        Entity task = m_store->entity(id); task.body = QStringLiteral("Original body");
        ChecklistItem item; item.text = QStringLiteral("Keep my identity"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString itemId = m_store->entity(id).checklist.first().id; QVERIFY(isUuid(itemId));
        const auto bodyEvents = [this, &id]() {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == QStringLiteral("body")) result.append(event);
            return result;
        };
        const QVector<EntityEvent> baselineEvents = bodyEvents(); QVERIFY(!baselineEvents.isEmpty());
        QCOMPARE(baselineEvents.last().newValue.toString(), task.body);
        const int beforeBodyEvents = baselineEvents.size();
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* chip = detail.findChild<QToolButton*>(QStringLiteral("GroupChip"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(title && editor && chip && checklist);
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Pending move title"));
        auto* body = editor->textEditor(); body->setFocus(); QApplication::processEvents(); QVERIFY(body->hasFocus());
        const QString pendingBody = QStringLiteral("**Pending body**\nAll lines must survive moving lists.");
        body->setPlainText(pendingBody);
        QTest::mouseClick(chip, Qt::LeftButton);
        auto* popup = detail.findChild<ListSelectPopup*>(QStringLiteral("GroupSelectMenu")); QVERIFY(popup);
        QTRY_VERIFY(popup->isVisible());
        auto* search = popup->findChild<QLineEdit*>(QStringLiteral("ListSearch"));
        auto* tree = popup->findChild<QTreeWidget*>(QStringLiteral("ListSelectTree")); QVERIFY(search && tree);
        QTreeWidgetItem *sourceItem = nullptr, *targetItem = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const QString listId = (*it)->data(0, Qt::UserRole).toString();
            if (listId == sourceList) sourceItem = *it;
            if (listId == targetList) targetItem = *it;
        }
        QVERIFY(sourceItem && targetItem); QCOMPARE(sourceItem->text(0), targetItem->text(0));
        QVERIFY(tree->currentItem()); QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), sourceList);
        search->setText(QStringLiteral("no matching list")); QTRY_VERIFY(!tree->isVisible());
        QCOMPARE(m_store->entity(id).listId, sourceList); QCOMPARE(m_store->entity(id).sectionId, section);
        search->clear(); QTest::keyClicks(search, QStringLiteral("TargetFolder"));
        QTRY_VERIFY(tree->isVisible()); QVERIFY(sourceItem->parent()->isHidden());
        QVERIFY(!targetItem->isHidden()); QVERIFY(!targetItem->parent()->isHidden()); QVERIFY(targetItem->parent()->isExpanded());
        tree->scrollToItem(targetItem); QApplication::processEvents();
        const QRect targetRect = tree->visualItemRect(targetItem); QVERIFY(!targetRect.isEmpty());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, targetRect.center());
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(m_store->entity(id).listId, targetList); QVERIFY(m_store->entity(id).sectionId.isEmpty());
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Pending move title"));
        QCOMPARE(m_store->entity(id).body, pendingBody); QCOMPARE(editor->content(), pendingBody);
        QCOMPARE(m_store->entity(id).checklist.first().id, itemId);
        QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), itemId);
        const QVector<EntityEvent> edits = bodyEvents(); QCOMPARE(edits.size(), beforeBodyEvents + 1);
        // Text history stores a full new snapshot; the edit-session baseline is used only to detect a change.
        QVERIFY(!edits.last().oldValue.isValid()); QCOMPARE(edits.last().newValue.toString(), pendingBody);
        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.finishEditing());
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        const Entity moved = reloaded.entity(id);
        QCOMPARE(moved.listId, targetList); QVERIFY(moved.sectionId.isEmpty());
        QCOMPARE(moved.title, QStringLiteral("Pending move title")); QCOMPARE(moved.body, pendingBody);
        QCOMPARE(moved.checklist.size(), 1); QCOMPARE(moved.checklist.first().id, itemId); QVERIFY(moved.checklist.first().done);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), moved.title); QCOMPARE(editor->content(), pendingBody);
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
            "## 本周要点\n\n整理 **完成情况**，<u>记录需要协调的事项</u>。\n\n"
            "- 汇总本周进展\n- 确认下周安排\n\n"
            "> <mark data-color=\"yellow\">把下一步写清楚</mark>，让事情更容易推进。");
        ChecklistItem one;
        one.text = QStringLiteral("汇总材料");
        one.done = true;
        task.checklist << one;
        one.text = QStringLiteral("核对时间表");
        one.done = false;
        task.checklist << one;
        task.tags << QStringLiteral("本周") << QStringLiteral("协作");
        QVERIFY(m_store->saveTask(task));
        const QString attachmentSource = referenceImage(m_dir->path()); QVERIFY(!attachmentSource.isEmpty());
        QVERIFY(!m_store->importAttachment(id, attachmentSource).id.isEmpty());
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
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail);
        auto* attachments = detail->findChild<AttachmentWidget*>(); QVERIFY(attachments);
        QTRY_VERIFY(attachments->isVisible());
        auto* attachmentIcon = attachments->findChild<QLabel*>(QStringLiteral("AttachmentIcon")); QVERIFY(attachmentIcon);
        auto* attachmentName = attachments->findChild<QLabel*>(QStringLiteral("AttachmentName")); QVERIFY(attachmentName);
        QVERIFY(attachmentIcon->isVisible()); QVERIFY(attachmentName->isVisible());
        QCOMPARE(attachmentName->text(), QStringLiteral("reference.png"));
        const QString dir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QVERIFY(QDir().mkpath(dir));
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel.png")));
            QImage divider = panel.grab().toImage().copy(
                rectIn(panel.findChild<QFrame*>(QStringLiteral("ListPane")), &panel).right() - 12, 16, 40, 100);
            QVERIFY(divider.scaled(240, 600, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(dir + QStringLiteral("/workspace-divider-detail.png")));
            QImage corner = panel.grab().toImage().copy(panel.width() - 190, 0, 190, 150);
            QImage onLight(corner.size(), QImage::Format_ARGB32_Premultiplied);
            onLight.fill(QColor(QStringLiteral("#E9E9E9")));
            QPainter cornerPainter(&onLight);
            cornerPainter.drawImage(0, 0, corner);
            cornerPainter.end();
            QVERIFY(corner.save(dir + QStringLiteral("/window-corner-detail.png")));
            QImage bottom = panel.grab().toImage().copy(panel.width() - 190, panel.height() - 150, 190, 150);
            QImage bottomOnLight(bottom.size(), QImage::Format_ARGB32_Premultiplied);
            bottomOnLight.fill(QColor(QStringLiteral("#E9E9E9")));
            QPainter bottomPainter(&bottomOnLight);
            bottomPainter.drawImage(0, 0, bottom);
            bottomPainter.end();
            QVERIFY(bottomOnLight.scaled(760, 600, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(dir + QStringLiteral("/window-bottom-corner-detail.png")));
            QVERIFY(m_store->setCompleted(id, true));
            auto* toast = panel.findChild<ToastIsland*>(QStringLiteral("WorkspaceToast"));
            QVERIFY(toast);
            QTRY_VERIFY(toast->isVisible());
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-toast.png")));
            QVERIFY(m_store->setStatus(id, TaskStatus::Active));
            toast->hide();
            panel.showTask();
            QTest::qWait(30);
            auto* closeDetail = detail->findChild<QToolButton*>(QStringLiteral("DetailClose"));
            QVERIFY(closeDetail);
            QTest::mouseClick(closeDetail, Qt::LeftButton);
            QTRY_VERIFY(panel.findChild<QFrame*>(QStringLiteral("DetailFrame"))->isVisible());
            QVERIFY(panel.findChild<TaskDetailWidget*>()->entityId().isEmpty());
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-three-columns.png")));
            panel.showTask(id);
            QTest::qWait(30);
            QVERIFY(detail->grab().save(dir + QStringLiteral("/task-detail.png")));
            panel.resize(900, 700);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-auto-three-columns.png")));
            panel.showTask(id);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-detail-overlay.png")));
            panel.resize(700, 620);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-auto-two-columns.png")));
            panel.resize(1240, 820);
            QTest::qWait(50);
            NoteWidget note(m_store->entity(id)); note.setStore(m_store.get());
            note.show();
            QTest::qWait(50);
            QVERIFY(note.grab().save(dir + QStringLiteral("/desktop-note.png")));
            note.hide();
            panel.activateWindow(); panel.raise(); QApplication::processEvents();

            auto* info = detail->findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
            QTest::mouseClick(info, Qt::LeftButton);
            auto* infoPopup = detail->findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup")); QVERIFY(infoPopup);
            QTRY_VERIFY(infoPopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(infoPopup));
            QVERIFY(savePopupPreview(detail, infoPopup, dir + QStringLiteral("/detail-info.png")));
            infoPopup->hide();
            auto* format = detail->findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
            QTest::mouseClick(format, Qt::LeftButton);
            auto* formatPanel = detail->findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(formatPanel);
            QTRY_VERIFY(formatPanel->isVisible());
            QVERIFY(nativePopupShadowDisabled(formatPanel));
            QVERIFY(savePopupPreview(detail, formatPanel, dir + QStringLiteral("/detail-toolbar.png")));
            formatPanel->hide();
            auto* group = detail->findChild<QToolButton*>(QStringLiteral("GroupChip")); QVERIFY(group);
            QTest::mouseClick(group, Qt::LeftButton);
            auto* groupPopup = detail->findChild<ListSelectPopup*>(); QVERIFY(groupPopup);
            QTRY_VERIFY(groupPopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(groupPopup));
            QVERIFY(savePopupPreview(detail, groupPopup, dir + QStringLiteral("/detail-list.png")));
            groupPopup->hide();
            // 原生 Windows 上菜单窗口也必须没有系统直角投影。
            QMenu shadowProbe;
            shadowProbe.addAction(QStringLiteral("菜单阴影检查"));
            shadowProbe.popup(detail->mapToGlobal(QPoint(24, 24)));
            QTRY_VERIFY(shadowProbe.isVisible());
            QVERIFY(nativePopupShadowDisabled(&shadowProbe));
            shadowProbe.hide();

            TaskDetailWidget narrow(m_store.get()); narrow.resize(360, 700); QVERIFY(narrow.setTask(id)); narrow.show();
            auto* narrowTitle = narrow.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(narrowTitle);
            narrow.activateWindow(); narrowTitle->setFocus(); narrowTitle->deselect();
            QTest::qWait(50); QVERIFY(narrow.width() <= 380);
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow.png")));
            Entity longTask = m_store->entity(id);
            for (int line = 1; line <= 35; ++line)
                longTask.body += QStringLiteral("\n\n### 第 %1 段\n长文档与标题共用外层滚动区域，检查项和参考附件始终接在正文之后。").arg(line);
            QVERIFY(m_store->saveTask(longTask));
            auto* scroll = narrow.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
            auto* editor = narrow.findChild<MarkdownEditor*>(); QVERIFY(scroll && editor);
            QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
            QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
            QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow-long.png")));
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QApplication::processEvents();
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow-bottom.png")));
        }
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};
QTEST_MAIN(UiTest)
#include "tst_ui.moc"
